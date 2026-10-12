// The guest end of Chimera's Vulkan bridge, for PPSSPP's Vulkan renderer.
//
// The frontend hands the core one function (SetVulkanBridge, waterbox.cpp).
// miniBox's guest half (source/vk, linked into the core) turns every Vulkan
// command the backend makes into a call of it, and VulkanLoader.cpp asks that
// half for each command by name (patches: "the Vulkan renderer is Chimera's").
//
// A load of the machine is a new Vulkan device on the frontend's side: the
// old one is destroyed there, with everything made on it. The backend still
// has to let go of what it holds - handles, lists, memory - and it does that
// by destroying each object, as it would on a desktop. While it does, nothing
// crosses: every command answers "done" at once (the quiet switch), because
// the object it names is already gone and the frontend would refuse it.
#include <cstdint>
#include <cstdio>

extern "C" {
typedef uint64_t (*chimera_vk_bridge_fn)(uint64_t op, uint64_t a, uint64_t b, uint64_t c,
                                         uint64_t d, uint64_t e);
bool chimera_vk_install(chimera_vk_bridge_fn fn);
uint64_t chimera_vk_context_id(void);
void chimera_vk_forget_mappings(void);
}

static chimera_vk_bridge_fn s_host;
static bool s_installed;
static bool s_quiet;

// the opcodes below 100 are the bridge's own questions (which device is this),
// and are answered whatever the switch says
static uint64_t Bridge(uint64_t op, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e)
{
  if (s_quiet && op >= 100)
    return 0;
  return s_host(op, a, b, c, d, e);
}

extern "C" void chimera_psp_install_vulkan_bridge(uint64_t addr)
{
  s_host = reinterpret_cast<chimera_vk_bridge_fn>(static_cast<uintptr_t>(addr));
  s_installed = s_host && chimera_vk_install(Bridge);
  if (!s_installed)
    fprintf(stderr, "chimera: the Vulkan bridge was offered and refused\n");
}

extern "C" int chimera_psp_vulkan_bridge_present(void)
{
  return s_installed;
}

// which device the frontend has now; 0 when it cannot say
extern "C" uint64_t chimera_psp_vulkan_context(void)
{
  return s_installed ? chimera_vk_context_id() : 0;
}

// The backend is about to let go of a device a load destroyed...
extern "C" void chimera_psp_vulkan_quiet(void)
{
  s_quiet = true;
}

// ...and has: the copies of mapped memory nothing will unmap now are given
// back, and commands cross again for the device about to be made.
extern "C" void chimera_psp_vulkan_old_device_gone(void)
{
  if (!s_quiet)
    return;
  chimera_vk_forget_mappings();
  s_quiet = false;
}

// One name of the OpenGL layer that the shader translator asks for whichever
// renderer runs. There is no OpenGL in this core; it is only ever asked when
// the target is GLSL for OpenGL, which Vulkan's is not.
#include "Common/GPU/OpenGL/GLFeatures.h"
int GLExtensions::GLSLVersion()
{
  return 0;
}
