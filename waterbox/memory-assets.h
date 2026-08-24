// Assets compiled into the guest image, served through PPSSPP's VFS.
#pragma once
class VFSBackend;
VFSBackend *Chimera_CreateMemoryAssetReader();
