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
