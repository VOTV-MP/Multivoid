// harness/autotest/disc_drills.h -- what the two disc drills wait on before they act. Private to the drills
// (autotest_sv_request.cpp, autotest_module_buy.cpp).

#pragma once

#include "harness/autotest/peer_signal.h"

#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/session/host_world_settle.h"
#include "coop/session/net_pump.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/object_index.h"

#include <atomic>

namespace harness::autotest::disc_drill {

template <class Fn>
void OnGameThread(Fn&& fn) {
    ue_wrap::game_thread::RunAndWait([&fn](std::atomic<int>& d) { fn(); d.store(1); });
}

// HOST: its player stands and its world has settled: the load's own removals are over and the object index holds every
// birth announced, so what the drill spawns or writes is not taken back.
inline bool HostWorldUp() {
    if (!peer_signal::WaitFor("the host's player", 240, [] {
            void* pawn = nullptr;
            OnGameThread([&] { pawn = coop::players::Registry::Get().Local(); });
            return pawn != nullptr;
        }))
        return false;
    return peer_signal::WaitFor("the host's world settled and the object index caught up", 120, [] {
        bool settled = false;
        OnGameThread([&] {
            settled = coop::host_world_settle::Settled() && ue_wrap::object_index::IsSeeded() &&
                      ue_wrap::object_index::Backlog() == 0;
        });
        return settled;
    });
}

// CLIENT: its world is announced and the host's puppet stands here.
inline bool HostPuppetUp() {
    if (!peer_signal::WaitFor("the announced world", 480, [] {
            bool ready = false;
            OnGameThread([&] { ready = coop::net_pump::HasAnnouncedWorldReady(); });
            return ready;
        }))
        return false;
    return peer_signal::WaitFor("the host's puppet", 60, [] {
        bool have = false;
        OnGameThread([&] {
            coop::RemotePlayer* p = coop::players::Registry::Get().Puppet(0);
            have = p && p->valid();
        });
        return have;
    });
}

}  // namespace harness::autotest::disc_drill
