// Stubs for subsystems the chimera build compiles out: VR, retro
// achievements, the websocket debugger, HW GPU backend probing, GL feature
// state, and shader translation. Each returns the inert answer the callers
// already handle.
#include <string>

#include "Common/GPU/OpenGL/GLFeatures.h"
#include "Common/GPU/ShaderTranslation.h"
#include "Common/VR/PPSSPPVR.h"
#include "Core/RetroAchievements.h"
#include "Core/Debugger/WebSocket.h"

// --- GL feature state (read by the shader generators; never initialized) ---
GLExtensions gl_extensions;
std::string g_all_gl_extensions;
std::string g_all_egl_extensions;

// --- Vulkan probing (Core/Config.cpp asks) ---
bool VulkanMayBeAvailable() { return false; }

// --- VR ---
bool IsVREnabled() { return false; }
bool IsBigScreenVRMode() { return false; }
bool IsFlatVRGame() { return false; }
bool IsGameVRScene() { return false; }
bool IsImmersiveVRMode() { return false; }

// --- shader translation (postshaders only; never used with softgpu) ---
bool TranslateShader(std::string *dst, ShaderLanguage destLang, const ShaderLanguageDesc &desc, TranslatedShaderMetadata *destMetadata, std::string src, ShaderLanguage srcLang, ShaderStage stage, std::string *errorMessage) {
	if (errorMessage)
		*errorMessage = "shader translation is compiled out of this build";
	return false;
}

// --- websocket debugger ---
void HandleDebuggerRequest(const http::ServerRequest &request) {}
void StopAllDebuggers() {}

// --- retro achievements ---
namespace Achievements {
bool HardcoreModeActive() { return false; }
void ChangeUMD(const Path &path, FileLoader *fileLoader) {}
void DoState(PointerWrap &p) {}
}  // namespace Achievements
