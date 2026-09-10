// ue_wrap/engine/gvas_meta.h -- direct .sav (GVAS) metadata reader for the save picker.
//
// Engine-wrapper layer (principle 7): understands UE4's SaveGameToSlot on-disk format -- an
// uncompressed GVAS header followed by a tagged property stream -- just enough to harvest the
// handful of saveSlot_C scalars a picker row shows, without deserializing the save. A VOTV save is
// 15-20 MB of world arrays and LoadGameFromSlot parses all of it on the game thread, so a picker
// that opened every slot that way froze the game for the whole scan. Here each unwanted payload is
// SKIPPED via its tag's Size field, so the cost follows the number of top-level properties rather
// than the file size. Pure file I/O -- no engine access, callable from ANY thread.
//
// SaveGameToSlot serializes DELTA-VS-CDO: a property equal to its class default is not in the file
// at all, which is why health and maxHealth go missing at 100/100. Every harvested field therefore
// ships a has* flag, and "missing" means "CDO default" -- the caller fills those from the live
// saveSlot_C CDO on the game thread, which is LoadGameFromSlot's own new-then-apply semantics.

#pragma once

#include <cstdint>
#include <string>

namespace ue_wrap::gvas_meta {

struct GvasSlotMeta {
    bool parsed = false;          // GVAS header + SaveGameClassName read OK
    bool isSaveSlotClass = false; // class name contains "saveSlot_C" -- data.sav
                                  // self-excludes exactly like the native
                                  // loadSlots DynamicCast filter
    bool hasSavedTimeZ = false; int32_t savedTimeZ = 0;  // savedtime FIntVector.Z (day - 1)
    bool hasPoints = false;     int32_t points = 0;
    bool hasHealth = false;     float   health = 0.f;
    bool hasMaxHealth = false;  float   maxHealth = 0.f;
    bool hasVersion = false;    std::wstring version;
    bool hasLastSavedDate = false; int64_t lastSavedDateTicks = 0;  // FDateTime ticks
};

// Parse the file's GVAS header + walk the TOP-LEVEL tagged-property stream, harvesting the
// fields above and seeking past everything else. Any thread.
//
// Returns false (out.parsed = false) for a file that never got as far as a class name: an IO
// failure, or magic that is not GVAS. The caller drops those. A stream that goes malformed AFTER
// the class name -- a bad tag, a negative size, a truncated payload -- only stops the walk, and
// still returns true: the row shows, with CDO defaults standing in for whatever was not
// reached.
bool ReadSlotMeta(const std::wstring& savPath, GvasSlotMeta& out);

}  // namespace ue_wrap::gvas_meta
