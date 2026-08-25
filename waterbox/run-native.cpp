// Host driver over psp-driver: boots a PSP executable/image, runs frames,
// reports per-frame video/audio digests (the gate format), and can run a
// pspautotests test and compare against its .expected file.
//
// usage: run-native <file> [--frames N] [--autotest] [--verbose]
//                   [--assets DIR] [--memstick DIR] [--dump-video PREFIX]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

#include "memory-assets.h"
#include "psp-driver.h"
#include "ram-filesystem.h"

// One line of a jaffar .sol movie: "||  lx,  ly,UDLRSsQTCXlr|" where the 12
// letter positions are Up Down Left Right Start Select Square Triangle Circle
// Cross LTrigger RTrigger (the old BizHawk PSP port's order) and a '.' means
// released. Returns the chimera packed-input word.
static bool parseSolLine(const char *line, uint64_t *packed) {
	if (line[0] != '|' || line[1] != '|')
		return false;
	int lx = 0, ly = 0, n = 0;
	if (sscanf(line + 2, " %d , %d ,%n", &lx, &ly, &n) < 2 || n == 0)
		return false;
	const char *b = line + 2 + n;
	// .sol order -> packed bit (Up..Right 0..3, Cross 4, Circle 5, Square 6,
	// Triangle 7, Start 8, Select 9, L 10, R 11)
	static const int kBit[12] = { 0, 1, 2, 3, 8, 9, 6, 7, 5, 4, 10, 11 };
	uint64_t p = 0;
	for (int i = 0; i < 12 && b[i] && b[i] != '|'; i++)
		if (b[i] != '.')
			p |= 1ull << kBit[i];
	if (lx < -128) lx = -128;
	if (lx > 127) lx = 127;
	if (ly < -128) ly = -128;
	if (ly > 127) ly = 127;
	p |= ((uint64_t)(uint8_t)(lx + 128)) << 16;
	p |= ((uint64_t)(uint8_t)(ly + 128)) << 24;
	*packed = p;
	return true;
}

static std::vector<uint64_t> loadMovie(const char *path) {
	std::vector<uint64_t> frames;
	FILE *f = fopen(path, "rb");
	if (!f)
		return frames;
	char line[256];
	while (fgets(line, sizeof line, f)) {
		uint64_t p;
		if (parseSolLine(line, &p))
			frames.push_back(p);
	}
	fclose(f);
	return frames;
}

static uint64_t fnv1a(const void *data, size_t len, uint64_t h = 1469598103934665603ULL) {
	const uint8_t *p = (const uint8_t *)data;
	for (size_t i = 0; i < len; i++) {
		h ^= p[i];
		h *= 1099511628211ULL;
	}
	return h;
}

// mkdir -p for a path's parent directories (export names are relative and
// clean by construction, so walking forward is safe)
static void makeParentDirs(const std::string &path) {
	for (size_t i = 1; i < path.size(); i++) {
		if (path[i] != '/')
			continue;
		std::string dir = path.substr(0, i);
#ifdef _WIN32
		_mkdir(dir.c_str());
#else
		mkdir(dir.c_str(), 0777);
#endif
	}
}

// --savedata-out: the memstick tree, written file by file - what the frontend
// zips through the savedata group, flattened for the gate to diff.
static bool exportSaveData(const char *dir) {
	int32_t n = Chimera_MemstickExportCount();
	for (int32_t i = 0; i < n; i++) {
		std::string path = std::string(dir) + "/" + Chimera_MemstickExportName(i);
		makeParentDirs(path);
		FILE *f = fopen(path.c_str(), "wb");
		if (!f) {
			fprintf(stderr, "could not write %s\n", path.c_str());
			return false;
		}
		int64_t size = Chimera_MemstickExportSize(i);
		bool ok = size == 0 || fwrite(Chimera_MemstickExportData(i), 1, (size_t)size, f) == (size_t)size;
		fclose(f);
		if (!ok) {
			fprintf(stderr, "could not write %s\n", path.c_str());
			return false;
		}
	}
	printf("savedata=%d\n", n);
	return true;
}

static bool writeTga(const char *path, const uint32_t *bgra, int w, int h) {
	FILE *f = fopen(path, "wb");
	if (!f)
		return false;
	uint8_t hdr[18] = {0};
	hdr[2] = 2;
	hdr[12] = w & 0xff; hdr[13] = w >> 8;
	hdr[14] = h & 0xff; hdr[15] = h >> 8;
	hdr[16] = 32;
	hdr[17] = 0x20;  // top-left origin
	fwrite(hdr, 1, 18, f);
	fwrite(bgra, 4, (size_t)w * h, f);
	fclose(f);
	return true;
}

