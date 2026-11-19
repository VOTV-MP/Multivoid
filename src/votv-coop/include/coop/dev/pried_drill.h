// coop/dev/pried_drill.h -- [dev] a crowbar's pry of a prop with no wall-attach component, on both peers.
//
// The pry lane (coop/props/prop_pry_sync) carries two bodies: a plank that clears its flags and a
// radiator that starts its breaking sequence. Arms (`pried_drill`): plank_host, plank_client,
// radiator_host, radiator_client -- the watching peer names the first stuck prop of the class in the
// lobby chat, the acting peer runs that prop's own crowbarOpen with no crowbar and says where its copy
// came to rest, and the watching peer judges its own copy: freed by the lane's replay, moved, and
// resting near the acting peer's. Every verdict is one line, `[PRIED-DRILL]` with the peer's tag and
// `PASS` or `FAIL`.
//
// Arms measure_plank and measure_radiator measure the same pry instead of judging it (pried_measure.h).

#pragma once

#include <string>

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::dev::pried_drill {

bool IsEnabled();

// This peer's join is over: a client's own, or on the host one client's with its window closed.
bool JoinOver(coop::net::Session& s);

// The first key, in order, among the live props of `clsName` or below, the stuck ones first; empty when none stands.
// One walk of the object array. Shared with pried_measure.
std::wstring PickByClass(const wchar_t* clsName);

// The drill's pass on either peer. A single read when off. Game thread.
void Tick(coop::net::Session* session);

// Back to the first step. Net disconnect.
void OnDisconnect();

}  // namespace coop::dev::pried_drill
