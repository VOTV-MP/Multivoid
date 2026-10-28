// coop/creatures/geom_octa_sync.h -- the Geom Octahedron's lane, the host's half: what its Blueprint decides, observed
// and sent.
//
// The Octa is an NPC mirror on a client (coop/creatures/npc_mirror_brain parks its decisions; geom_octa_relay replays
// the host's). This lane watches the bodies that decide and the ones that show it, sends the host's two decisions as
// GeomOctaEvent, and prints one `[OCTA]` line for each, so a drill reads the outcomes and each peer's own look from the
// logs. Coop layer; game thread.

#pragma once

#include <cstddef>
#include <cstdint>

namespace coop::net { class Session; }

namespace coop::geom_octa_sync {

// The lines. Each but the last ends in `t=<ms>`, coop::net::NowMs(): the log stamp has a one-second grain.
//   aggro sent        the host's `aggro` body passed its guard (no target on entry), sent with the target it was given
//                     and the Octa's verdict count as the base
//   verdict sent      the host's "fire" montage finish, sent: n, the Octa's count for its life, and refire (a target
//                     stands). Counted for every Octa, sent only for one whose EntitySpawn was sent
//   idle              the "close" montage finish, on every peer
//   laser             a laser's BeginPlay, on every peer
//   mirror ran move   a client mirror's wander body ran: it was not parked
// Fields: `eid=` is the Octa's NPC element id, or `none` for an actor that is no Octa element (a laser's is its
// owner's). `target=` is `none`, `slot:<n>` (a player: the host's own is 0, a puppet its slot) or `element:<eid>` (an
// element the registry names); an actor no element names, an ATV among them, is `none`.

// Register the script-gate watches. Idempotent; a watch the gate refuses is said once and asked again on the next call
// (the install fanout makes one every pump tick) until all hold. The watches match every incarnation of the class, and
// are live once the game thread resolves their names (the class loads). Game thread.
void Install(coop::net::Session* session);

// What this machine has seen since the session began, for a drill to read; the log lines carry the detail.
struct Counters {
    uint32_t hostAggro;     // aggros the host's Octas decided (sent or not)
    uint32_t hostVerdict;   // fire verdicts the host's Octas counted (sent or not)
    uint32_t idle;          // idle lines
    uint32_t laser;         // laser lines
    uint32_t mirrorMove;    // mirror ran move lines
    uint64_t lastLaserMs;   // coop::net::NowMs() at the last laser line (0 before the first)
    uint32_t sendFailed;    // GeomOctaEvents the reliable channel refused (a lost decision)
    uint32_t keepFailed;    // Aggros that could not be kept for an unsent spawn (no serial, or the table full)
    uint32_t overlapMin;  // [dev] minSphere begin-overlaps the host's Octa took (only while octa_drill=measure|attack)
    uint32_t overlapProp;   // [dev] the same for propAggroSphere
    bool     windowReached;    // the open window's n-th host verdict has been counted
    uint32_t idleAtWindow;      // the idle count at the moment it was counted
    uint32_t joinMid;           // join snapshots that found an Octa mid-attack on a nameable target (the Aggro path)
    uint32_t joinBetween;       // join snapshots that found the Octa between attacks, or on a target the wire cannot name
};
Counters GetCounters();

// Opens a window on the host verdicts of `octa` counted from now on (another Octa's are not counted): when the `n`-th
// of them is counted, the idle count at that moment is kept in the counters, so a poll that sees the n-th verdict late
// still tells an idle that came in between. One window at a time; opening another replaces it. Game thread.
void OpenVerdictWindow(uint32_t n, void* octa);

// HOST, after each NPC EntitySpawn send attempt (npc_sync's interceptor, npc_world_enum's enrol, and npc_pose_host's join
// snapshot, per slot, on a success only): `className` and `classLen` are the spawned class (bytes, no terminator needed),
// `actor` the spawned actor or null where it does not exist yet (the interceptor sends before the spawn), `sent` whether
// any peer was sent it. Only the Octa matters. The host sends a GeomOctaEvent only for an Octa whose spawn it has sent,
// since the event rides Default behind the spawn and the client's mirror is built by it. A kept Aggro (the first aggro runs
// inside the spawn's finish, before this) goes out here if its attack still stands, and is dropped when nothing was
// sent. `slot` is the one peer the send reached (the join snapshot's), -1 for the live sends, which reach every
// world-ready peer: the kept Aggro goes to that same audience. A snapshot's send of an Octa mid-attack on a target the
// wire can name also sends the joiner one Aggro (the join's re-send, logged `join slot=`). Game thread.
void OnNpcSpawnSent(uint32_t eid, void* actor, const char* className, size_t classLen, bool sent, int slot);

// The target text (`slot:1`, `none`, ...) of the last aggro the host decided, or an empty string before the first.
const char* LastAggroTarget();

// Per-session state back to its start: the verdict counts, the kept Aggros, the sent spawns, the counters. The watches
// stay latched.
void OnDisconnect();

}  // namespace coop::geom_octa_sync
