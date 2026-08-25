// The chimera guest ABI layer: wraps psp-driver into the miniBox core ABI
// (the same export surface as chimera-core-quickernes/neshawk). Compiled ONLY
// for the guest; run-native.cpp is the native twin.
//
// Input packing (FrameAdvance's u64), must match run-wbx.c and the frontend
// keybind order in waterbox.config:
//   bit 0..11 : Up, Down, Left, Right, Cross, Circle, Square, Triangle,
//               Start, Select, L, R
//   bit 16..23: left stick X, biased u8 (0=left, 128=center, 255=right)
//   bit 24..31: left stick Y, biased u8 (0=up, 128=center, 255=down)
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include <emulibc.h>

#include "psp-driver.h"
#include "ram-filesystem.h"

#include "Core/HLE/sceCtrl.h"

static char g_loadError[512];
static bool g_inited;

// Fixed output buffers, savestated as ordinary guest memory.
static uint32_t g_video[480 * 272];
static int16_t g_audio[8192 * 2];
static int g_audioFrames;

extern "C" {

ECL_EXPORT const char *GetLoadError(void) { return g_loadError; }

ECL_EXPORT int Init(void)
{
	g_loadError[0] = '\0';

	// The original rom filename, mounted by the driver/frontend so extension
	// based detection still works; the rom bytes themselves are mounted under
	// that name. Falls back to plain "rom".
	char romName[256] = "rom";
	if (FILE *f = fopen("rom.name", "rb")) {
		size_t n = fread(romName, 1, sizeof romName - 1, f);
		while (n > 0 && (romName[n - 1] == '\n' || romName[n - 1] == '\r'))
			n--;
		romName[n] = '\0';
		fclose(f);
	}

	PspDrvConfig cfg;
	cfg.bootPath = romName;
	cfg.assetsDir = "";     // empty = the assets compiled into this image
	cfg.memstickDir = "";   // TODO(M4): RAM memory stick filesystem
	cfg.cpuCore = 2;        // IR interpreter
	cfg.threads = 2;
	cfg.verboseLog = false;
	cfg.collectDebugOutput = false;

	std::string err;
	if (!pspdrv_boot(cfg, &err)) {
		snprintf(g_loadError, sizeof g_loadError, "%s", err.c_str());
		printf("chimera-ppsspp: cannot boot: %s\n", err.c_str());
		return 0;
	}
	g_inited = true;
	return 1;
}

ECL_EXPORT void FrameAdvance(uint64_t input)
{
	if (!g_inited)
		return;

	pspdrv_run_frame(pspdrv_input_from_packed(input));

	int w, h;
	const uint32_t *video = pspdrv_video(&w, &h);
	memcpy(g_video, video, sizeof g_video);

	int frames;
	const int16_t *audio = pspdrv_audio(&frames);
	if (frames > 8192)
		frames = 8192;
	g_audioFrames = frames;
	memcpy(g_audio, audio, (size_t)frames * 2 * sizeof(int16_t));
}

ECL_EXPORT uint32_t *GetVideoBgra(void) { return g_video; }
ECL_EXPORT int16_t *GetAudio(void) { return g_audio; }
ECL_EXPORT int GetAudioSampleCount(void) { return g_audioFrames; }

// The PSP's exact refresh rate: 60 * (1.001)^-1... actually 59.9400599...Hz,
// which is 60000000/1001001 exactly (pixel clock derived).
ECL_EXPORT int GetVsyncNumerator(void) { return 60000000; }
ECL_EXPORT int GetVsyncDenominator(void) { return 1001001; }

ECL_EXPORT int InputWasRead(void) { return pspdrv_input_was_read() ? 1 : 0; }

// ---- memory domains ----
// Domains resolve lazily through the PSP memory map (base + masked offset).

ECL_EXPORT int GetMemoryDomainCount(void) { return 3; }
ECL_EXPORT const char *GetMemoryDomainName(int i)
{
	const char *name = nullptr;
	pspdrv_domain(i, &name, nullptr, nullptr);
	return name;
}
ECL_EXPORT uint8_t *GetMemoryDomainPtr(int i)
{
	uint8_t *data = nullptr;
	pspdrv_domain(i, nullptr, &data, nullptr);
	return data;
}
ECL_EXPORT int64_t GetMemoryDomainSize(int i)
{
	uint32_t size = 0;
	pspdrv_domain(i, nullptr, nullptr, &size);
	return size;
}
ECL_EXPORT int GetMemoryDomainWritable(int i) { return i >= 0 && i < 3; }

// ---- persistent data: the memory stick ----
// What the machine keeps when switched off. The frontend files it in the
// bundle under the id below and hands it back on the next load.

static uint8_t *g_persistentBuf;
static int g_persistentCap;

ECL_EXPORT int GetPersistentSize(void)
{
	return (int)Chimera_MemstickSerializeSize();
}

ECL_EXPORT uint8_t *GetPersistent(void)
{
	size_t n = Chimera_MemstickSerializeSize();
	if (n == 0)
		return nullptr;
	if ((int)n > g_persistentCap) {
		delete[] g_persistentBuf;
		g_persistentBuf = new uint8_t[n];
		g_persistentCap = (int)n;
	}
	Chimera_MemstickSerialize(g_persistentBuf, n);
	return g_persistentBuf;
}

ECL_EXPORT const char *GetPersistentName(void) { return "Memory Stick"; }
ECL_EXPORT const char *GetPersistentId(void) { return "memstick"; }

ECL_EXPORT uint8_t *GetPersistentBuffer(int size)
{
	if (size <= 0)
		return nullptr;
	if (size > g_persistentCap) {
		delete[] g_persistentBuf;
		g_persistentBuf = new uint8_t[size];
		g_persistentCap = size;
	}
	return g_persistentBuf;
}

ECL_EXPORT int PutPersistent(int length)
{
	if (length <= 0 || !g_persistentBuf)
		return 0;
	return Chimera_MemstickDeserialize(g_persistentBuf, (size_t)length) ? 1 : 0;
}

}  // extern "C"
