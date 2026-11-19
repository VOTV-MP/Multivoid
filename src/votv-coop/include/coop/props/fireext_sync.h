// coop/props/fireext_sync.h -- a fire extinguisher's foam (prop_fireExt_C) is shown on every peer, and the host's copy
// answers a client's damage.
//
// An extinguisher sprays from a hand (its `fire`: the effect, the sound, a tick that drains `life` and casts
// `extinguishRay`) and thrusts in the world (its `shoot`, the runaway). Each ran on one machine only. A player's spray
// goes to the host (FireExtIntent op 2), which judges the sender's spray budget and hand, shows it on its mirror of that
// hand and tells every peer (FireExtState op 1), each showing it on its mirror. A hold that ends ends the spray as an
// event. A client's damage to its copy asks the host (op 3, AskRunaway): the host judges the sender's budget, reach and
// the copy and runs the game's own verb, and the thrust lane says the thrust to every peer (op 0).
//
// TRANSITIONAL until fire itself is the host's: fire runs on every peer separately, so each peer's own ray is how its fire goes out.
// Every peer casts the game's ray from its mirror of the sprayer's hand, from the puppet's synced eye along its synced
// aim, each frame the spray is on (CastRays); an explicit start and end never run the mirror's tick. Once fire is
// the host's, that change deletes the casts of a non-host peer (CastRays there, and the sprayer's own tick ray on a client).
#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct FireExtPayload;
}  // namespace coop::net

namespace coop::fireext_sync {

// Keep the session and register the watches on the extinguisher's fire and on this player's hold change. Idempotent.
void Install(coop::net::Session* session);

// HOST: each spray's count. BOTH: the rays and the display of every other player's spray, and a client's thrusts that wait
// for their copy. Per frame.
void Tick();

// HOST, at a joiner's world-ready edge: every spray, and every extinguisher that thrusts or is partly spent, to that slot.
void SendJoinSnapshotForSlot(int slot);

// HOST: a client's spray start or stop (op 2) or runaway ask (op 3); the dispatcher checked the fields.
void OnIntent(int senderSlot, const coop::net::FireExtPayload& p);

// CLIENT: the host's thrust (op 0) or a player's spray (op 1); the dispatcher checked the fields.
void OnState(const coop::net::FireExtPayload& p);

// CLIENT: this client's copy took damage that would start its runaway: ask the host to run it (op 3). At most 8 copies are
// asked about; one asked about again waits a second, then two, up to 30 seconds, until its thrust is shown. A ninth takes
// the place of the copy asked about longest ago when that ask is over 30 seconds old (a cache eviction, not a verdict),
// else it is not asked and the refusal is counted.
void AskRunaway(void* copy);

// HOST: say a world extinguisher's thrust (op 0) to every peer, or to `toSlot` when it is not negative; by the copy, or by
// its element id when the copy is gone. Nothing is sent for an extinguisher with no element id. A negative `charge` says the
// extinguisher is gone: an off with no charge, which only retires what a joiner's copy waits for. Any other says its charge.
void SayThrust(void* ext, bool on, float charge, int toSlot);
void SayThrustByEid(uint32_t eid, bool on, float charge, int toSlot);

// HOST, for the drill: the reason the host last gave a runaway ask about the extinguisher `eid` (applied, budget, notReady,
// noRow, wrongType, noBody, outOfReach, noTarget, notExtinguisher, heldByOther, alreadyActive, verbFailed), or null when it gave none.
// The last 8 eids are kept.
const char* LastRunawayAnswer(uint32_t eid);

// For the drills: the facts the "fireext: applied" lines print, and what this peer has been told. `seq` is the number of the
// last line of that spray slot or thrust copy (0: none yet), and rises with every line of either kind, so a drill that noted
// the number when it began waiting sees a later line as a different one. A spray's charge is the hand's, read after its
// state was written; a thrust's is the copy's.
struct Applied {
    uint32_t seq = 0;
    bool on = false;
    float charge = -1.f;
};
Applied LastSprayApplied(uint8_t slot);
Applied LastThrustApplied(uint32_t eid);

// CLIENT: the host's last word on `slot`'s spray said on (shown or still waiting for the slot's mirror); a thrust for `eid`
// that waits for its copy; and, HOST, the count the host keeps of `slot`'s charge (-1 when it has none).
bool SprayToldOn(uint8_t slot);
bool ThrustWaits(uint32_t eid);
float HostSprayCount(uint8_t slot);

// A peer left: its spray ends here and, on the host, is said off to everyone; its tallies are printed.
void OnPeerLeft(uint8_t slot);

// Session end: the sprays, the waiting thrusts, the asks and the tallies are cleared.
void OnDisconnect();

}  // namespace coop::fireext_sync
