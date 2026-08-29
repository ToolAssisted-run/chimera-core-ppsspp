// The chimera PPSSPP driver. See psp-driver.h. Modeled on headless/Headless.cpp
// (the System_* surface, the config block) and libretro/libretro.cpp (the
// frame-stepping loop); both are part of PPSSPP, GPL-2.0-or-later, like this file.
#include "psp-driver.h"

#include <cctype>
#include <cstring>
#include <algorithm>
#include <vector>

#include "ppsspp_config.h"

#include "Common/CPUDetect.h"
#include "Common/Data/Convert/ColorConv.h"
#include "Common/File/VFS/VFS.h"
#include "Common/File/VFS/DirectoryReader.h"

#include "memory-assets.h"
#include "Common/File/FileUtil.h"
#include "Common/Log.h"
#include "Common/Log/LogManager.h"
#include "Common/System/Display.h"
#include "Common/System/NativeApp.h"
#include "Common/System/System.h"
#include "Common/System/Request.h"
#include "Common/Thread/ThreadManager.h"
#include "Common/TimeUtil.h"

#include "Core/Config.h"
#include "Core/ConfigValues.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/CoreParameter.h"
#include "Core/HLE/sceCtrl.h"
#include "Core/HLE/sceUtility.h"
#include "Core/MemMap.h"
#include "Core/MIPS/MIPS.h"
#include "Core/System.h"

#include "GPU/GPU.h"
#include "GPU/GPUCommon.h"
#include "GPU/Common/GPUDebugInterface.h"

// ---------------------------------------------------------------------------
// Driver state
// ---------------------------------------------------------------------------

static std::string g_debugOutput;
static bool g_collectDebugOutput;
static bool g_verboseLog;

static std::vector<uint32_t> g_videoBuf(480 * 272, 0xFF000000u);
static int g_videoW = 480, g_videoH = 272;
static bool g_render = true;

// Audio accumulated between frames (interleaved stereo s16 @ 44100).
static std::vector<int16_t> g_audioAccum;
static std::vector<int16_t> g_audioFrame;

static uint32_t g_prevButtons;

// ---------------------------------------------------------------------------
// The System_* surface (the "platform"). Everything is inert or routed to the
// driver; nothing may touch the host outside what the sandbox allows.
// ---------------------------------------------------------------------------

void NativeFrame(GraphicsContext *graphicsContext) {}
void NativeResized() {}

std::string System_GetProperty(SystemProperty prop) { return ""; }
std::vector<std::string> System_GetPropertyStringVec(SystemProperty prop) { return std::vector<std::string>(); }
int64_t System_GetPropertyInt(SystemProperty prop) {
	if (prop == SYSPROP_SYSTEMVERSION)
		return 31;
	return -1;
}
float System_GetPropertyFloat(SystemProperty prop) { return -1.0f; }
bool System_GetPropertyBool(SystemProperty prop) {
	switch (prop) {
	case SYSPROP_CAN_JIT:
		return true;
	case SYSPROP_SKIP_UI:
		// Headless.cpp says true to skip the PPGe atlas load; we must not. The
		// atlas is what draws the in-game savedata/utility dialogs, its texture
		// lives in kernel RAM (machine-shaping), and a real console renders
		// those dialogs. The atlas ships embedded, so loading it is free.
		return false;
	default:
		return false;
	}
}
void System_Notify(SystemNotification notification) {}
void System_PostUIMessage(UIMessage message, std::string_view param) {}
void System_RunOnMainThread(std::function<void()> func) {
	// There is no separate main thread here; run in place.
	if (func)
		func();
}
bool System_MakeRequest(SystemRequestType type, int requestId, const std::string &param1, const std::string &param2, int64_t param3, int64_t param4) {
	switch (type) {
	case SystemRequestType::SEND_DEBUG_OUTPUT:
		// NOTE: do not collect here; the collectDebugOutput pointer already
		// receives every chunk (sceIo appends to both paths).
		if (g_verboseLog)
			fwrite(param1.data(), 1, param1.size(), stderr);
		return true;
	default:
		return false;
	}
}
void System_AskForPermission(SystemPermission permission) {}
PermissionStatus System_GetPermissionStatus(SystemPermission permission) { return PERMISSION_STATUS_GRANTED; }
void System_AudioGetDebugStats(char *buf, size_t bufSize) { if (buf) buf[0] = '\0'; }
void System_AudioClear() { g_audioAccum.clear(); }
void System_AudioPushSamples(const int32_t *audio, int numSamples, float volume) {
	// numSamples is FRAMES of interleaved stereo s32 (sums of s16 sources).
	if (!audio)
		return;
	size_t old = g_audioAccum.size();
	g_audioAccum.resize(old + numSamples * 2);
	for (int i = 0; i < numSamples * 2; i++) {
		int32_t v = audio[i];
		if (v < -32768) v = -32768;
		if (v > 32767) v = 32767;
		g_audioAccum[old + i] = (int16_t)v;
	}
}