int main(int argc, char **argv) {
	const char *file = nullptr;
	const char *assets = nullptr;
	const char *memstick = nullptr;
	const char *root = nullptr;
	const char *dumpPrefix = nullptr;
	const char *fontDir = nullptr;
	const char *sliceOut = nullptr;
	const char *moviePath = nullptr;
	const char *savedataOut = nullptr;
	int pspModel = 1;
	int cpuCore = 1; // matches the declared default (waterbox.config: jit)
	const char *rtcBase = nullptr;
	std::vector<const char *> sets;
	unsigned long sliceOff = 0, sliceLen = 0;
	int frames = 60;
	bool autotest = false, verbose = false, gate = false;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--autotest")) autotest = true;
		else if (!strcmp(argv[i], "--gate")) gate = true;
		else if (!strcmp(argv[i], "--movie") && i + 1 < argc) moviePath = argv[++i];
		else if (!strcmp(argv[i], "--verbose")) verbose = true;
		else if (!strcmp(argv[i], "--assets") && i + 1 < argc) assets = argv[++i];
		else if (!strcmp(argv[i], "--memstick") && i + 1 < argc) memstick = argv[++i];
		else if (!strcmp(argv[i], "--root") && i + 1 < argc) root = argv[++i];
		else if (!strcmp(argv[i], "--psp-model") && i + 1 < argc) pspModel = strcmp(argv[++i], "psp-1000") == 0 ? 0 : 1;
		else if (!strcmp(argv[i], "--rtc-base") && i + 1 < argc) rtcBase = argv[++i];
		else if (!strcmp(argv[i], "--set") && i + 1 < argc) sets.push_back(argv[++i]);
		else if (!strcmp(argv[i], "--cpu") && i + 1 < argc) {
			++i;
			cpuCore = !strcmp(argv[i], "jit") ? 1 : !strcmp(argv[i], "interpreter") ? 0 : 2;
		}
		else if (!strcmp(argv[i], "--font-dir") && i + 1 < argc) fontDir = argv[++i];
		else if (!strcmp(argv[i], "--savedata-out") && i + 1 < argc) savedataOut = argv[++i];
		else if (!strcmp(argv[i], "--dump-video") && i + 1 < argc) dumpPrefix = argv[++i];
		else if (!strcmp(argv[i], "--ram-slice") && i + 3 < argc) {
			sliceOff = strtoul(argv[++i], nullptr, 0);
			sliceLen = strtoul(argv[++i], nullptr, 0);
			sliceOut = argv[++i];
		}
		else if (argv[i][0] != '-') file = argv[i];
		else { fprintf(stderr, "unknown arg %s\n", argv[i]); return 2; }
	}
	if (!file) {
		fprintf(stderr, "usage: run-native <file> [--frames N] [--autotest] [--verbose] [--assets DIR] [--memstick DIR] [--dump-video PREFIX]\n");
		return 2;
	}

	PspDrvConfig cfg;
	cfg.bootPath = file;
	cfg.assetsDir = assets ? assets : "";  // empty = embedded assets
	cfg.memstickDir = memstick ? memstick : "bin/memstick";
	cfg.verboseLog = verbose;
	cfg.mountRoot = root ? root : "";
	cfg.pspModel = pspModel;
	cfg.cpuCore = cpuCore;
	if (rtcBase) {
		int64_t t = pspdrv_parse_datetime(rtcBase);
		if (t < 0) { fprintf(stderr, "bad --rtc-base (want YYYY-MM-DD HH:MM:SS)\n"); return 2; }
		cfg.rtcBaseSeconds = t;
	}
	// User-provided system fonts, mirroring the firmware channel: any of the
	// registry's file names present in the directory shadows the bundled one.
	if (fontDir) {
		for (int i = 0; i < pspdrv_font_file_count; i++) {
			std::string path = std::string(fontDir) + "/" + pspdrv_font_files[i];
			if (FILE *f = fopen(path.c_str(), "rb")) {
				std::vector<uint8_t> bytes;
				uint8_t chunk[65536];
				size_t n;
				while ((n = fread(chunk, 1, sizeof chunk, f)) > 0)
					bytes.insert(bytes.end(), chunk, chunk + n);
				fclose(f);
				if (!bytes.empty())
					Chimera_AddFontOverride(pspdrv_font_files[i], bytes.data(), bytes.size());
			}
		}
	}

	for (const char *kv : sets) {
		std::string pair(kv);
		size_t eq = pair.find('=');
		if (eq == std::string::npos ||
		    !pspdrv_apply_setting(cfg, pair.substr(0, eq).c_str(), pair.c_str() + eq + 1)) {
			fprintf(stderr, "bad --set '%s'\n", kv);
			return 2;
		}
	}
	cfg.collectDebugOutput = autotest;

	std::vector<uint64_t> movie;
	if (moviePath) {
		movie = loadMovie(moviePath);
		if (movie.empty()) {
			fprintf(stderr, "no frames parsed from %s\n", moviePath);
			return 1;
		}
		if (frames == 60)
			frames = (int)movie.size();
	}

	std::string err;
	if (!pspdrv_boot(cfg, &err)) {
		fprintf(stderr, "boot failed: %s\n", err.c_str());
		return 1;
	}

	// --gate: the run-wbx protocol - the same per-frame input pattern, chained
	// video/audio digests, final memory-domain digests - so the two builds'
	// outputs diff directly.
	uint64_t gateV = 0, gateA = 0;
	PspDrvInput input;
	for (int i = 0; i < frames; i++) {
		if (moviePath) {
			uint64_t p = (size_t)i < movie.size() ? movie[i] : (0x80ull << 16) | (0x80ull << 24);
			input = pspdrv_input_from_packed(p);
		} else if (gate) {
			// run-wbx's padForFrame, verbatim
			uint64_t x = (uint64_t)i * 6364136223846793005ULL + 1442695040888963407ULL;
			x ^= x >> 33;
			input = pspdrv_input_from_packed((x & 0xFFF) | (((x >> 12) & 0xFF) << 16) | (((x >> 20) & 0xFF) << 24));
		}
		pspdrv_run_frame(input);

		int w, h, nsamp;
		const uint32_t *video = pspdrv_video(&w, &h);
		const int16_t *audio = pspdrv_audio(&nsamp);
		if (gate) {
			gateV = fnv1a(video, (size_t)w * h * 4, gateV ? gateV : 1469598103934665603ULL);
			gateA = fnv1a(audio, (size_t)nsamp * 4, gateA ? gateA : 1469598103934665603ULL);
		} else if (!autotest) {
			printf("frame=%d video=%016llx audio=%016llx samples=%d cycles=%llu\n",
			       i, (unsigned long long)fnv1a(video, (size_t)w * h * 4),
			       (unsigned long long)fnv1a(audio, (size_t)nsamp * 4), nsamp,
			       (unsigned long long)pspdrv_cycles());
		}
		if (dumpPrefix) {
			char path[1024];
			snprintf(path, sizeof path, "%s%05d.tga", dumpPrefix, i);
			writeTga(path, video, w, h);
		}
	}

	if (gate) {
		printf("frames=%d\n", frames);
		printf("videoHash=%016llx\n", (unsigned long long)gateV);
		printf("audioHash=%016llx\n", (unsigned long long)gateA);
		for (int i = 0; ; i++) {
			const char *dn; uint8_t *dd; uint32_t ds;
			if (!pspdrv_domain(i, &dn, &dd, &ds))
				break;
			printf("domain[%s]=%016llx\n", dn, (unsigned long long)fnv1a(dd, ds));
		}
	}

	if (savedataOut && !exportSaveData(savedataOut)) {
		pspdrv_shutdown();
		return 1;
	}

	if (sliceOut) {
		const char *dn; uint8_t *dd; uint32_t ds;
		if (pspdrv_domain(0, &dn, &dd, &ds) && sliceOff + sliceLen <= ds) {
			FILE *f = fopen(sliceOut, "wb");
			if (f) {
				fwrite(dd + sliceOff, 1, sliceLen, f);
				fclose(f);
			}
		}
	}

	if (autotest) {
		// The test prints its output through the debug channel; compare with
		// the .expected next to the file.
		std::string expectedPath = file;
		size_t dot = expectedPath.rfind('.');
		if (dot != std::string::npos)
			expectedPath = expectedPath.substr(0, dot) + ".expected";
		std::string expected;
		if (FILE *f = fopen(expectedPath.c_str(), "rb")) {
			char buf[4096];
			size_t n;
			while ((n = fread(buf, 1, sizeof buf, f)) > 0)
				expected.append(buf, n);
			fclose(f);
		}
		// Compare like test.py: per-line, trailing whitespace and trailing
		// blank lines ignored, CRLF tolerated.
		auto normalize = [](const std::string &in) {
			std::string out;
			size_t start = 0;
			while (start <= in.size()) {
				size_t nl = in.find('\n', start);
				std::string line = in.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
				while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
					line.pop_back();
				out += line;
				out += '\n';
				if (nl == std::string::npos)
					break;
				start = nl + 1;
			}
			while (out.size() >= 2 && out[out.size() - 1] == '\n' && out[out.size() - 2] == '\n')
				out.pop_back();
			return out;
		};
		const std::string got = normalize(pspdrv_debug_output());
		expected = expected.empty() ? expected : normalize(expected);
		if (!expected.empty() && got == expected) {
			printf("PASS %s\n", file);
		} else {
			printf("FAIL %s\n----got----\n%s\n----expected----\n%s\n", file, got.c_str(), expected.c_str());
			pspdrv_shutdown();
			return 1;
		}
	}

	pspdrv_shutdown();
	return 0;
}
