// Assets compiled into the guest image, served through PPSSPP's VFS.
#pragma once
#include <cstddef>
class VFSBackend;
VFSBackend *Chimera_CreateMemoryAssetReader();

// User-provided system fonts (the firmware channel): each override shadows
// one bundled flash0/font file. Add them all before boot; the overlay reader
// is registered ahead of the embedded assets, so its names win.
void Chimera_AddFontOverride(const char *fileName, const void *data, size_t len);
bool Chimera_HasFontOverrides();
VFSBackend *Chimera_CreateFontOverlayReader();
