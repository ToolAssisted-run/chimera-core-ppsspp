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

/* One line of a jaffar .sol movie; must parse EXACTLY like run-native.cpp's
 * parseSolLine (the gate compares the two replays). */
static int parseSolLine(const char *line, uint64_t *packed)
{
	if (line[0] != '|' || line[1] != '|') return 0;
	int lx = 0, ly = 0, n = 0;
	if (sscanf(line + 2, " %d , %d ,%n", &lx, &ly, &n) < 2 || n == 0) return 0;
	const char *b = line + 2 + n;
	static const int kBit[12] = { 0, 1, 2, 3, 8, 9, 6, 7, 5, 4, 10, 11 };
	uint64_t p = 0;
	int i;
	for (i = 0; i < 12 && b[i] && b[i] != '|'; i++)
		if (b[i] != '.') p |= 1ull << kBit[i];
	if (lx < -128) lx = -128;
	if (lx > 127) lx = 127;
	if (ly < -128) ly = -128;
	if (ly > 127) ly = 127;
	p |= ((uint64_t)(uint8_t)(lx + 128)) << 16;
	p |= ((uint64_t)(uint8_t)(ly + 128)) << 24;
	*packed = p;
	return 1;
}

static uint64_t *loadMovie(const char *path, long *count)
{
	FILE *f = fopen(path, "rb");
	if (!f) return 0;
	uint64_t *frames = 0; long n = 0, cap = 0;
	char line[256];
	while (fgets(line, sizeof line, f)) {
		uint64_t p;
		if (!parseSolLine(line, &p)) continue;
		if (n == cap) { cap = cap ? cap * 2 : 1024; frames = realloc(frames, cap * sizeof *frames); }
		frames[n++] = p;
	}
	fclose(f);
	*count = n;
	return frames;
}

/* deterministic per-frame input, matching run-native's --pattern mode:
 * 12 button bits + centered sticks */
