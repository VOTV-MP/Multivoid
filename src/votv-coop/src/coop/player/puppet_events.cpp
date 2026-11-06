// coop/player/puppet_events.cpp -- see coop/player/puppet_events.h.

#include "coop/player/puppet_events.h"

#include "coop/net/protocol.h"
#include "coop/player/players_registry.h"

#include "ue_wrap/core/hot_path_guard.h"

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

enum class Seen : uint8_t { Nothing, Alive, Dead };
std::array<Seen, coop::players::kMaxPeers> g_seen{};

}  // namespace

bool AddLifeListener(LifeFn fn) {
    UE_ASSERT_GAME_THREAD("puppet_events::AddLifeListener");
    return fn != nullptr && g_life.Add(fn);
}

bool AddPoseListener(PoseFn fn) {
    UE_ASSERT_GAME_THREAD("puppet_events::AddPoseListener");
    return fn != nullptr && g_pose.Add(fn);
}

void Announce(uint8_t slot, const coop::net::PoseSnapshot& pose) {
    UE_ASSERT_GAME_THREAD("puppet_events::Announce");
    if (slot >= coop::players::kMaxPeers) return;
    // Alive is the test the registry's LifeOf applies to the same stream: any positive health.
    const bool alive = pose.healthFrac > 0;
    const Seen now = alive ? Seen::Alive : Seen::Dead;
    const Seen was = g_seen[slot];
    g_seen[slot] = now;
    if (was != Seen::Nothing && was != now)
        for (int i = 0; i < g_life.count; ++i) g_life.fn[i](slot, alive);
    for (int i = 0; i < g_pose.count; ++i) g_pose.fn[i](slot, pose);
}

void ForgetSlot(uint8_t slot) {
    UE_ASSERT_GAME_THREAD("puppet_events::ForgetSlot");
    if (slot < coop::players::kMaxPeers) g_seen[slot] = Seen::Nothing;
}

}  // namespace coop::puppet_events
