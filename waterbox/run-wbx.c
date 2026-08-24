/* Standalone driver for the waterboxed PPSSPP core: runs core.wbx through the
 * miniBox host over a PSP executable/image and reports per-frame video/audio/
 * RAM digests, so the sandboxed build can be compared against run-native on
 * the same inputs.
 *
 * usage: run-wbx <core.wbx> <game> <frames> [--rerecord] [--blank]
 *
 * --rerecord round-trips the whole guest machine through save/load state
 *   around every frame; the digests must be identical either way.
 */
#include "minibox.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
	const uint8_t *b = (const uint8_t *)p;
	if (!h) h = 1469598103934665603ULL;
	for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ULL; }
	return h;
}

typedef struct { FILE *f; } freader;
static intptr_t file_read(uintptr_t ud, uint8_t *d, uintptr_t s) { return (intptr_t)fread(d, 1, s, ((freader *)ud)->f); }
typedef struct { const uint8_t *p; size_t n, pos; } memreader;
static intptr_t mem_reader(uintptr_t ud, uint8_t *d, uintptr_t s)
{
	memreader *m = (memreader *)ud;
	size_t take = s < (m->n - m->pos) ? s : (m->n - m->pos);
	memcpy(d, m->p + m->pos, take); m->pos += take; return (intptr_t)take;
}
typedef struct { uint8_t *b; size_t len, cap, pos; } membuf;
static int32_t mem_write(uintptr_t ud, const uint8_t *d, uintptr_t n)
{
	membuf *m = (membuf *)ud;
	if (m->len + n > m->cap) { m->cap = (m->len + n) * 2 + 64; m->b = realloc(m->b, m->cap); }
	memcpy(m->b + m->len, d, n); m->len += n; return 0;
}
static intptr_t mem_read(uintptr_t ud, uint8_t *d, uintptr_t n)
{
	membuf *m = (membuf *)ud;
	uintptr_t avail = m->len - m->pos; if (n > avail) n = avail;
	memcpy(d, m->b + m->pos, n); m->pos += n; return (intptr_t)n;
}

/* deterministic per-frame input, matching run-native's --pattern mode:
 * 12 button bits + centered sticks */
static uint64_t padForFrame(long frame)
{
	uint64_t x = (uint64_t)frame * 6364136223846793005ULL + 1442695040888963407ULL;
	x ^= x >> 33;
	return (x & 0xFFF) | (0x80ull << 16) | (0x80ull << 24);
}

typedef int (MB_GUEST_ABI *intfn)(void);
typedef void (MB_GUEST_ABI *framefn)(uint64_t);
typedef uintptr_t (MB_GUEST_ABI *ptrfn)(void);
typedef uintptr_t (MB_GUEST_ABI *ptrfn_i)(int);
typedef int (MB_GUEST_ABI *intfn_i)(int);
typedef int64_t (MB_GUEST_ABI *i64fn_i)(int);

static uintptr_t proc(mb_host *h, const char *n)
{
	mb_return r; wbx_get_proc_addr(h, n, &r);
	if (r.error_message[0]) { fprintf(stderr, "proc %s: %s\n", n, r.error_message); exit(2); }
	if (!r.data) { fprintf(stderr, "missing required export %s\n", n); exit(2); }
	return r.data;
}

