// The chimera PPSSPP driver. See psp-driver.h. Modeled on headless/Headless.cpp
// (the System_* surface, the config block) and libretro/libretro.cpp (the
// frame-stepping loop); both are part of PPSSPP, GPL-2.0-or-later, like this file.
#include "psp-driver.h"

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
		return true;
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

	PinCpuInfo(cfg.threads);
	g_threadManager.Init(cfg.threads, cfg.threads);

	g_display.Recalculate(480, 272, 1.0f, 1.0f, 1.0f);

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
	g_Config.iTimeFormat = PSP_SYSTEMPARAM_TIME_FORMAT_24HR;
	g_Config.bEncryptSave = true;
	g_Config.sNickName = cfg.nickName;
	g_Config.iTimeZone = 0;
	g_Config.iDateFormat = PSP_SYSTEMPARAM_DATE_FORMAT_YYYYMMDD;
	g_Config.iButtonPreference = cfg.buttonPreference;
	g_Config.iLockParentalLevel = 9;
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
	g_Config.sMACAddress = "12:34:56:78:9A:BC";
	g_Config.iFirmwareVersion = PSP_DEFAULT_FIRMWARE;
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
	g_Config.bFuncReplacements = true;

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

	ReadbackVideo();
	g_audioFrame.swap(g_audioAccum);
	g_audioAccum.clear();
}

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
