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
#include <vector>

#include <emulibc.h>
#include <waterbox_settings.h>
#include <waterbox_slots.h>

#include "memory-assets.h"
#include "psp-driver.h"
#include "ram-filesystem.h"

#include "Core/HLE/sceCtrl.h"

static char g_loadError[512];
static bool g_inited;

// Axes set by the frontend through the SetAxis export (index 0 = L-Stick X,
// 1 = L-Stick Y; positive Y = up, matching the config's axis declaration and
// the natural gamepad passthrough). Once the export has been used, it stays
// authoritative over the packed-input analog bytes - a driver uses one path
// or the other, never both, so this is deterministic.
static int32_t g_axis[2];
static bool g_axisMode;

/* Turbo. The host sets this to 0 when nobody is going to look at the frame.
 * ECL_INVISIBLE because it is the frontend's policy for the moment, not part of
 * the machine: a state saved while fast-forwarding must not put the machine
 * back into it when it is loaded to be looked at. The driver's own copy of the
 * flag is ordinary memory and therefore inside the savestate, which is why the
 * export writes both. */
ECL_INVISIBLE static int g_render = 1;

// Fixed output buffers, savestated as ordinary guest memory.
static uint32_t g_video[480 * 272];
static int16_t g_audio[8192 * 2];
static int g_audioFrames;

extern "C" {

ECL_EXPORT const char *GetLoadError(void) { return g_loadError; }

ECL_EXPORT int Init(void)
{
	g_loadError[0] = '\0';

	// The disc to boot. A chimera project mounts "slots" ({"disc":["name"]},
	// the file itself mounted under that canonical name - see file_slots.json
	// and chimera's docs/project.md); without it, the original rom filename
	// arrives as "rom.name" so extension based detection still works, the
	// bytes mounted under that name. Falls back to plain "rom".
	char romName[256] = "rom";
	if (!wbx_slot_first("disc", romName, sizeof romName)) {
		if (FILE *f = fopen("rom.name", "rb")) {
			size_t n = fread(romName, 1, sizeof romName - 1, f);
			while (n > 0 && (romName[n - 1] == '\n' || romName[n - 1] == '\r'))
				n--;
			romName[n] = '\0';
			fclose(f);
		}
	}

	PspDrvConfig cfg;
	cfg.bootPath = romName;
	cfg.assetsDir = "";     // empty = the assets compiled into this image
	cfg.memstickDir = "";   // the RAM memory stick, see ram-filesystem.cpp
	cfg.cpuCore = 2;        // IR interpreter
	cfg.threads = 2;
	cfg.verboseLog = false;
	cfg.collectDebugOutput = false;

	// Sync settings: everything that shapes the machine, read once from the
	// mounted "settings" channel (the package defaults overlaid with the
	// user's choices; movies record them). One shared applier
	// (pspdrv_apply_setting) serves this adapter and the native reference.
	{
		static const char *const kKeys[] = {
			"cpuCore", "pspModel", "language", "nickname", "buttonPreference",
			"rtcBase", "cpuClock", "ioTiming", "timeZone", "daylightSavings",
			"firmwareVersion", "macAddress", "dateFormat", "timeFormat",
			"parentalLevel", "funcReplacements", "encryptSave",
		};
		char buf[128];
		for (size_t i = 0; i < sizeof(kKeys) / sizeof(kKeys[0]); i++)
			if (wbx_setting_str(kKeys[i], buf, sizeof buf) >= 0)
				pspdrv_apply_setting(cfg, kKeys[i], buf);
	}

	// Real system fonts from the firmware channel: the frontend mounts each
	// provided file under its declared id (the flash0 font file name). One
	// open per mounted file is the VFS contract, so read each exactly once
	// and hand the bytes to the font overlay, which shadows the bundled
	// replacement of the same name.
	for (int i = 0; i < pspdrv_font_file_count; i++) {
		if (FILE *f = fopen(pspdrv_font_files[i], "rb")) {
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

	std::string err;
	if (!pspdrv_boot(cfg, &err)) {
		snprintf(g_loadError, sizeof g_loadError, "%s", err.c_str());
		printf("chimera-ppsspp: cannot boot: %s\n", err.c_str());
		return 0;
	}
	g_inited = true;
	return 1;
}

ECL_EXPORT void SetAxis(int32_t index, int32_t value)
{
	if (index >= 0 && index < 2) {
		if (value < -128) value = -128;
		if (value > 127) value = 127;
		g_axis[index] = value;
		g_axisMode = true;
	}
}

ECL_EXPORT void FrameAdvance(uint64_t input)
{
	if (!g_inited)
		return;

	PspDrvInput in = pspdrv_input_from_packed(input);
	if (g_axisMode) {
		in.leftX = (int8_t)g_axis[0];
		in.leftY = (int8_t)g_axis[1];
	}
	pspdrv_run_frame(in);

	if (g_render)
	{
		int w, h;
		const uint32_t *video = pspdrv_video(&w, &h);
		memcpy(g_video, video, sizeof g_video);
	}

	int frames;
	const int16_t *audio = pspdrv_audio(&frames);
	if (frames > 8192)
		frames = 8192;
	g_audioFrames = frames;
	memcpy(g_audio, audio, (size_t)frames * 2 * sizeof(int16_t));
}

/* Turbo (optional guest ABI group): while off the core produces no picture and
 * is otherwise exactly the machine it would have been. On a PSP that means the
 * readback and the conversion, and NOT the drawing: the GE draws into memory
 * the game can read back as a texture, so the drawing is part of the machine.
 * run-gate.sh's turbo leg is the proof - half a run undrawn leaves the same
 * machine, and the same pictures once drawing resumes. */
ECL_EXPORT void SetRenderingEnabled(int on)
{
	g_render = on != 0;
	pspdrv_set_rendering(g_render != 0);
}

ECL_EXPORT uint32_t *GetVideoBgra(void) { return g_video; }
ECL_EXPORT int16_t *GetAudio(void) { return g_audio; }
ECL_EXPORT int GetAudioSampleCount(void) { return g_audioFrames; }

// The PSP's refresh rate, as PPSSPP itself models it: sceDisplay sets
// timePerVblank = 1.001 / 60 seconds, so the machine runs at 60 / 1.001 =
// 59.94005994...Hz - the same NTSC ratio a PS2 and a Dreamcast run at, and the
// same one this core's siblings declare.
//
// It said 60000000/1001001 for a while, which is 59.94000006Hz: an extra digit
// in the denominator, about a millionth off. Nothing to watch, but it is the
// number a movie header records and the number a frontend turns into a running
// time, so it wants to be the machine's rather than nearly the machine's.
ECL_EXPORT int GetVsyncNumerator(void) { return 60000; }
ECL_EXPORT int GetVsyncDenominator(void) { return 1001; }

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

// ---- savedata export (the sixth optional guest ABI group) ----
// The memory stick is this core's save data (docs/save-data.md): the RAM
// memstick tree lives in guest memory, so savestates already carry it, and
// this group is the user's way OUT. The list is dynamic; the host snapshots
// via Count and reads the buffers directly at a frame boundary.

ECL_EXPORT int32_t GetSaveDataFileCount(void) { return Chimera_MemstickExportCount(); }
ECL_EXPORT const char *GetSaveDataFileName(int32_t i) { return Chimera_MemstickExportName(i); }
ECL_EXPORT int64_t GetSaveDataFileSize(int32_t i) { return Chimera_MemstickExportSize(i); }
ECL_EXPORT const uint8_t *GetSaveDataFileBuffer(int32_t i) { return Chimera_MemstickExportData(i); }

}  // extern "C"