bool NativeSaveSecret(std::string_view nameOfSecret, std::string_view data) { return false; }
std::string NativeLoadSecret(std::string_view nameOfSecret) { return ""; }

#if PPSSPP_PLATFORM(ANDROID)
bool System_AudioRecordingIsAvailable() { return false; }
bool System_AudioRecordingState() { return false; }
#endif

// ---------------------------------------------------------------------------
// Determinism: the emulated machine must not depend on the host machine.
// ---------------------------------------------------------------------------

// Pin cpu_info to a fixed profile: a plain x86-64 with SSE4.2 and nothing
// newer. Codegen choices (vertex decoder jit, softgpu pixel/sampler jit) key
// off these flags, so leaving them at detected values would make the same
// core behave differently on different hosts. Applied in BOTH the native and
// the guest build, so the equivalence gate compares like against like.
static void PinCpuInfo(int threads) {
	cpu_info.num_cores = threads;
	cpu_info.logical_cpu_count = threads;
#if PPSSPP_ARCH(AMD64) || PPSSPP_ARCH(X86)
	cpu_info.bAVX = false;
	cpu_info.bAVX2 = false;
	cpu_info.bFMA3 = false;
	cpu_info.bFMA4 = false;
	cpu_info.bBMI1 = false;
	cpu_info.bBMI2 = false;
	cpu_info.bBMI2_fast = false;
	cpu_info.bLZCNT = false;
	cpu_info.bMOVBE = false;
	cpu_info.bXOP = false;
	cpu_info.bRTM = false;
	cpu_info.bAES = false;
	cpu_info.bSHA = false;
	cpu_info.bF16C = false;
	cpu_info.bSSE4A = false;
	cpu_info.bAtom = false;
	cpu_info.bSSE = true;
	cpu_info.bSSE2 = true;
	cpu_info.bSSE3 = true;
	cpu_info.bSSSE3 = true;
	cpu_info.bSSE4_1 = true;
	cpu_info.bSSE4_2 = true;
	cpu_info.bPOPCNT = true;
	cpu_info.bLAHFSAHF64 = true;
	cpu_info.bLongMode = true;
	cpu_info.bFXSR = true;
#endif
}

#ifdef CHIMERA_GUEST
extern "C" unsigned __default_stacksize;  // musl internal, one static link away
#endif

// Read by the CHIMERA_WATERBOX branch of __RtcInit (see patches/): the
// emulated RTC's base time. Set from the config before PSP_Init runs.
extern "C" int64_t Chimera_RtcBaseSeconds = 1495889068;

// Mirrors sceFont's fontRegistry (Core/HLE/sceFont.cpp): every flash0 font
// file the HLE can load. zh_gb.pgf ships with certain games rather than any
// console, but the registry reads it from flash0 too, so a user may supply it.
const char *const pspdrv_font_files[] = {
	"ltn0.pgf", "ltn1.pgf", "ltn2.pgf", "ltn3.pgf", "ltn4.pgf", "ltn5.pgf",
	"ltn6.pgf", "ltn7.pgf", "ltn8.pgf", "ltn9.pgf", "ltn10.pgf", "ltn11.pgf",
	"ltn12.pgf", "ltn13.pgf", "ltn14.pgf", "ltn15.pgf",
	"jpn0.pgf", "kr0.pgf", "zh_gb.pgf",
};
const int pspdrv_font_file_count = (int)(sizeof(pspdrv_font_files) / sizeof(pspdrv_font_files[0]));

