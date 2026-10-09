// UPnP port forwarding stub for the waterbox build: the sandbox has no network,
// so every operation reports the un-initialized/failed state upstream code
// already handles. Replaces extern/ppsspp/Core/Util/PortManager.cpp.
#include "Core/Util/PortManager.h"

PortManager g_PortManager;

bool PortManager::Initialize(unsigned int timeout) { return false; }
bool PortManager::Add(const char *protocol, unsigned short port, unsigned short intport, const std::string &desc) { return false; }
bool PortManager::Remove(const char *protocol, unsigned short port) { return false; }
void PortManager::Shutdown(double budgetSeconds) {}
bool PortManager::RefreshPortList() { return false; }
bool PortManager::Clear() { return false; }
bool PortManager::Restore() { return false; }
void PortManager::Terminate() {}
bool PortManager::HaveControlURL() const { return false; }
bool PortManager::OutOfTime() const { return true; }

void __UPnPInit(unsigned int timeout_ms) {}
void __UPnPShutdown() {}

void UPnP_Add(const char *protocol, unsigned short port, unsigned short intport) {}
void UPnP_Remove(const char *protocol, unsigned short port) {}
void UPnP_Notify() {}
