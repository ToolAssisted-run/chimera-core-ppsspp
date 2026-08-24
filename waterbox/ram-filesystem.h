// The memory stick as an in-memory filesystem. In the sandbox nothing may
// touch the host filesystem, and everything the machine can change must live
// in guest memory so whole-machine savestates capture it; savedata therefore
// goes into this tree. Compiled into BOTH builds so the equivalence gate
// compares like against like.
#pragma once

#include <memory>

class IFileSystem;
class IHandleAllocator;

std::shared_ptr<IFileSystem> Chimera_CreateRamMemstick(IHandleAllocator *hAlloc);

// The persistent-data channel: the whole memstick tree as one flat,
// deterministic byte stream (magic "ChimMS01"; entries sorted; see the .cpp).
// Size 0 means "nothing worth keeping" (no files beyond the standard dirs).
size_t Chimera_MemstickSerializeSize();
size_t Chimera_MemstickSerialize(unsigned char *out, size_t cap);
bool Chimera_MemstickDeserialize(const unsigned char *in, size_t len);
