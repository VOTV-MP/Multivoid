// ue_wrap/saved_signals.h -- standalone engine access for the desk signal
// LIBRARY: gamemode.savedSignals_0 (TArray<Fstruct_signalDataDynamic>,
// @0x0968) and its two native verbs. Principle-7 engine-wrapper layer;
// coop::signal_sync drives the mirror through here.
//
// RE (2026-06-12 savedSignals agent pass):
// - The live list is the gamemode array; saveSlot.savedSignals_comp_0 is its
//   save marshal (copied at saveObjects/load) -- persistence is free.
// - gamemode.saveSignal(signal, new, checkOnly, downloadedAtQuality,
//   selfQuality, succ): validity-gated (decoded>=size && size>0) append +
//   "Create Signal List" pane rebuild + specials/forceObjects bookkeeping.
//   `new` is a dead param; selfQuality=true keeps the row's own quality.
//   No sounds/points/stats inside -- those live at the producer's button
//   sites and correctly stay producer-local.
// - gamemode.deleteSignal(IndexToRemove): Array_Remove + pane rebuild only.
// - Producers: download-save, drive import, copySignal (direct Array_Add).
//   Export-to-drive = Array_Get + deleteSignal (a MOVE).

#pragma once

#include "ue_wrap/desk/signal_dynamic.h"

#include <cstdint>

namespace ue_wrap::saved_signals {

// Per-row INSTANCE key (raw bytes; zero reflected calls) -- same doctrine as
// ue_wrap::email::RowKey: stable per row lifetime (rows move only by byte
// copy), NOT a cross-peer identity (that's the signal_wire content hash).
// A user RENAME reallocs the name FString -> key change -> the shadow diff
// sees delete+append and re-broadcasts the row, which IS the desired
// convergence for renames (order moves to tail on mirrors; documented).
struct RowKey {
    uint64_t namePtr = 0;  // FString data ptr @0x00
    uint64_t idPtr   = 0;  // FString data ptr @0x18
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

bool ReadRow(int32_t index, ue_wrap::signal_dynamic::Row& out);
bool ReadRowKey(int32_t index, RowKey& out);

// Reflected gamemode.saveSignal(row, new=false, checkOnly=false,
// daq=row.downloadedAtQuality, selfQuality=true) -- the native append.
// False on unresolved/call failure OR succ=false (validity gate).
bool ApplySaveSignal(const ue_wrap::signal_dynamic::Row& row);

// Reflected gamemode.deleteSignal(IndexToRemove) -- the native remove +
// pane refresh.
bool DeleteSignal(int32_t index);

}  // namespace ue_wrap::saved_signals
