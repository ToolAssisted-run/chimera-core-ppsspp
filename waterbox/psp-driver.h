// The chimera PPSSPP driver: one frame-stepped, deterministic embedding of
// PPSSPP (softgpu + interpreter), shared by the native reference build
// (run-native.cpp) and the waterbox guest adapter (waterbox.cpp).
#pragma once

#include <cstdint>
#include <string>

struct PspDrvConfig {
	// What to boot: an .elf/.prx/EBOOT.PBP/.iso/.cso path (native build), or the
	// mounted VFS name (guest build).
	std::string bootPath;
	// Directory with PPSSPP's assets/ (fonts, PPGe atlas). Native only.
	std::string assetsDir;
	// Memory stick directory (native only; the guest uses the RAM filesystem).
	std::string memstickDir;
	// If set, mounted as host0:/umd0: (pspautotests convention).
	std::string mountRoot;
	// CPU core: 0 = interpreter, 1 = JIT, 2 = IR interpreter (matches CPUCore).
	int cpuCore = 2;
	// Fixed worker-thread count (determinism: never derive from the machine).
	int threads = 2;
	// PSP model: 0 = PSP-1000, 1 = Slim (more RAM).
	int pspModel = 1;
	// Emulated locale/config the machine boots with.
	int language = 1;  // PSP_SYSTEMPARAM_LANGUAGE_ENGLISH
	std::string nickName = "Chimera";
	// 0 = Circle confirms (Japanese consoles), 1 = Cross confirms.
	int buttonPreference = 1;  // PSP_SYSTEMPARAM_BUTTON_CROSS
	// The console's clock at power-on, seconds since the Unix epoch. The
	// default is the sandbox epoch (2017-05-27 12:44:28 UTC), matching the
	// constant clock the box reports. Emulated time advances from here with
	// emulation, never with the host.
	int64_t rtcBaseSeconds = 1495889068;
	// Locked CPU clock in MHz; 0 = unlocked (the game sets it, 222 default).
	int lockedCpuSpeed = 0;
	// I/O timing model: 0 fast, 2 simulated, 3 UMD-slow-simulated. (1 = host
	// timing exists upstream and is nondeterministic; never selectable here.)
	int ioTimingMethod = 0;
	// System parameters games read via sceUtility.
	int timeZoneMinutes = 0;
	bool daylightSavings = false;
	int firmwareVersion = 660;
	std::string macAddress = "12:34:56:78:9A:BC";
	int dateFormat = 0;   // 0 yyyy-mm-dd, 1 mm-dd-yyyy, 2 dd-mm-yyyy
	int timeFormat = 0;   // 0 24h, 1 12h
	int parentalLevel = 9;
	// HLE replacement of known functions (memcpy etc): faster, less
	// hardware-exact timing.
	bool funcReplacements = true;
	// Kirk-encrypt savedata like a real PSP.
	bool encryptSave = true;
	// Console-style logging to stderr for debugging.
	bool verboseLog = false;
	// Collect the emulated printf/debug output (pspautotests protocol).
	bool collectDebugOutput = false;
};

struct PspDrvInput {
	uint32_t buttons = 0;   // CTRL_* bitmask (sceCtrl encoding)
	// Sticks, -128..127 (0 = center). Right stick exists on no real PSP but
	// sceCtrl supports it (used by remasters/emulator extensions).
	int8_t leftX = 0, leftY = 0, rightX = 0, rightY = 0;
};

// Apply one named setting (the waterbox.config surface) to the config.
// Returns false for an unknown key or unparseable value; one implementation
// for the guest adapter and the native reference, so the two sides can never
// drift.
bool pspdrv_apply_setting(PspDrvConfig &cfg, const char *key, const char *value);

// Parse "YYYY-MM-DD HH:MM:SS" into seconds since the Unix epoch with plain
// civil-date arithmetic (no libc time machinery - identical everywhere).
// Returns -1 if the string does not parse.
int64_t pspdrv_parse_datetime(const char *s);

// Decode the chimera packed-input u64 (buttons bits 0..11, analog bytes at
// 16/24 and 32/40) into a driver input. One definition for the guest adapter,
// run-native --gate, and run-wbx's documentation.
PspDrvInput pspdrv_input_from_packed(uint64_t packed);

// The flash0 font files sceFont's registry can consume, in registry order.
// These are the ids the firmware channel declares (waterbox.config), the
// mounted names the guest adapter probes, and the file names run-native
// --font-dir looks for. A user-provided file shadows the bundled replacement
// of the same name (see Chimera_AddFontOverride in memory-assets.h).
extern const char *const pspdrv_font_files[];
extern const int pspdrv_font_file_count;

// Boots the machine. Returns false and fills *error on failure.
bool pspdrv_boot(const PspDrvConfig &config, std::string *error);
void pspdrv_shutdown();

// Runs exactly one video frame (one vblank-to-vblank slice).
void pspdrv_run_frame(const PspDrvInput &input);

// Video: BGRA8888, 480x272, valid until the next run_frame call.
const uint32_t *pspdrv_video(int *width, int *height);

// Audio accumulated during the last frame: interleaved stereo s16 @44100Hz.
const int16_t *pspdrv_audio(int *frames);

// Emulated CPU cycle counter (for tooling / debugging).
uint64_t pspdrv_cycles();

// Whether the game read the controller during the last frame (lag detection).
bool pspdrv_input_was_read();

// The debug output collected so far (only when collectDebugOutput).
const std::string &pspdrv_debug_output();

// Memory domains (valid after boot): 0=RAM, 1=VRAM, 2=Scratchpad.
// Returns false past the end.
bool pspdrv_domain(int i, const char **name, uint8_t **data, uint32_t *size);
