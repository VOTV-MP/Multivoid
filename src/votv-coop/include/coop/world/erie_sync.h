// coop/world/erie_sync.h -- the Erie plush quest and the live Erie it makes, one for every player, the host's (docs/erie.md).
//
// prop_eriePlush_C eats a paper and a pencil, then `write` names a food; eating that food raises `fullness`, the odds of `fuck`,
// the morph that mints the live Erie (eriePlushLive_C), grants `liveerie` and destroys the plush. The live Erie has drag
// (erieDrag_C), chop (erieChop_C) and angry (npc_angryErieFlesh_C) forms. The host owns all of it; a client's plush copy runs no
// hit handler, `write`, `fuck`, `anger`, `lonetimer`, `eatcheesetimer` or fire, microwave or explosion verdict (spawn_authority):
//   - the hit: a client's hit on its copy by a quest prop is refused and asked of the host (PropIgniteIntent kVerbErieHit: the
//     plush's key, the prop's element id); the host runs the handler with its own copy, so notes and food cross as props;
//   - the state: `haspaper` and `haspencil` go from the host on change (AmbientFixtureState kEriePlush), and a copy runs its own
//     `upd`; a joiner is sent every plush;
//   - the forms: the live and angry Erie ride the npc lane (late allowlist entries, graphs parked by npc_mirror_brain), the drag
//     and chop the world-actor lane; every function that mints one is a spawn source, and a client's own mint is cancelled;
//   - the achievement: the host's `liveerie` grant is an EventCue (kCueLiveErie), and a client grants its own profile.
// A client's mirror of a form needs the form's class loaded, which a world holding a plush or a saved live Erie has. The lane
// has not been run on two peers yet. Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct AmbientFixtureStatePayload;
struct EventCuePayload;
}  // namespace coop::net

namespace coop::erie_sync {

inline constexpr uint32_t kCueLiveErie = 170;   // the plush became the live Erie on the host: the achievement `liveerie`
inline constexpr uint8_t kVerbErieHit = 12;     // PropIgniteIntent: a client's copy of the plush was hit by a prop of the quest

void Install(coop::net::Session* session);

// HOST: each plush's flags once a second when one changed. CLIENT: a state waiting for its copy.
void Tick();

// CLIENT: a cue of this lane. False when the id is not this lane's.
bool OnCue(const coop::net::EventCuePayload& p);

// CLIENT: a plush's flags (family kEriePlush).
void OnState(const coop::net::AmbientFixtureStatePayload& p);

// HOST: a PropIgniteIntent as it came. True when its verb is this lane's.
bool TakeIntent(int senderSlot, const uint8_t* bytes, uint16_t len);

// HOST: a peer's world is ready: every plush is sent again.
void QueueConnectBroadcastForSlot(int slot);

void OnDisconnect();

// [dev] A drill's reflected call of the handler on a client's copy stands for the game's own (a physical hit), so the lane judges it
// as one. Off outside a drill.
void SetDrillCallsCount(bool on);

// [dev] How many hits the client refused on its copies of the plush, and how many of those it asked of the host.
uint64_t RefusedHits();
uint64_t ForwardedHits();

}  // namespace coop::erie_sync
