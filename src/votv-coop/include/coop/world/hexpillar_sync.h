// coop/world/hexpillar_sync.h -- the basalt pillars are the host's.
//
// Digging the stone (prop_hexpillar_C) up runs its digUp, which mints a hexpillar_C. A pillar rises in its own tick
// (`a`, 0 to 1, drawn by `upd`) and, as it rises, mints the next pillar beside it, `i` times; each machine ran its own,
// so the host's spread reached no client. HOST: the pillars cross as world-actor mirrors (the stone and each pillar are
// catch sources), save-loaded ones enrolled at their births (coop/world/ambient_births); a rising pillar's `a` is sent
// as it moves, and every pillar's to a joiner at its world-ready (EventCue 131 its place, 132 its `a`). CLIENT: a
// mirror's tick is parked, so it never spreads, and it draws the host's `a` (one that came before its mirror waits for
// it); its own pillars end at their births. Its dig of the stone is cancelled and asked of the host (AnomalyAsk op 1,
// coop/world/anomaly_ask); its swing at a pillar is a hit intent (coop/creatures/hit_target_creature), and an
// explosion's reach of a mirror is the host's. Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct EventCuePayload;
}  // namespace coop::net

namespace coop::hexpillar_sync {

// The EventCue ids; the cue registry's ids are append-only.
inline constexpr uint32_t kCuePillarAt = 131;    // a pillar's place
inline constexpr uint32_t kCuePillarRise = 132;  // x = its `a`; applies to the pillar 131 named

inline constexpr const wchar_t* kPillarClass = L"hexpillar_C";

void Install(coop::net::Session* session);

// The watches, retried until registered; the host's enrol and sends, a client's sweep. Game thread, once a tick.
void Tick();

// A client's receiver: true when the cue was this lane's.
bool OnCue(const coop::net::EventCuePayload& p);

// HOST: a client's dig of the stone at the place (AnomalyAsk op 1).
void OnDigAsk(uint8_t senderSlot, float x, float y, float z);

// HOST: every pillar's rise, queued for a joiner at its world-ready (coop/session/connect_replay). Game thread.
void SendJoin(int slot);

void OnDisconnect();

}  // namespace coop::hexpillar_sync