static uint64_t padForFrame(long frame)
{
	uint64_t x = (uint64_t)frame * 6364136223846793005ULL + 1442695040888963407ULL;
	x ^= x >> 33;
	/* buttons in the low 12 bits, a wandering left stick in the analog bytes */
	return (x & 0xFFF) | (((x >> 12) & 0xFF) << 16) | (((x >> 20) & 0xFF) << 24);
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
	const char *wbxPath = 0, *romPath = 0, *sramOut = 0, *sramIn = 0, *moviePath = 0, *settingsPath = 0;
	long frames = 60; int rerecord = 0, blank = 0, plainrom = 0, rewind = 0, axesViaExport = 0;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--rerecord")) rerecord = 1;
		else if (!strcmp(argv[i], "--rewind")) rewind = 1;
		else if (!strcmp(argv[i], "--plain-rom")) plainrom = 1;
		else if (!strcmp(argv[i], "--saveram-out") && i + 1 < argc) sramOut = argv[++i];
		else if (!strcmp(argv[i], "--saveram-in") && i + 1 < argc) sramIn = argv[++i];
		else if (!strcmp(argv[i], "--movie") && i + 1 < argc) moviePath = argv[++i];
		else if (!strcmp(argv[i], "--axes-via-export")) axesViaExport = 1;
		else if (!strcmp(argv[i], "--settings") && i + 1 < argc) settingsPath = argv[++i];
		else if (!strcmp(argv[i], "--blank")) blank = 1;
		else if (!wbxPath) wbxPath = argv[i];
		else if (!romPath) romPath = argv[i];
		else frames = strtol(argv[i], 0, 0);
	}
	if (!wbxPath || !romPath) { fprintf(stderr, "usage: run-wbx <core.wbx> <game> <frames> [--rerecord] [--blank]\n"); return 2; }

	uint64_t *movie = 0; long movieLen = 0;
	if (moviePath) {
		movie = loadMovie(moviePath, &movieLen);
		if (!movie || movieLen == 0) { fprintf(stderr, "no frames parsed from %s\n", moviePath); return 1; }
		if (frames == 60) frames = movieLen;
	}

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
	if (plainrom)
		snprintf(vfsname, sizeof vfsname, "rom");  /* the frontend's fixed mount name */
	else
		snprintf(vfsname, sizeof vfsname, "/%s", base);
	freader romr = { rf };
	wbx_mount_file(h, vfsname, file_read, (uintptr_t)&romr, false, &r);
	if (r.error_message[0]) { fprintf(stderr, "mount rom: %s\n", r.error_message); return 1; }
	fclose(rf);
	if (!plainrom) {
		memreader nr = { (const uint8_t *)vfsname, strlen(vfsname), 0 };
		wbx_mount_file(h, "rom.name", mem_reader, (uintptr_t)&nr, false, &r);
		if (r.error_message[0]) { fprintf(stderr, "mount rom.name: %s\n", r.error_message); return 1; }
	}

	/* the frontend always mounts "settings" (empty object when nothing was
	 * changed); mirror it so a sync setting can be exercised without one */
	long settingsLen = 0;
	uint8_t *settings = 0;
	if (settingsPath) {
		FILE *sf = fopen(settingsPath, "rb");
		if (!sf) { fprintf(stderr, "cannot read %s\n", settingsPath); return 1; }
		fseek(sf, 0, SEEK_END); settingsLen = ftell(sf); fseek(sf, 0, SEEK_SET);
		settings = malloc(settingsLen ? settingsLen : 1);
		if (fread(settings, 1, settingsLen, sf) != (size_t)settingsLen) { fprintf(stderr, "short read on %s\n", settingsPath); return 1; }
		fclose(sf);
	}
	memreader setr = { settings, (size_t)settingsLen, 0 };
	wbx_mount_file(h, "settings", mem_reader, (uintptr_t)&setr, false, &r);
	if (r.error_message[0]) { fprintf(stderr, "mount settings: %s\n", r.error_message); return 1; }

	wbx_activate_host(h, &r);
	intfn Init = (intfn)proc(h, "Init");
	if (Init() != 1) {
		ptrfn GetLoadError = (ptrfn)proc(h, "GetLoadError");
		fprintf(stderr, "Init failed: %s\n", (const char *)GetLoadError());
		return 1;
	}

	if (sramIn) {
		FILE *f = fopen(sramIn, "rb");
		if (!f) { fprintf(stderr, "cannot read %s\n", sramIn); return 1; }
		fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
		ptrfn_i GetPersistentBuffer = (ptrfn_i)proc(h, "GetPersistentBuffer");
		intfn_i PutPersistent = (intfn_i)proc(h, "PutPersistent");
		void *dst = (void *)GetPersistentBuffer((int)n);
		if (!dst || fread(dst, 1, n, f) != (size_t)n) { fprintf(stderr, "saveram-in failed\n"); return 1; }
		fclose(f);
		if (!PutPersistent((int)n)) { fprintf(stderr, "core refused the save data\n"); return 1; }
	}

	framefn FrameAdvance = (framefn)proc(h, "FrameAdvance");
	/* --axes-via-export drives the stick through the SetAxis export exactly as
	 * the frontend does; the digests must match the packed-analog path, which
	 * is what the gate uses this flag to prove. */
	void (MB_GUEST_ABI *SetAxis)(int32_t, int32_t) = 0;
	if (axesViaExport)
		SetAxis = (void (MB_GUEST_ABI *)(int32_t, int32_t))proc(h, "SetAxis");
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

	/* --rewind: the TAS shape of savestates. Run the first half, save, run the
	 * second half recording its digests, load the mid-state and run the second
	 * half again: both passes must digest identically. */
	if (rewind) {
		long half = frames / 2;
		for (long f = 0; f < half; f++) {
			uint64_t in = movie ? (f < movieLen ? movie[f] : (0x80ull << 16) | (0x80ull << 24)) : blank ? (0x80ull << 16) | (0x80ull << 24) : padForFrame(f);
			if (SetAxis) { SetAxis(0, (int32_t)((in >> 16) & 0xFF) - 128); SetAxis(1, 128 - (int32_t)((in >> 24) & 0xFF)); }
			FrameAdvance(in);
		}
		st.len = 0;
		wbx_save_state(h, mem_write, (uintptr_t)&st, &r);
		if (r.error_message[0]) { fprintf(stderr, "rewind save: %s\n", r.error_message); return 1; }
		uint64_t v1 = 0, a1 = 0, v2 = 0, a2 = 0;
		for (long f = half; f < frames; f++) {
			uint64_t in = movie ? (f < movieLen ? movie[f] : (0x80ull << 16) | (0x80ull << 24)) : blank ? (0x80ull << 16) | (0x80ull << 24) : padForFrame(f);
			if (SetAxis) { SetAxis(0, (int32_t)((in >> 16) & 0xFF) - 128); SetAxis(1, 128 - (int32_t)((in >> 24) & 0xFF)); }
			FrameAdvance(in);
			v1 = fnv(v1, (const void *)GetVideoBgra(), 480 * 272 * 4);
			a1 = fnv(a1, (const void *)GetAudio(), (size_t)GetAudioSampleCount() * 4);
		}
		st.pos = 0;
		wbx_load_state(h, mem_read, (uintptr_t)&st, &r);
		if (r.error_message[0]) { fprintf(stderr, "rewind load: %s\n", r.error_message); return 1; }
		for (long f = half; f < frames; f++) {
			uint64_t in = movie ? (f < movieLen ? movie[f] : (0x80ull << 16) | (0x80ull << 24)) : blank ? (0x80ull << 16) | (0x80ull << 24) : padForFrame(f);
			if (SetAxis) { SetAxis(0, (int32_t)((in >> 16) & 0xFF) - 128); SetAxis(1, 128 - (int32_t)((in >> 24) & 0xFF)); }
			FrameAdvance(in);
			v2 = fnv(v2, (const void *)GetVideoBgra(), 480 * 272 * 4);
			a2 = fnv(a2, (const void *)GetAudio(), (size_t)GetAudioSampleCount() * 4);
		}
		printf("rewind=%s\n", (v1 == v2 && a1 == a2) ? "identical" : "DIVERGED");
		printf("videoHash=%016llx\nvideoHash2=%016llx\n", (unsigned long long)v1, (unsigned long long)v2);
		wbx_deactivate_host(h, &r); wbx_destroy_host(h, &r);
		free(st.b);
		return (v1 == v2 && a1 == a2) ? 0 : 1;
	}

	for (long f = 0; f < frames; f++) {
		if (rerecord) {
			st.len = 0;
			wbx_save_state(h, mem_write, (uintptr_t)&st, &r);
			st.pos = 0;
			wbx_load_state(h, mem_read, (uintptr_t)&st, &r);
			if (r.error_message[0]) { fprintf(stderr, "rerecord: %s\n", r.error_message); return 1; }
		}
		{
			uint64_t in = movie ? ((size_t)f < (size_t)movieLen ? movie[f] : (0x80ull << 16) | (0x80ull << 24)) : blank ? (0x80ull << 16) | (0x80ull << 24) : padForFrame(f);
			if (SetAxis) { SetAxis(0, (int32_t)((in >> 16) & 0xFF) - 128); SetAxis(1, 128 - (int32_t)((in >> 24) & 0xFF)); }
			FrameAdvance(in);
		}
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

	if (sramOut) {
		intfn PersistentSize = (intfn)proc(h, "GetPersistentSize");
		ptrfn GetPersistent = (ptrfn)proc(h, "GetPersistent");
		int n = PersistentSize();
		const void *src = n > 0 ? (const void *)GetPersistent() : 0;
		FILE *f = fopen(sramOut, "wb");
		if (!f) { fprintf(stderr, "cannot write %s\n", sramOut); return 1; }
		if (src) fwrite(src, 1, (size_t)n, f);
		fclose(f);
		printf("saveRamBytes=%d\n", src ? n : 0);
	}

	wbx_deactivate_host(h, &r); wbx_destroy_host(h, &r);
	free(st.b);
	return 0;
}
