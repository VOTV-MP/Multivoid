// ue_wrap/desk/saved_signals.h -- engine access for the desk signal LIBRARY,
// gamemode.savedSignals_0 (TArray<Fstruct_signalDataDynamic>) and its two native verbs. Principle-7
// engine-wrapper layer; coop::signal_sync drives the mirror through here.
//
// The live list is the gamemode array, and saveSlot.savedSignals_comp_0 is its save marshal, copied
// at saveObjects and at load, so persistence is free.
//
// gamemode.saveSignal(signal, new, checkOnly, downloadedAtQuality, selfQuality, succ) is a
// validity-gated append -- decoded >= size and size > 0 -- plus a "Create Signal List" pane rebuild
// and the specials and forceObjects bookkeeping. `new` is a dead param, and selfQuality=true keeps
// the row's own quality. No sounds, points or stats are inside: those live at the producer's button
// sites and correctly stay producer-local. gamemode.deleteSignal(IndexToRemove) is an Array_Remove
// plus that rebuild, nothing else. Producers are download-save, drive import and copySignal's
// direct Array_Add; export to a drive is an Array_Get plus deleteSignal, so it is a MOVE.

#pragma once

#include "ue_wrap/core/row_apply.h"
#include "ue_wrap/desk/signal_dynamic.h"

#include <cstdint>
#include <string>

namespace ue_wrap::saved_signals {

// Per-row INSTANCE key (raw bytes; zero reflected calls) -- same doctrine as
// ue_wrap::email::RowKey: stable per row lifetime (rows move only by byte
// copy), NOT a cross-peer identity (that's the signal_wire content hash).
// A user RENAME reallocs the name FString -> key change -> the shadow diff
// sees delete+append and re-broadcasts the row, which IS the desired
// convergence for renames (order moves to tail on mirrors; documented).
struct RowKey {
    uint64_t namePtr = 0;  // the name FString's data pointer
    uint64_t idPtr   = 0;  // the id FString's data pointer
    int64_t  date    = 0;
    int32_t  level   = 0;
    uint8_t  isCopy  = 0;

    bool operator==(const RowKey& o) const {
        return namePtr == o.namePtr && idPtr == o.idPtr && date == o.date &&
               level == o.level && isCopy == o.isCopy;
    }
};

// Resolve gamemode + savedSignals_0 + the two verbs (throttled lazy retry).
bool EnsureResolved();

// Element count (-1 if unresolved / no world).
int32_t Count();

bool ReadRow(int32_t index, ue_wrap::signal_dynamic::Row& out, bool withImage = false);
bool ReadRowKey(int32_t index, RowKey& out);

// Reflected gamemode.saveSignal(row, new=false, checkOnly=false,
// daq=row.downloadedAtQuality, selfQuality=true) -- the native append. NotNow when the game cannot
// be asked (no gamemode, the verb or the name library unresolved); Refused when it said no (succ=false,
// the validity gate) or the row cannot be put to it; the reason in `why` (ue_wrap/core/row_apply.h).
ue_wrap::RowApply ApplySaveSignal(const ue_wrap::signal_dynamic::Row& row);

// Reflected gamemode.deleteSignal(IndexToRemove) -- the native remove +
// pane refresh.
bool DeleteSignal(int32_t index);

// The leaf of the first ORDINARY (non-unique) signal template name, read from the gamemode's
// signalNames_default / signalUniques (filled in parallel from the list_signals DataTable, so one
// index serves both -- BP: mainGamemode.cpp:2399-2458, lib.cpp:3906-3917). Empty (false) if the
// gamemode or either array is not up yet, so a unique name is never mistaken for an ordinary one. A
// drill uses it to build a saved-signal row the native accepts (succ depends only on decoded >= size
// > 0) with no unique-signal `specials` side effect. Game thread.
bool FirstOrdinarySignalName(std::wstring& out);

}  // namespace ue_wrap::saved_signals
