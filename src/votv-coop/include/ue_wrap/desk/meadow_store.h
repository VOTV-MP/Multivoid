// ue_wrap/meadow_store.h -- standalone engine access for the MEADOW signal
// DATABASE on the in-game stationary PC (game classes laptop_C / ui_laptop_C):
// saveSlot.savedSignals_0 (TArray<Fstruct_signalDataDynamic>, 0x70 stride) and
// its two id-preserving ui_laptop verbs. Principle-7 engine-wrapper layer;
// coop::meadow_db_sync drives the mirror through here.
//
// RE (votv-meadow-db-L9-impl-DESIGN-2026-07-19.md, 15-round /qf):
// - The store LIVES on the saveSlot object (persistence free); the deck list's
//   save mirror savedSignals_comp_0 is a different array (no lane).
// - The ui_laptop widget is created ONCE at mainGamemode BeginPlay and stored
//   in gamemode.laptop (persistent per-world, screen-open irrelevant); the
//   laptop DEVICE's BeginPlay repoints widget.laptop := self. Apply gate =
//   both pointers live.
// - ui_laptop.addSignal(data) is id-PRESERVING (zero mint sites -- the re-mint
//   lives upstream in the unit-2/unit-4 chain) and maintains the data array +
//   the parallel widget array (`slots` + vb_signals children + recountChildren
//   ind refresh) in ONE call. removeSignal(index) likewise; its playSignal
//   tail STOPS audio (SetActive FALSE) -- wire deletes are audio-benign.
// - NO pointer-based RowKey here (deliberate divergence from saved_signals.h):
//   sortSignal moves rows via BP Array copies that deep-copy FStrings, so
//   pointer identity dies at every move. Cross-peer identity is the
//   signal_wire CONTENT hash alone; the lane keeps a content-hash multiset.

#pragma once

#include "ue_wrap/desk/signal_dynamic.h"

#include <cstdint>

namespace ue_wrap::meadow_store {

// Resolve gamemode {laptop, saveSlot} offsets, the saveSlot store offset, and
// the ui_laptop verbs (throttled lazy retry). True when the core set is up.
bool EnsureResolved();

// The resolved ui_laptop_C class (nullptr until EnsureResolved succeeds). The
// lane's 0x45 ctx class-check reads this instead of running its own FindClass
// walk (perf audit C-1: an unthrottled per-tick FindClass retry is the exact
// pre-world 60 Hz array-walk bomb the resolver throttle exists to prevent).
void* LaptopWidgetClass();

// The live ui_laptop widget (gamemode.laptop) with a live device back-pointer
// (widget.laptop). nullptr when either is unresolved/dead -- the apply gate.
void* Widget();

// Element count of saveSlot.savedSignals_0 (-1 if unresolved / no world).
int32_t Count();

bool ReadRow(int32_t index, ue_wrap::signal_dynamic::Row& out);

// Reflected ui_laptop.addSignal(data=row) on the live widget -- the native
// id-preserving append (data + widget arrays coherent in one call).
bool ApplyAddSignal(const ue_wrap::signal_dynamic::Row& row);

// Reflected ui_laptop.removeSignal(index) -- the native remove (both arrays +
// selection re-point; audio-benign).
bool ApplyRemoveSignal(int32_t index);

// Byte-permute the store rows in place: row i of the NEW order := old row
// srcIdx[i]. srcIdx must be a full permutation of [0, Count()). FString/array
// pointers move WITH their 0x70 blocks (one temp copy, each pointer lands
// exactly once -- no alloc/free/dup); GT-serial so the mid-state is
// unobservable. The WIDGET arrays are NOT touched -- follow with
// ApplyGenSignalList (the game's own full rebuild) for coherence.
bool ReorderRows(const int32_t* srcIdx, int32_t n);

// Reflected ui_laptop.genSignalList() -- the game's own full widget-list
// rebuild from the data array (zero-arg; the order re-applier).
bool ApplyGenSignalList();

}  // namespace ue_wrap::meadow_store
