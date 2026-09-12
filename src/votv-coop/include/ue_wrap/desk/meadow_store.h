// ue_wrap/desk/meadow_store.h -- standalone engine access for the MEADOW signal DATABASE on the
// in-game stationary PC (game classes laptop_C / ui_laptop_C): saveSlot.savedSignals_0, a
// TArray<Fstruct_signalDataDynamic> of 0x70 stride, and its two id-preserving ui_laptop verbs.
// Principle-7 engine-wrapper layer; coop::meadow_db_sync drives the mirror through here.
//
// The store LIVES on the saveSlot object, so persistence is free; the deck list's save mirror
// savedSignals_comp_0 is a different array with no lane of its own.
//
// There is NO pointer-based RowKey here, a deliberate divergence from saved_signals.h: sortSignal
// moves rows through blueprint array copies that deep-copy FStrings, so pointer identity dies at
// every move. Cross-peer identity is the signal_wire CONTENT hash alone, and the lane keeps a
// content-hash multiset.

#pragma once

#include "ue_wrap/desk/signal_dynamic.h"

#include <cstdint>

namespace ue_wrap::meadow_store {

// Resolve gamemode {laptop, saveSlot} offsets, the saveSlot store offset, and
// the ui_laptop verbs (throttled lazy retry). True when the core set is up.
bool EnsureResolved();

// The resolved ui_laptop_C class (nullptr until EnsureResolved succeeds). The lane's verb-watch context
// class-check reads this instead of running its own FindClass walk, which unthrottled per tick is
// the pre-world 60 Hz array-walk the resolver throttle exists to prevent.
void* LaptopWidgetClass();

// The live ui_laptop widget (gamemode.laptop) with a live device back-pointer (widget.laptop);
// nullptr when either is unresolved or dead, which is the apply gate. The widget is created ONCE at
// mainGamemode BeginPlay and stored on the gamemode, so it is persistent per world whether or not
// the screen is open, and the laptop DEVICE's BeginPlay repoints widget.laptop := self.
void* Widget();

// Element count of saveSlot.savedSignals_0 (-1 if unresolved / no world).
int32_t Count();

bool ReadRow(int32_t index, ue_wrap::signal_dynamic::Row& out);

// Reflected ui_laptop.addSignal(data=row) on the live widget -- the native insert. It is
// id-PRESERVING, minting nothing, since the re-mint lives upstream in the unit-2 and unit-4 chain,
// and it maintains the data array and the parallel widget array (`slots`, the vb_signals children
// and the recountChildren index refresh) in ONE call.
bool ApplyAddSignal(const ue_wrap::signal_dynamic::Row& row);

// Reflected ui_laptop.removeSignal(index) -- the native remove, maintaining both arrays in one call
// as addSignal does. Its playSignal tail STOPS audio by setting the component inactive, so a wire
// delete is audio-benign.
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
