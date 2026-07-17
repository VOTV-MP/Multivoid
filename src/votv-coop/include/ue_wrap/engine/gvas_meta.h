// ue_wrap/gvas_meta.h -- direct .sav (GVAS) metadata reader for the save picker.
//
// Engine-wrapper layer (principle 7): understands UE4's SaveGameToSlot on-disk
// format (uncompressed GVAS header + tagged property stream) just enough to
// harvest the handful of saveSlot_C scalars a picker row shows -- WITHOUT
// deserializing the save. A VOTV save is 15-20 MB of world arrays;
// UGameplayStatics::LoadGameFromSlot parses ALL of it synchronously on the game
// thread, so the old picker scan (native loadSlots = N x LoadGameFromSlot) froze
// the game for seconds at picker-open (user 2026-07-11, ~11 saves x ~17 MB).
// Here every unwanted property's payload is SKIPPED via its tag's Size field:
// a 20 MB file costs a few hundred small reads + seeks, runs on a WORKER thread.
//
// Tagged-property caveat (measured on s_1234.sav, 2026-07-11): SaveGameToSlot
// serializes DELTA-VS-CDO -- a property equal to its class default is NOT in the
// file (health/maxHealth absent at 100/100; Version absent when it equals the
// authored default). "Missing" therefore means "CDO default"; the caller fills
// those from the live saveSlot_C CDO (game thread), reproducing
// LoadGameFromSlot's NewObject-then-apply-deltas semantics exactly.
//
// Pure file I/O -- no engine access; callable from ANY thread.

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

// Parse the file's GVAS header + walk the TOP-LEVEL tagged-property stream,
// harvesting the fields above and seeking past everything else. Returns false
// (out.parsed=false) on IO failure / not-a-GVAS / malformed stream -- the
// caller drops the file from the list (native parity: a LoadGameFromSlot
// failure was skipped too). Any thread.
bool ReadSlotMeta(const std::wstring& savPath, GvasSlotMeta& out);

}  // namespace ue_wrap::gvas_meta
