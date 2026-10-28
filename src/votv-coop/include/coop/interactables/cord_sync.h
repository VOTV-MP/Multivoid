// coop/interactables/cord_sync.h -- a cord's link to its socket as the host's state. The host decides every link; the
// player who plugs or unplugs a cord on a client runs the game's own verb first and reports its outcome as a claim, and
// the host's answer (the cord's link as the host now has it) is what the client settles on. MTA's shape: in
// Packet_VehicleTrailer (reference/mtasa-blue/Server/mods/deathmatch/logic/CGame.cpp:3820-3915) the client reports its
// own attach, the server tests the sender and the state, applies, and the others follow. Divergences: the host answers
// the claimant (MTA skips the source; the answer follows VEHICLE_REQUEST_IN's reply, CGame.cpp:3092-3098, as the floppy
// slot lane does); a reach test; compare-first on both edges (MTA's detach only, :3901); the occupant of a socket stays
// and the claiming cord ends unplugged, the game's connect rule, where MTA's newcomer evicts (:3845-3871); a claim on a
// cord the host's own player drags is answered with the host's link.
//
// Wire: ReliableKind::CordLinkState (CordLinkPayload). Op 0 a client's claim, to the host, never relayed; op 1 the
// canonical, the host's answer to a claim (seq = the claim's) or its broadcast of a change (seq 0); op 2 the end of
// the seed a joiner is sent at its world-ready edge. Game thread throughout.
//
// What watches what is in cord_sync_watch.cpp, the lane in cord_sync.cpp.

#pragma once

#include "coop/net/protocol.h"

#include <string>

namespace coop::net { class Session; }

namespace coop::cord_sync {

// Register the watches on the cord's own verbs and the scan-hub listing of corded props.
void Install(coop::net::Session* session);

// Settle the watches. HOST: send the seed a joiner is owed once the listing is of the current world. CLIENT: retry
// the canonicals whose prop or socket was not here. Both: the 60 s line of the lane's counters, silent at zero.
void Tick();

// A message that reached the dispatcher: op 0 is the host's to judge, ops 1 and 2 a client's to apply. The
// dispatcher has judged the format and the direction.
void OnReliable(const coop::net::CordLinkPayload& payload, uint8_t senderSlot);

// Why the dispatcher refused a message of this kind. Each refused message has one reason, the first that holds in this
// order: a payload that is not the struct's size, a key longer than the wire's, an op the sender's role may not send
// (an op unknown or in the wrong direction), a field a form leaves empty or a pad that is not.
enum class Drop : uint8_t { Size, Key, Op, Field };

// The dispatcher refused a message of this kind: counted under its reason, not logged.
void CountDropped(Drop why);

// HOST, at a joiner's world-ready edge: every corded prop's link, plugged or not, then op 2. Waits for the listing
// of the current world.
void QueueConnectBroadcastForSlot(int peerSlot);

// The session's end: the lane's tables and counters, after a summary line.
void OnDisconnect();

// The level socket (a socket no prop owns) whose actor-save key is `key`, or null. The wire names a socket by this
// key, and the lookup is a table the lane reads from the object index once per world generation: a key a peer sends
// is only ever looked up, never made a name the engine's pool keeps. Game thread.
void* LevelSocket(const std::wstring& key);

// What the drills read of the lane. A client's: whether the seed's op 2 has arrived, the claims sent (the latest
// claim's seq, since the seq starts at 1 and counts each claim), the stretch breaks refused, and the canonicals the
// pending rule dropped (the answer to a claim this client did not register is one). A host's: the claims taken and
// how each ended (each claim ends in exactly one of applied, equal and the refusals), the answers sent, and the
// dispatcher's drops by reason. A read; the fields of the other role stay zero.
struct DrillView {
    bool seeded = false;
    uint32_t claimsSent = 0;
    uint32_t stretchRefused = 0;
    uint32_t pendingDropped = 0;
    uint32_t claims = 0, claimsApplied = 0, claimsEqual = 0;
    uint32_t refusedRate = 0, refusedDead = 0, refusedReach = 0, refusedHostDrag = 0;
    uint32_t unresolved = 0, keyPending = 0, laneOff = 0;
    uint32_t answers = 0;
    uint32_t dropSize = 0, dropKey = 0, dropOp = 0, dropField = 0;
};
DrillView ReadForDrill();

}  // namespace coop::cord_sync
