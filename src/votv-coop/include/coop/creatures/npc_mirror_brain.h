// coop/creatures/npc_mirror_brain.h -- a creature mirror's Blueprint makes nothing of its own.
//
// A mirror is the creature's whole Blueprint running on a client: every body its BeginPlay armed (timers, delay
// resumes, overlap and montage delegates, move callbacks) fires on the client's clock and acts there. The host's
// copy is the creature; the pose stream shows the mirror. So a mirror of a parked family has its event graph refused
// entry by entry but the few a table names, and the part of BeginPlay it needs to look right is done by our code.
//   - FAMILIES: the zombie and every class below it, and the fossilhound; the table's class names decide it
//     (IsParkedClass). Every other family keeps its own pair of hits until it is parked.
//   - THE MARK: set when a mirror is born, or when a client's own save-loaded twin is bound; a client's own creature
//     is never marked and runs whole. It is the actor's slot and the slot's serial, true only while the slot holds
//     that serial, so a recycled address is never a mirror and a mark nothing removed matches nothing.
//   - THE WATCHES: a script-gate name watch per class that owns an event graph, before the body.
//   - THE END: the death seam's end of an Npc mirror removes its mark after the engine's own end, so the mirror's
//     ReceiveDestroyed is still refused.
// Coop layer; game thread.

#pragma once

#include <cstddef>
#include <cstdint>

namespace coop::npc_mirror_brain {

// Mark `actor`, a mirror of a creature, before its BeginPlay can run. A full table drops the marks whose slot no
// longer holds their serial first; a mirror still unmarkable is said, and its Blueprint runs. Game thread.
void MarkMirror(void* actor);

// Is `obj`, a live object, a marked mirror? One probe of the table. Game thread.
bool IsMarkedMirror(void* obj);

// Remove the mark of the slot `index` that holds `serial`; a slot with another serial is left alone.
void UnmarkMirror(int32_t index, int32_t serial);

// Is `cls`, or a class above it, one of the parked families? Walks the class chain against the tables' class
// names. Game thread.
bool IsParkedClass(void* cls);

// A mirror of any family has been begun and not finished (or a twin is being bound): mark it when its class is
// parked, and say, once per class, when a watch its BeginPlay passes through is not live. Game thread.
void PrepareBirth(void* actor);

// After the mirror is installed: the presentation its class's BeginPlay set up and the refused graph no longer
// does. A no-op for an actor that is not a marked mirror. Game thread.
void SetUpPresentation(void* actor);

// Register the watches and subscribe to the death seam, each once per process. Called every pump tick by the
// install fanout, which is also the retry until each piece latches. Game thread.
void Install();

// Teardown: say what the watches refused, drop every mark.
void OnDisconnect();

// ---- The tables (npc_mirror_brain_table.cpp) ----
// The entries of one event graph that run on a marked mirror, as EntryPoint values.
struct EntryTable {
    const wchar_t* className;
    const int32_t* allowed;
    size_t         count;
};

// The table of the class that OWNS the executing graph (the outer class of the ubergraph function), by name; null
// when no table names it. A class with no graph of its own runs its parent's graph, so it is looked up by that one.
const EntryTable* TableFor(void* graphClass);

}  // namespace coop::npc_mirror_brain