static bool matchOption(const char *value, const char *const *options, int n, int *out) {
	for (int i = 0; i < n; i++)
		if (strcmp(value, options[i]) == 0) { *out = i; return true; }
	return false;
}

bool pspdrv_apply_setting(PspDrvConfig &cfg, const char *key, const char *value) {
	if (strcmp(key, "cpuCore") == 0) {
		if (strcmp(value, "jit") == 0) cfg.cpuCore = 1;
		else if (strcmp(value, "interpreter") == 0) cfg.cpuCore = 0;
		else if (strcmp(value, "ir-interpreter") == 0) cfg.cpuCore = 2;
		else return false;
		return true;
	}
	if (strcmp(key, "pspModel") == 0) {
		if (strcmp(value, "psp-1000") == 0) cfg.pspModel = 0;
		else if (strcmp(value, "psp-2000") == 0) cfg.pspModel = 1;
		else return false;
		return true;
	}
	if (strcmp(key, "language") == 0) {
		static const char *const kLangs[12] = {
			"japanese", "english", "french", "spanish", "german", "italian",
			"dutch", "portuguese", "russian", "korean",
			"chinese-traditional", "chinese-simplified",
		};
		return matchOption(value, kLangs, 12, &cfg.language);
	}
	if (strcmp(key, "nickname") == 0) {
		if (!value[0]) return false;
		cfg.nickName = value;
		return true;
	}
	if (strcmp(key, "buttonPreference") == 0) {
		if (strcmp(value, "circle") == 0) cfg.buttonPreference = 0;
		else if (strcmp(value, "cross") == 0) cfg.buttonPreference = 1;
		else return false;
		return true;
	}
	if (strcmp(key, "rtcBase") == 0) {
		int64_t t = pspdrv_parse_datetime(value);
		if (t < 0) return false;
		cfg.rtcBaseSeconds = t;
		return true;
	}
	if (strcmp(key, "cpuClock") == 0) {
		int mhz = atoi(value);
		if (mhz < 0 || mhz > 333) return false;
		cfg.lockedCpuSpeed = mhz;
		return true;
	}
	if (strcmp(key, "ioTiming") == 0) {
		// upstream's IOTIMING_HOST (1) reads the host clock and is
		// nondeterministic; it is deliberately not an option
		if (strcmp(value, "fast") == 0) cfg.ioTimingMethod = 0;
		else if (strcmp(value, "simulated") == 0) cfg.ioTimingMethod = 2;
		else if (strcmp(value, "umd-slow") == 0) cfg.ioTimingMethod = 3;
		else return false;
		return true;
	}
	if (strcmp(key, "timeZone") == 0) {
		int min = atoi(value);
		if (min < -720 || min > 840) return false;
		cfg.timeZoneMinutes = min;
		return true;
	}
	if (strcmp(key, "daylightSavings") == 0) {
		cfg.daylightSavings = strcmp(value, "true") == 0 || strcmp(value, "1") == 0;
		return true;
	}
	if (strcmp(key, "firmwareVersion") == 0) {
		int fw = atoi(value);
		if (fw < 100 || fw > 999) return false;
		cfg.firmwareVersion = fw;
		return true;
	}
	if (strcmp(key, "macAddress") == 0) {
		// XX:XX:XX:XX:XX:XX, hex
		if (strlen(value) != 17) return false;
		for (int i = 0; i < 17; i++) {
			if ((i % 3) == 2) {
				if (value[i] != ':') return false;
			} else if (!isxdigit((unsigned char)value[i])) {
				return false;
			}
		}
		cfg.macAddress = value;
		return true;
	}
	if (strcmp(key, "dateFormat") == 0) {
		static const char *const kFmts[3] = { "yyyy-mm-dd", "mm-dd-yyyy", "dd-mm-yyyy" };
		return matchOption(value, kFmts, 3, &cfg.dateFormat);
	}
	if (strcmp(key, "timeFormat") == 0) {
		static const char *const kFmts[2] = { "24h", "12h" };
		return matchOption(value, kFmts, 2, &cfg.timeFormat);
	}
	if (strcmp(key, "parentalLevel") == 0) {
		int lvl = atoi(value);
		if (lvl < 0 || lvl > 11) return false;
		cfg.parentalLevel = lvl;
		return true;
	}
	if (strcmp(key, "funcReplacements") == 0) {
		cfg.funcReplacements = strcmp(value, "false") != 0 && strcmp(value, "0") != 0;
		return true;
	}
	if (strcmp(key, "encryptSave") == 0) {
		cfg.encryptSave = strcmp(value, "false") != 0 && strcmp(value, "0") != 0;
		return true;
	}
	return false;
}

