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

// Parse "YYYY-MM-DD HH:MM:SS" into seconds since the Unix epoch with plain
// civil-date arithmetic (no libc time machinery - identical everywhere).
// Returns -1 if the string does not parse.
int64_t pspdrv_parse_datetime(const char *s);

// Decode the chimera packed-input u64 (buttons bits 0..11, analog bytes at
// 16/24 and 32/40) into a driver input. One definition for the guest adapter,
// run-native --gate, and run-wbx's documentation.
PspDrvInput pspdrv_input_from_packed(uint64_t packed);

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
