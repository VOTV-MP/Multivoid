// coop/props/prop_syncer.h -- the feeder API of a prop's syncer, and the CLIENT that syncs a prop its own
// interaction moves.
//
// A peer whose own interaction moves a prop (a kick, a drag that is not a hold) calls Claim every tick the
// interaction lasts. On the host that is the driven-prop channel's own claim (prop_drive_host). On a client it
// is MTA's push (reference/mtasa-blue/Client/mods/deathmatch/logic/CClientGame.cpp:4712-4733): one reliable
// PropSyncClaim per episode, the host's PropSyncAnswer taking the client from Pending to Syncing, and then its
// copy's pose goes to the host on PropDrivePose while the copy keeps simulating here. The episode ends with the
// client's own rest End, or by the host taking the prop back (Revoked). The host's side is prop_syncer_host.
//
// Game thread only.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
enum class PropSyncReason : uint8_t;
struct PropSyncAnswerPayload;
}  // namespace coop::net

namespace coop::prop_syncer {

// The feeder API: `actor`'s motion is this peer's interaction right now. Idempotent -- call it every tick the
// interaction lasts. HOST: prop_drive_host::Claim. CLIENT: opens an episode with a PropSyncClaim, or refreshes
// the open one; an actor with no host element id is a counted refusal, never a send. `reason` names the feeder.
void Claim(void* actor, coop::net::PropSyncReason reason);

// Per gameplay tick, first thing in the driven-prop pump: learns the role, and on a client publishes the pose of
// each prop it syncs, ends the ones that rested and drops the ones a hand took or that died.
void Tick(coop::net::Session& s, bool isHost);

// CLIENT: the host's verdict on a claim, or the end of a row it granted. Validated at the router.
void OnAnswer(const coop::net::PropSyncAnswerPayload& p);

// CLIENT: the host's End of generation `gen` of `eid` arrived (called before the stream applies it): the entry
// of that generation goes. Validated at the router.
void OnHostEnd(uint32_t eid, uint8_t gen);

// CLIENT: true while this peer authors `eid`'s drive (a claim pending or granted): the host's poses for it are
// its own rows relayed, and the stream's batch apply skips them. Counts the skip.
bool SkipsHostPose(uint32_t eid);

// Session end: the table, the backoffs and the counters. Game thread.
void OnDisconnect();

namespace dev {   // read only by src/coop/dev/ (the prop syncer drill); the last-grant record is kept only while the drill row is on
enum class EntryState : uint8_t { None, Pending, Syncing };
struct EntryView {
    EntryState state = EntryState::None;
    uint8_t    gen = 0;         // the granted generation; 0 while Pending
    uint16_t   seq = 0;         // the claim's seq
    uint32_t   rowsSent = 0;    // PropDrivePose rows sent in this episode
};
// The open episode on `eid`, state None when there is none.
EntryView EntryOf(uint32_t eid);
// The generation of the last grant this session for `eid`, 0 if none.
uint8_t LastGrantedGen(uint32_t eid);
// Milliseconds from the claim's send to its last grant this session for `eid`, 0 if none.
uint64_t LastGrantMs(uint32_t eid);

enum class Count : uint8_t {
    ClaimsSent, FeederRefused, HeldOff, SendFailed, Granted, GrantedNoEntry, Refused, Revoked, HostEnds,
    AnswersDropped, RowsSent, RestEnds, HandEnds, Died, EchoSkipped, PendingExpired, kCount
};
uint32_t CountOf(Count c);
// Refusals the host answered, by `refusal` (a PropSyncRefusal value; one beyond Paced reads 0).
uint32_t RefusedBy(uint8_t refusal);
}  // namespace dev

}  // namespace coop::prop_syncer
