// The memory stick as an in-memory filesystem. In the sandbox nothing may
// touch the host filesystem, and everything the machine can change must live
// in guest memory so whole-machine savestates capture it; savedata therefore
// goes into this tree. Compiled into BOTH builds so the equivalence gate
// compares like against like.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

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

// Seeding (chimera#161): what a project puts on the stick before the machine
// starts, during Init and so inside the sealed baseline - a savestate then
// carries only what the game writes. A zip's files go onto the stick: an
// entry already under PSP/ goes where it says (the tree Export Save Data
// writes, so export-then-import round-trips); any other entry is a folder the
// user took off a stick - a save ("ULUS10041DATA00/PARAM.SFO") or a game's
// DLC ("ULUS10336/...") - and goes under `under` ("PSP/SAVEDATA/" or
// "PSP/GAME/"). False, with *err saying why, for bytes that are no zip, a zip
// with no file in it, or an entry that climbs out of the stick ("..", an
// absolute path): a project whose data is silently ignored is worse than one
// that will not start. *files counts what went on.
bool Chimera_MemstickSeedZip(const uint8_t *data, size_t len, const char *under, int *files, std::string *err);
