// coop/player/puppet_events.cpp -- see coop/player/puppet_events.h.

#include "coop/player/puppet_events.h"

#include "coop/net/protocol.h"
#include "coop/player/players_registry.h"

#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"

#include <array>

namespace coop::puppet_events {
namespace {

constexpr int kMaxListeners = 4;

template <typename Fn>
struct List {
    std::array<Fn, kMaxListeners> fn{};
    int count = 0;

    bool Add(Fn f) {
        for (int i = 0; i < count; ++i)
            if (fn[i] == f) return true;
        if (count == kMaxListeners) return false;
        fn[count++] = f;
        return true;
    }
};

List<LifeFn> g_life;
List<PoseFn> g_pose;

std::array<Seen, coop::players::kMaxPeers> g_seen{};
std::array<bool, coop::players::kMaxPeers> g_heldSaid{};  // a slot's first held (pre-ready) pose was logged

}  // namespace

bool AddLifeListener(LifeFn fn) {
    UE_ASSERT_GAME_THREAD("puppet_events::AddLifeListener");
    return fn != nullptr && g_life.Add(fn);
}

bool AddPoseListener(PoseFn fn) {
    UE_ASSERT_GAME_THREAD("puppet_events::AddPoseListener");
    return fn != nullptr && g_pose.Add(fn);
}

LifeStep StepLife(Seen was, uint8_t healthFrac, bool worldReady) {
    // A pose before the peer's world is ready carries no life state: nothing is learned and nothing is raised.
    if (!worldReady) return {was, false, false};
    // Alive is the test the registry's LifeOf applies to the same stream: any positive health.
    const bool alive = healthFrac > 0;
    const Seen now = alive ? Seen::Alive : Seen::Dead;
    // The first pose of a ready slot has no "from": a death in it is still raised (a peer dead when its puppet
    // appears, a join mid-death, must reach the listeners), an alive first pose raises nothing.
    const bool raise = was != now && (was != Seen::Nothing || !alive);
    return {now, raise, alive};
}

void Announce(uint8_t slot, const coop::net::PoseSnapshot& pose, bool worldReady) {
    UE_ASSERT_GAME_THREAD("puppet_events::Announce");
    if (slot >= coop::players::kMaxPeers) return;
    const LifeStep step = StepLife(g_seen[slot], pose.healthFrac, worldReady);
    g_seen[slot] = step.now;
    if (step.raise)
        for (int i = 0; i < g_life.count; ++i) g_life.fn[i](slot, step.alive);
    if (!worldReady && !g_heldSaid[slot]) {
        // The premise of the gate, measured once per slot: the sender sends full health (255) until its save resolves
        // (coop/player/local_streams.cpp), which reads alive; this line shows what a pre-ready pose really carried.
        g_heldSaid[slot] = true;
        UE_LOGI("puppet_events: slot %u's first pose before its world is ready carries healthFrac %u -- held as unknown",
                static_cast<unsigned>(slot), static_cast<unsigned>(pose.healthFrac));
    }
    for (int i = 0; i < g_pose.count; ++i) g_pose.fn[i](slot, pose);
}

void ForgetSlot(uint8_t slot) {
    UE_ASSERT_GAME_THREAD("puppet_events::ForgetSlot");
    if (slot >= coop::players::kMaxPeers) return;
    g_seen[slot] = Seen::Nothing;
    g_heldSaid[slot] = false;
}

}  // namespace coop::puppet_events
