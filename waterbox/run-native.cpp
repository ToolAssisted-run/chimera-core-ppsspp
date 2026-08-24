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

#include "psp-driver.h"

static uint64_t fnv1a(const void *data, size_t len, uint64_t h = 1469598103934665603ULL) {
	const uint8_t *p = (const uint8_t *)data;
	for (size_t i = 0; i < len; i++) {
		h ^= p[i];
		h *= 1099511628211ULL;
	}
	return h;
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
	int frames = 60;
	bool autotest = false, verbose = false;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--autotest")) autotest = true;
		else if (!strcmp(argv[i], "--verbose")) verbose = true;
		else if (!strcmp(argv[i], "--assets") && i + 1 < argc) assets = argv[++i];
		else if (!strcmp(argv[i], "--memstick") && i + 1 < argc) memstick = argv[++i];
		else if (!strcmp(argv[i], "--root") && i + 1 < argc) root = argv[++i];
		else if (!strcmp(argv[i], "--dump-video") && i + 1 < argc) dumpPrefix = argv[++i];
		else if (argv[i][0] != '-') file = argv[i];
		else { fprintf(stderr, "unknown arg %s\n", argv[i]); return 2; }
	}
	if (!file) {
		fprintf(stderr, "usage: run-native <file> [--frames N] [--autotest] [--verbose] [--assets DIR] [--memstick DIR] [--dump-video PREFIX]\n");
		return 2;
	}

	PspDrvConfig cfg;
	cfg.bootPath = file;
	cfg.assetsDir = assets ? assets : "../extern/ppsspp/assets";
	cfg.memstickDir = memstick ? memstick : "bin/memstick";
	cfg.verboseLog = verbose;
	cfg.mountRoot = root ? root : "";
	cfg.collectDebugOutput = autotest;

	std::string err;
	if (!pspdrv_boot(cfg, &err)) {
		fprintf(stderr, "boot failed: %s\n", err.c_str());
		return 1;
	}

	PspDrvInput input;
	for (int i = 0; i < frames; i++) {
		pspdrv_run_frame(input);

		int w, h, nsamp;
		const uint32_t *video = pspdrv_video(&w, &h);
		const int16_t *audio = pspdrv_audio(&nsamp);
		if (!autotest) {
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