int64_t pspdrv_parse_datetime(const char *str) {
	int y, mo, d, h, mi, se;
	if (sscanf(str, "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6)
		return -1;
	if (mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 ||
	    mi < 0 || mi > 59 || se < 0 || se > 59 || y < 1970 || y > 9999)
		return -1;
	// days_from_civil (Howard Hinnant): proleptic Gregorian, era arithmetic
	int64_t yy = y - (mo <= 2 ? 1 : 0);
	int64_t era = (yy >= 0 ? yy : yy - 399) / 400;
	int64_t yoe = yy - era * 400;
	int64_t doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	int64_t days = era * 146097 + doe - 719468;
	return days * 86400 + h * 3600 + mi * 60 + se;
}

// ---------------------------------------------------------------------------
// Boot / shutdown
// ---------------------------------------------------------------------------

bool pspdrv_boot(const PspDrvConfig &cfg, std::string *error) {
	g_collectDebugOutput = cfg.collectDebugOutput;
	g_verboseLog = cfg.verboseLog;
	g_debugOutput.clear();

	g_Config.bEnableLogging = cfg.verboseLog;
	g_logManager.Init(&g_Config.bEnableLogging, false);
	for (int i = 0; i < (int)Log::NUMBER_OF_LOGS; i++) {
		g_logManager.SetEnabled((Log)i, cfg.verboseLog);
		g_logManager.SetLogLevel((Log)i, cfg.verboseLog ? LogLevel::LDEBUG : LogLevel::LERROR);
	}
	if (cfg.verboseLog)
		g_logManager.EnableOutput(LogOutput::Printf);

#ifdef CHIMERA_GUEST
	// musl's default THREAD stack is 128KB and the empty-auxv sandbox keeps it
	// from reading PT_GNU_STACK; PPSSPP's threads assume glibc-sized stacks.
	// One static link, so we can just set libc's internal default.
	__default_stacksize = 8u << 20;
#endif

	Chimera_RtcBaseSeconds = cfg.rtcBaseSeconds;

	PinCpuInfo(cfg.threads);
	g_threadManager.Init(cfg.threads, cfg.threads);

	g_display.Recalculate(480, 272, 1.0f, 1.0f, 1.0f);

	// Real system fonts the user provided (the firmware channel) go in FIRST:
	// the VFS answers from the first backend that has the name, so these
	// shadow the bundled flash0/font replacements file by file.
	if (Chimera_HasFontOverrides())
		g_VFS.Register("", Chimera_CreateFontOverlayReader());

	if (!cfg.assetsDir.empty())
		g_VFS.Register("", new DirectoryReader(Path(cfg.assetsDir)));
	else
		// The assets the machine needs (vfpu tables, flash0 fonts, PPGe atlas,
		// lang, compat databases) are compiled into the image, so the native
		// reference and the sandbox read EXACTLY the same bytes.
		g_VFS.Register("", Chimera_CreateMemoryAssetReader());

	g_Config.RestoreDefaults(RestoreSettingsBits::SETTINGS | RestoreSettingsBits::CONTROLS, true);

	// The headless/deterministic configuration. Mirrors headless/Headless.cpp,
	// with sound ON (the frontend wants it) and everything host-dependent or
	// nondeterministic forced off.
	g_Config.iCpuCore = cfg.cpuCore;
	g_Config.iDumpFileTypes = 0;
	g_Config.bEnableSound = true;
	g_Config.bFirstRun = false;
	g_Config.bIgnoreBadMemAccess = true;
	g_Config.sReportHost.clear();
	g_Config.bAutoSaveSymbolMap = false;
	g_Config.bCacheFullIsoInRam = false;
	g_Config.bSkipBufferEffects = false;
	g_Config.iSkipGPUReadbackMode = (int)SkipGPUReadbackMode::NO_SKIP;
	g_Config.bHardwareTransform = false;
	g_Config.iAnisotropyLevel = 0;
	g_Config.iMultiSampleLevel = 0;
	g_Config.iLanguage = cfg.language;
	g_Config.iTimeFormat = cfg.timeFormat;
	g_Config.bEncryptSave = cfg.encryptSave;
	g_Config.sNickName = cfg.nickName;
	g_Config.iTimeZone = cfg.timeZoneMinutes;
	g_Config.bDayLightSavings = cfg.daylightSavings;
	g_Config.iDateFormat = cfg.dateFormat;
	g_Config.iButtonPreference = cfg.buttonPreference;
	g_Config.iLockParentalLevel = cfg.parentalLevel;
	g_Config.iLockedCPUSpeed = cfg.lockedCpuSpeed;
	g_Config.iIOTimingMethod = cfg.ioTimingMethod;
	g_Config.iInternalResolution = 1;
	g_Config.bSoftwareSkinning = true;
	g_Config.bVertexDecoderJit = true;
	g_Config.bSoftwareRendering = true;
	g_Config.bSoftwareRenderingJit = true;
	g_Config.iSplineBezierQuality = 2;
	g_Config.bHighQualityDepth = true;
	g_Config.bMemStickInserted = true;
	g_Config.iMemStickSizeGB = 4;
	g_Config.bEnableWlan = false;
	g_Config.sMACAddress = cfg.macAddress;
	g_Config.iFirmwareVersion = cfg.firmwareVersion;
	g_Config.iPSPModel = cfg.pspModel;
	g_Config.iGameVolume = VOLUMEHI_FULL;
	g_Config.iReverbVolume = VOLUMEHI_FULL;
	g_Config.internalDataDirectory.clear();
	g_Config.bUseOldAtrac = false;
	g_Config.bEnableCheats = false;
	g_Config.bEnableUPnP = false;
	g_Config.bEnableNetworkChat = false;
	g_Config.bDiscordRichPresence = false;
	g_Config.bEnableStateUndo = false;
	g_Config.bFuncReplacements = cfg.funcReplacements;

	if (!cfg.assetsDir.empty())
		g_Config.flash0Directory = Path(cfg.assetsDir) / "flash0";
	if (!cfg.memstickDir.empty()) {
		g_Config.memStickDirectory = Path(cfg.memstickDir);
		File::CreateDir(g_Config.memStickDirectory);
		CreateSysDirectories();
	}

	CoreParameter coreParameter;
	coreParameter.cpuCore = (CPUCore)cfg.cpuCore;
	coreParameter.gpuCore = GPUCORE_SOFTWARE;
	coreParameter.graphicsContext = nullptr;
	coreParameter.enableSound = true;
	coreParameter.fileToStart = Path(cfg.bootPath);
	coreParameter.mountIso.clear();
	coreParameter.mountRoot = Path(cfg.mountRoot);
	coreParameter.startBreak = false;
	coreParameter.headLess = true;
	coreParameter.renderScaleFactor = 1;
	coreParameter.renderWidth = 480;
	coreParameter.renderHeight = 272;
	coreParameter.pixelWidth = 480;
	coreParameter.pixelHeight = 272;
	coreParameter.fastForward = true;
	if (g_collectDebugOutput)
		coreParameter.collectDebugOutput = &g_debugOutput;

	if (!PSP_InitStart(coreParameter)) {
		if (error)
			*error = "PSP_InitStart failed";
		return false;
	}
	std::string error_string;
	BootState state;
	while ((state = PSP_InitUpdate(&error_string)) == BootState::Booting) {
		sleep_ms(1, "boot-poll");
	}
	if (state != BootState::Complete) {
		if (error)
			*error = error_string.empty() ? "boot failed" : error_string;
		PSP_Shutdown(false);
		return false;
	}

	coreState = CORE_RUNNING_CPU;
	g_prevButtons = 0;
	g_audioAccum.clear();
	return true;
}

void pspdrv_shutdown() {
	PSP_Shutdown(true);
	g_threadManager.Teardown();
}

// ---------------------------------------------------------------------------
// Frame stepping
// ---------------------------------------------------------------------------

static void ReadbackVideo() {
	if (!gpuDebug)
		return;
	GPUDebugBuffer buf;
	if (!gpuDebug->GetCurrentFramebuffer(buf, GPU_DBG_FRAMEBUF_DISPLAY, -1)) {
		if (g_verboseLog)
			fprintf(stderr, "ReadbackVideo: GetCurrentFramebuffer FAILED\n");
		return;
	}
	if (g_verboseLog)
		fprintf(stderr, "ReadbackVideo: stride2=%d h=%d stride=%d fmt=%d flip=%d\n",
		        (int)buf.GetStride(), (int)buf.GetHeight(), buf.GetStride(), (int)buf.GetFormat(), buf.GetFlipped() ? 1 : 0);
	if (buf.GetStride() == 0 || buf.GetHeight() == 0)
		return;

	// Convert whatever came back into 480x272 BGRA8888. Rows may be flipped;
	// smaller buffers leave the remainder black. (Adapted from headless/
	// Compare.cpp, TranslateDebugBufferToCompare.)
	const u32 w = 480, h = 272;
	std::fill(g_videoBuf.begin(), g_videoBuf.end(), 0xFF000000u);
	const u32 safeW = std::min(w, buf.GetStride());
	const u32 safeH = std::min(h, buf.GetHeight());

	const u32 *pixels32 = (const u32 *)buf.GetData();
	const u16 *pixels16 = (const u16 *)buf.GetData();
	int srcStride = buf.GetStride();
	if (buf.GetFlipped()) {
		int toLastRow = srcStride * (safeH - 1);
		pixels32 += toLastRow;
		pixels16 += toLastRow;
		srcStride = -srcStride;
	}

	for (u32 y = 0; y < safeH; ++y) {
		u32 *dst = &g_videoBuf[y * w];
		switch (buf.GetFormat()) {
		case GPU_DBG_FORMAT_8888:
			ConvertRGBA8888ToBGRA8888(dst, pixels32, safeW);
			break;
		case GPU_DBG_FORMAT_8888_BGRA:
			memcpy(dst, pixels32, safeW * sizeof(u32));
			break;
		case GPU_DBG_FORMAT_565:
			ConvertRGB565ToBGRA8888(dst, pixels16, safeW);
			break;
		case GPU_DBG_FORMAT_5551:
			ConvertRGBA5551ToBGRA8888(dst, pixels16, safeW);
			break;
		case GPU_DBG_FORMAT_4444:
			ConvertRGBA4444ToBGRA8888(dst, pixels16, safeW);
			break;
		default:
			return;
		}
		pixels32 += srcStride;
		pixels16 += srcStride;
	}
	// Force alpha opaque: the PSP display ignores framebuffer alpha.
	for (u32 i = 0; i < w * h; i++)
		g_videoBuf[i] |= 0xFF000000u;
}

void pspdrv_run_frame(const PspDrvInput &input) {
	// Inputs, injected at the HLE level exactly like the libretro port.
	uint32_t changedDown = input.buttons & ~g_prevButtons;
	uint32_t changedUp = g_prevButtons & ~input.buttons;
	__CtrlUpdateButtons(changedDown, changedUp);
	g_prevButtons = input.buttons;
	__CtrlSetAnalogXY(CTRL_STICK_LEFT, input.leftX / 128.0f, input.leftY / 128.0f);
	__CtrlSetAnalogXY(CTRL_STICK_RIGHT, input.rightX / 128.0f, input.rightY / 128.0f);

	const DisplayLayoutConfig &layout = g_Config.GetDisplayLayoutConfig(g_display.GetDeviceOrientation());
	if (gpu)
		gpu->BeginHostFrame(layout);

	coreState = CORE_RUNNING_CPU;
	PSP_RunLoopWhileState();
	switch (coreState) {
	case CORE_NEXTFRAME:
	case CORE_POWERDOWN:
		coreState = CORE_RUNNING_CPU;
		break;
	default:
		break;
	}

	if (gpu)
		gpu->EndHostFrame();

	// Turbo: the frame happened, nobody is going to look at it. The readback
	// and the format conversion are the only things here that exist purely for
	// the outside world, so they are what turbo saves; the buffer keeps the
	// last frame that was read back.
	if (g_render)
		ReadbackVideo();
	g_audioFrame.swap(g_audioAccum);
	g_audioAccum.clear();
}

void pspdrv_set_rendering(bool on) { g_render = on; }

const uint32_t *pspdrv_video(int *width, int *height) {
	if (width) *width = g_videoW;
	if (height) *height = g_videoH;
	return g_videoBuf.data();
}

const int16_t *pspdrv_audio(int *frames) {
	if (frames) *frames = (int)(g_audioFrame.size() / 2);
	return g_audioFrame.data();
}

uint64_t pspdrv_cycles() {
	return (uint64_t)CoreTiming::GetTicks();
}

bool pspdrv_input_was_read() {
	// TODO(M4): instrument sceCtrl reads for real lag detection.
	return true;
}

const std::string &pspdrv_debug_output() {
	return g_debugOutput;
}

PspDrvInput pspdrv_input_from_packed(uint64_t packed) {
	static const uint32_t kButtonMap[12] = {
		CTRL_UP, CTRL_DOWN, CTRL_LEFT, CTRL_RIGHT,
		CTRL_CROSS, CTRL_CIRCLE, CTRL_SQUARE, CTRL_TRIANGLE,
		CTRL_START, CTRL_SELECT, CTRL_LTRIGGER, CTRL_RTRIGGER,
	};
	PspDrvInput in;
	for (int i = 0; i < 12; i++)
		if (packed & (1ull << i))
			in.buttons |= kButtonMap[i];
	int lx = (int)((packed >> 16) & 0xFF);
	int ly = (int)((packed >> 24) & 0xFF);
	int rx = (int)((packed >> 32) & 0xFF);
	int ry = (int)((packed >> 40) & 0xFF);
	if (lx == 0 && ly == 0) { lx = 128; ly = 128; }  // unmapped analog = centered
	if (rx == 0 && ry == 0) { rx = 128; ry = 128; }
	in.leftX = (int8_t)(lx - 128);
	in.leftY = (int8_t)(128 - ly);  // driver: positive = up (sceCtrl convention)
	in.rightX = (int8_t)(rx - 128);
	in.rightY = (int8_t)(128 - ry);
	return in;
}

bool pspdrv_domain(int i, const char **name, uint8_t **data, uint32_t *size) {
	static const struct { const char *name; uint32_t addr; uint32_t size; } kD[] = {
		{ "RAM", 0x08000000, 0 },
		{ "VRAM", 0x04000000, 0x00200000 },
		{ "Scratchpad", 0x00010000, 0x00004000 },
	};
	if (i < 0 || i > 2 || !Memory::base)
		return false;
	if (name) *name = kD[i].name;
	if (data) *data = Memory::GetPointerWriteUnchecked(kD[i].addr);
	if (size) *size = i == 0 ? Memory::g_MemorySize : kD[i].size;
	return true;
}
