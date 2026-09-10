// ue_wrap/actors/floppy_disc.h -- the floppy disc prop (Aprop_floppyDisc_C): its class test and
// its two content fields.
//
// Engine-wrapper layer (principle 7): field reads and writes, no network and no coop state. These
// lived in the laptop wrapper, which made a disc's own fields reachable only once laptop_C had
// resolved -- a disc on a table, in a hand or in a server box needs no laptop, and the server-box
// lanes were reaching through the laptop to read one.
//
// The disc's SAVE record is not here: getData/loadData carry data and readWrites (the dump puts
// them at strings[0] and ints[0]) and that path is ue_wrap/actors/save_record. These raw accessors
// remain for the callers that must observe the fields THEMSELVES -- the class gate on the reap
// target, and the disc driver, which has to see what the game wrote rather than what the codec
// says it wrote.
//
// Game thread.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ue_wrap::floppy_disc {

// Resolve the class and the two field offsets. Retried at most once a second while the class is
// not loaded (Blueprint classes load on demand), so a caller on a hot path cannot turn a miss into
// a stream of object-array walks.
bool EnsureResolved();

// Any prop_floppyDisc_C variant (the base class or a descendant).
bool IsDiscClass(void* cls);

// The disc's own mutable content.
struct DiscContent {
    int32_t readWrites = -1;
    std::vector<std::wstring> data;  // the disc's own `data` array
};

bool ReadDiscContent(void* discActor, DiscContent& out);
bool WriteDiscContent(void* discActor, const DiscContent& in);

// Drop the resolved class and offsets (level change).
void ResetCache();

}  // namespace ue_wrap::floppy_disc
