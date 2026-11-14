// harness/autotest/rejoin_steps.h -- the two halves of a drill's join arm that never differ: the host waits for the
// client's first life to leave and the next life's world to come up, and the first life leaves on its own. A drill
// reads the lane's own state at the ready edge between them. With the rig's --rejoin. Private to the drills that
// include it (grep `rejoin_steps.h` under harness/autotest/ for the list).

#pragma once

#include "harness/autotest/peer_signal.h"
#include "harness/session_runtime.h"

#include "coop/net/session.h"
#include "coop/session/net_pump.h"
#include "ue_wrap/core/game_thread.h"

#include <atomic>
#include <cstdint>

namespace harness::autotest::rejoin {

// HOST: the generation of the client's first life, read once it has said it will leave.
inline uint32_t FirstLifeGen() { return harness::session_runtime::Session().peerGenerationForSlot(1); }

// HOST: the first life is gone from slot 1 and the next life's world is ready there. `atEdge` runs once on the game
// thread in the task that sees the edge, the moment the connect replay has run. False when a bound passed.
template <class Fn>
bool AwaitNextLifeReady(uint32_t firstGen, Fn&& atEdge) {
    namespace PS = peer_signal;
    if (!PS::WaitFor("the client's first life gone from slot 1", 300, [firstGen] {
            auto& s = harness::session_runtime::Session();
            return !s.IsSlotConnected(1) || s.peerGenerationForSlot(1) != firstGen;
        }))
        return false;
    return PS::WaitFor("the rejoined client's world ready", 400, [firstGen, &atEdge] {
        bool ready = false;
        ue_wrap::game_thread::RunAndWait([&](std::atomic<int>& d) {
            auto& s = harness::session_runtime::Session();
            ready = s.IsSlotWorldReady(1) && s.peerGenerationForSlot(1) != firstGen;
            if (ready) atEdge();
            d.store(1);
        });
        return ready;
    });
}

// CLIENT, first life: leave for the rig's rejoin and wait for the session to stop.
inline void LeaveForRejoin(const char* why) {
    ue_wrap::game_thread::RunAndWait([&](std::atomic<int>& d) {
        auto& s = harness::session_runtime::Session();
        if (s.running()) coop::net_pump::FleeAfterNativeMenuTravel(s, why);
        d.store(1);
    });
    peer_signal::WaitFor("this life's session stopped", 15, [] { return !harness::session_runtime::Session().running(); }, false);
}

}  // namespace harness::autotest::rejoin
