// UPnP port forwarding stub for the waterbox build: the sandbox has no network,
// so every operation reports the un-initialized/failed state upstream code
// already handles. Replaces extern/ppsspp/Core/Util/PortManager.cpp.
#include "Core/Util/PortManager.h"

PortManager g_PortManager;

PortManager::PortManager() {}
PortManager::~PortManager() {}

bool PortManager::Initialize(const unsigned int timeout) { return false; }
int PortManager::GetInitState() { return UPNP_INITSTATE_NONE; }
bool PortManager::Add(const char *protocol, unsigned short port, unsigned short intport) { return false; }
bool PortManager::Remove(const char *protocol, unsigned short port) { return false; }
void PortManager::Shutdown() {}
bool PortManager::RefreshPortList() { return false; }
bool PortManager::Clear() { return false; }
bool PortManager::Restore() { return false; }
void PortManager::Terminate() {}

void __UPnPInit(const int timeout_ms) {}
void __UPnPShutdown() {}

void UPnP_Add(const char *protocol, unsigned short port, unsigned short intport) {}
void UPnP_Remove(const char *protocol, unsigned short port) {}
