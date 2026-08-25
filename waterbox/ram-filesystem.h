// The memory stick as an in-memory filesystem. In the sandbox nothing may
// touch the host filesystem, and everything the machine can change must live
// in guest memory so whole-machine savestates capture it; savedata therefore
// goes into this tree. Compiled into BOTH builds so the equivalence gate
// compares like against like.
#pragma once

#include <cstdint>
#include <memory>

class IFileSystem;
class IHandleAllocator;

std::shared_ptr<IFileSystem> Chimera_CreateRamMemstick(IHandleAllocator *hAlloc);

// Savedata export (chimera's docs/save-data.md): the memory stick IS this
// core's save data, and these walk it for the savedata guest ABI group in
// waterbox.cpp (and run-native's --savedata-out). Count() snapshots the node
// tree - the list is dynamic, a game creates files while it runs - and the
// accessors refer to the snapshot taken by the most recent Count() call.
int32_t Chimera_MemstickExportCount();
// Relative '/'-separated path ("PSP/SAVEDATA/.../DATA.BIN"), original case.
const char *Chimera_MemstickExportName(int32_t index);
int64_t Chimera_MemstickExportSize(int32_t index);
const uint8_t *Chimera_MemstickExportData(int32_t index);