int main(int argc, char **argv)
{
	const char *wbxPath = 0, *romPath = 0;
	long frames = 60; int rerecord = 0, blank = 0;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--rerecord")) rerecord = 1;
		else if (!strcmp(argv[i], "--blank")) blank = 1;
		else if (!wbxPath) wbxPath = argv[i];
		else if (!romPath) romPath = argv[i];
		else frames = strtol(argv[i], 0, 0);
	}
	if (!wbxPath || !romPath) { fprintf(stderr, "usage: run-wbx <core.wbx> <game> <frames> [--rerecord] [--blank]\n"); return 2; }

	FILE *rf = fopen(romPath, "rb");
	if (!rf) { fprintf(stderr, "cannot open %s\n", romPath); return 1; }
	FILE *wf = fopen(wbxPath, "rb");
	if (!wf) { fprintf(stderr, "cannot open %s\n", wbxPath); return 1; }

	/* PPSSPP is a big machine: the PSP address space arena (256MiB calloc)
	 * plus PPSSPP's own heap live in sbrk/mmap. */
	mb_memory_layout_template layout = { 128u << 20, 16u << 20, 64u << 20, 64u << 20, 1024u << 20 };
	freader fr = { wf };
	mb_return r;
	wbx_create_host(&layout, "core.wbx", file_read, (uintptr_t)&fr, &r);
	fclose(wf);
	if (r.error_message[0]) { fprintf(stderr, "create: %s\n", r.error_message); return 1; }
	mb_host *h = (mb_host *)r.data;

	/* the rom under its original basename (extension drives type detection),
	 * plus rom.name so the guest knows what to boot */
	const char *base = strrchr(romPath, '/');
	base = base ? base + 1 : romPath;
	char vfsname[512];
	snprintf(vfsname, sizeof vfsname, "/%s", base);
	freader romr = { rf };
	wbx_mount_file(h, vfsname, file_read, (uintptr_t)&romr, false, &r);
	if (r.error_message[0]) { fprintf(stderr, "mount rom: %s\n", r.error_message); return 1; }
	fclose(rf);
	memreader nr = { (const uint8_t *)vfsname, strlen(vfsname), 0 };
	wbx_mount_file(h, "rom.name", mem_reader, (uintptr_t)&nr, false, &r);
	if (r.error_message[0]) { fprintf(stderr, "mount rom.name: %s\n", r.error_message); return 1; }

	wbx_activate_host(h, &r);
	intfn Init = (intfn)proc(h, "Init");
	if (Init() != 1) {
		ptrfn GetLoadError = (ptrfn)proc(h, "GetLoadError");
		fprintf(stderr, "Init failed: %s\n", (const char *)GetLoadError());
		return 1;
	}

	framefn FrameAdvance = (framefn)proc(h, "FrameAdvance");
	ptrfn GetVideoBgra = (ptrfn)proc(h, "GetVideoBgra");
	ptrfn GetAudio = (ptrfn)proc(h, "GetAudio");
	intfn GetAudioSampleCount = (intfn)proc(h, "GetAudioSampleCount");
	intfn GetMemoryDomainCount = (intfn)proc(h, "GetMemoryDomainCount");
	ptrfn_i GetMemoryDomainName = (ptrfn_i)proc(h, "GetMemoryDomainName");
	ptrfn_i GetMemoryDomainPtr = (ptrfn_i)proc(h, "GetMemoryDomainPtr");
	i64fn_i GetMemoryDomainSize = (i64fn_i)proc(h, "GetMemoryDomainSize");

	int nd = GetMemoryDomainCount();
	printf("domains=%d\n", nd);
	for (int i = 0; i < nd; i++)
		printf("  [%d] %-12s size=%lld\n", i, (const char *)GetMemoryDomainName(i),
		       (long long)GetMemoryDomainSize(i));

	wbx_deactivate_host(h, &r);
	wbx_seal(h, &r);
	if (r.error_message[0]) { fprintf(stderr, "seal: %s\n", r.error_message); return 1; }
	wbx_activate_host(h, &r);

	uint64_t vh = 0, ah = 0;
	membuf st = {0};
	for (long f = 0; f < frames; f++) {
		if (rerecord) {
			st.len = 0;
			wbx_save_state(h, mem_write, (uintptr_t)&st, &r);
			st.pos = 0;
			wbx_load_state(h, mem_read, (uintptr_t)&st, &r);
			if (r.error_message[0]) { fprintf(stderr, "rerecord: %s\n", r.error_message); return 1; }
		}
		FrameAdvance(blank ? (0x80ull << 16) | (0x80ull << 24) : padForFrame(f));
		vh = fnv(vh, (const void *)GetVideoBgra(), 480 * 272 * 4);
		ah = fnv(ah, (const void *)GetAudio(), (size_t)GetAudioSampleCount() * 4);
	}

	printf("frames=%ld\n", frames);
	printf("videoHash=%016llx\n", (unsigned long long)vh);
	printf("audioHash=%016llx\n", (unsigned long long)ah);
	for (int i = 0; i < nd; i++) {
		const char *dname = (const char *)GetMemoryDomainName(i);
		if (!dname) continue;
		uint64_t dh = fnv(0, (const void *)GetMemoryDomainPtr(i), (size_t)GetMemoryDomainSize(i));
		printf("domain[%s]=%016llx\n", dname, (unsigned long long)dh);
	}

	wbx_deactivate_host(h, &r); wbx_destroy_host(h, &r);
	free(st.b);
	return 0;
}
