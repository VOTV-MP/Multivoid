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

// Precedent. MTA patches an engine function to return where it wants an effect gone, unconditionally
// (reference/mtasa-blue/Client/multiplayer_sa/CMultiplayerSA.cpp:854-858), and gates a copy's own decisions on
// IsSyncing() or IsLocalPlayer() (reference/mtasa-blue/Client/mods/deathmatch/logic/CClientPed.cpp:6546, :6756). Here
// the gate is per mirror, by mark. MTA also locks a copy's output: the syncer's report locks its health
// (reference/mtasa-blue/Client/mods/deathmatch/logic/CPedSync.cpp:236-252) and the damage handler restores it
// (reference/mtasa-blue/Client/mods/deathmatch/logic/CClientGame.cpp:4509). The output lock does not reach a
// creature's Blueprint, which also writes world state and saves, so the body is refused at its entry.
//
// Mark `actor`, a mirror of a creature, before its BeginPlay can run. A full table drops the marks whose slot no
// longer holds their serial first; a mirror still unmarkable is said, and its Blueprint runs. Game thread.
void MarkMirror(void* actor);

// Is `obj`, a live object, a marked mirror? One probe of the table. Game thread.
bool IsMarkedMirror(void* obj);

// Remove the mark of the slot `index` that holds `serial`; a slot with another serial is left alone.
void UnmarkMirror(int32_t index, int32_t serial);

// Is `cls`, or a class above it, one of the parked families? Walks the class chain against the tables' class
// names, by FName index. Game thread.
bool IsParkedClass(void* cls);

// A mirror of any family has been begun and not finished (or a twin is being bound): mark it when its class is
// parked, and say, once per class, when a watch its BeginPlay passes through is not live. The tables' class names are
// resolved first; until they are, no class can be told as parked, so the mirror is left unmarked, its Blueprint runs
// (fail open), and that is said once per class. Game thread.
void PrepareBirth(void* actor);

// How a mirror came to be: spawned by the client for the host's creature, whose BeginPlay the graph watches refused;
// or adopted, a twin the client's own save loaded, whose BeginPlay ran whole as the client's own creature.
enum class Origin : uint8_t { Spawned, Adopted };

// After the mirror is installed: the presentation its class's BeginPlay set up and the refused graph no longer
// does. On an adopted twin the zombie's setup is run again (it writes the same animation instance, and the walk
// blend is picked again); only the hound's per-viewer loop is not kicked a second time, since its own BeginPlay
// started it. A no-op for an actor that is not a marked mirror. Game thread.
void SetUpPresentation(void* actor, Origin origin);

// [dev], for the drill alone: how many times SetUpPresentation left the hound's per-viewer kick out for an adopted
// twin, whose own BeginPlay ran it (counted each time).
uint32_t AdoptedKickSkips();

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

// The table of the class that OWNS the executing graph (the outer class of the ubergraph function); null when no table
// names it. A class with no graph of its own runs its parent's graph, so it is looked up by that one. The classes are
// told apart by FName index, the names resolved once (ResolveClassNames); until they are, no class has a table, and
// that is said once.
const EntryTable* TableFor(void* graphClass);

// Resolve the tables' class names to FNames; true once they all are. Install retries it every pump tick until then,
// and the lookups below ask for it themselves. Game thread.
bool ResolveClassNames();

// The table whose class name is the text `className`, for a caller holding a name rather than a class; null when none.
// Compares text, so a caller keeps the result. Game thread.
const EntryTable* TableNamed(const wchar_t* className);

// Is `cls`, or a class above it, the class of `table`? The chain stops after 16 hops. Game thread.
bool ChainHasTable(void* cls, const EntryTable& table);

}  // namespace coop::npc_mirror_brain
