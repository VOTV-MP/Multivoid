// coop/props/prop_key_index.h -- every live keyed prop of this peer by its key, for the reads that turn a
// key on the wire into an actor. Seeded complete when a session takes the script gate and kept so; a wire
// read (Resolve, Find) that misses catches the object index up before it answers, so its "no holder" means
// no such prop here, while the uniqueness check (AnotherHolder) reads the index as it stands.
//
// Membership comes from the engine's own create and delete notifications (ue_wrap/core/object_index's
// listeners), the key from the bodies that write it: prop_C writes its key in init (from getKey, which turns
// None into a fresh key), in loadData (which then runs init) and in the setKey event, and the one write the
// game makes to another object's key (ATV.createContainer) is followed by that object's getKey. Script-gate
// POST watches on those four bodies, every class, re-read the key field after each. trashBitsPile keeps
// actor_save's key, written by its loadData and setKey. The chip pile and the garbage clump have no key field
// and are not here (their identity is the eid).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

// MTA's id map is minted by its owner (PopUniqueID,
// reference/mtasa-blue/Server/mods/deathmatch/logic/CElementIDs.cpp:23), so it is complete and unique by
// construction, and its read refuses an element being deleted (:57). Here the game and the save mint the
// keys, so every holder of a key is kept and a wire read's miss is answered only after the object index
// has caught up.
// Game thread, all of it.
namespace coop::prop_key_index {

// The watches and the object-index listener, registered once: harness::Start posts it as the first
// game-thread task, and the frame tail and a session's seed call it again to drive the watches' names to
// resolve. A latched read after that.
void Install();

// The session's edges. Begin when the session takes the script gate: the store is seeded from the object
// index once the four watches are live (until then, and for good if one is dead, the reads answer Pending).
// End when it lets go: the store is cleared, since a write while no session holds the gate goes unobserved.
// Tick on each session tick: a seed still owed, the births parked while their object loaded, the hand set,
// the 60 s summary. A scan-hub consumer checks the store against the live props in the hub's slices and files
// again what the feeds missed, counted as unobserved.
void Begin();
void End();
void Tick();

// Hit: an actor holds the key now. Miss: none does (for Resolve and Find, after the object index caught up;
// for AnotherHolder, as the index stands). Pending: the index cannot answer yet (no session seed, no current
// world, a birth still loading, off the game thread, or inside an object-index drain).
enum class Read : uint8_t { Hit, Miss, Pending };

// The live actor of the current world that holds `key`, the lowest slot of those that do; a child actor and
// a player's hand are never one. `out` is set on Hit only.
Read Resolve(const std::wstring& key, void** out);

// Resolve for a caller whose "not yet" and "not here" are the same answer: the actor on Hit, else null. A
// Pending answered so is counted in the summary, and the first of a session is named. A wire message whose key
// names no prop is then dropped, as MTA drops a packet naming an element it does not hold
// (reference/mtasa-blue/Server/mods/deathmatch/logic/CGame.cpp:2424-2425, :2672-2673); nothing searches further.
void* Find(const std::wstring& key);

// Whether an actor other than `self` holds `key`, by the same rule: a uniqueness check at enrolment, which
// runs inside a script body, so it never catches the object index up: Miss here means none indexed now.
// `other` is set on Hit when given.
Read AnotherHolder(const std::wstring& key, void* self, void** other = nullptr);

// Whether a read now is answered by an index that holds this world: seeded, and the object index's last drain
// left no backlog. The client's world-ready gate asks it, so the host's world arrives on an index that holds
// this one. True outside a session.
bool CanAnswer();

// Whether this install can never index: a watch or the object-index listener refused, or a watch dead for good
// (each logged where it was found). Known on the first game-thread task after boot (harness::Start installs
// it there) or at a session's seed. A session start refuses on it, and a join waiting to announce ends on it
// (MV-J32). Any thread.
bool Unavailable();

// How many props the index holds now, for a diagnostic line.
size_t Size();

// A number that moves whenever the index's table changes: a prop born, dead, re-keyed, the seed, the end of a
// session's hold. A caller that retries a read asks again only when it moved, since a read that missed cannot
// answer otherwise until then. Game thread.
uint64_t Version();

// [dev] Every entry as the store holds it, for the parity probe's stale count.
struct DebugEntry {
    void*        actor;
    int32_t      idx;
    std::wstring key;
};
void DebugCollect(std::vector<DebugEntry>& out);

}  // namespace coop::prop_key_index
