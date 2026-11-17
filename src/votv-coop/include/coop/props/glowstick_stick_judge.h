// coop/props/glowstick_stick_judge.h -- the host's judge of a client's glowstick stick.
//
// A client's stick crosses as its glowstick's own save record (coop/props/glowstick_sync) and the HOST takes it into its
// world: `loc` is the point its glowstick hangs from, `attachToKey` the prop it is constrained to. The host asks where the
// record is in hand (prop_record_gates' intent record gate, called from prop_save_data::OnChunk and again from the park),
// as MTA's server judges a client's request against its own state and answers the asker alone
// (CGame.cpp:3041-3043 the source must be the player, 3118-3135 a distance cut-off, 3347-3363 the failure reply with the
// server's position). The checks: the stick fields are the glowstick layout and finite; the sender's hold of this
// glowstick is the one it sticks it in, once per hold; the point lies within the glowstick's ray plus a latency slack of
// the host's copy; an attached stick's frames and transform are bounded and the struck prop is live and near the point; a
// static stick's point lies on a surface. A refused stick is answered by the host's own record of that glowstick, sent to the
// sender alone, and a client's release of a stuck glowstick is refused too (a grab frees it, at the hold's begin).
// Game thread.
#pragma once

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::glowstick_stick_judge {

// Caches the session and registers the record gate for prop_glowstick_C. Idempotent.
void Install(coop::net::Session* session);

// Session teardown: the counters are said and cleared.
void OnDisconnect();


namespace dev {   // the glowstick drill's events, on the host
struct Counts {
    uint32_t judged = 0;       // sticks the judge ruled on
    uint32_t refused = 0;      // sticks refused
    uint32_t corrected = 0;    // the host's records sent to the sender of a refused stick
};
Counts Get();
}  // namespace dev

}  // namespace coop::glowstick_stick_judge
