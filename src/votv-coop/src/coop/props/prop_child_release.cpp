// coop/props/prop_child_release.cpp -- see coop/props/prop_child_release.h.

#include "coop/props/prop_child_release.h"

#include "coop/props/active_drive.h"      // NowMs
#include "coop/props/prop_drive_host.h"   // InAHand: a hand took the door while it waited
#include "coop/props/prop_park.h"         // Unpark
#include "ue_wrap/actors/prop.h"          // GetStaticMesh
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/engine.h"

#include <cstddef>
#include <vector>

namespace coop::prop_child_release {
namespace {

namespace E = ue_wrap::engine;

// A door parked at its release pose, and the parent whose drive it waits out. Both held by slot and serial: a
// wait whose door or parent died is found dead and dropped.
struct Wait {
    ue_wrap::CachedObjRef door;
    ue_wrap::CachedObjRef parent;
    ue_wrap::FVector loc{};
    ue_wrap::FRotator rot{};
    bool hasPose = false;
    uint64_t sinceMs = 0;
};
std::vector<Wait> g_waits;

// Counters for the log: the first of each and every 100th.
uint32_t g_deferred = 0, g_afterParent = 0, g_boundHit = 0;
bool Said(uint32_t& n) { return ++n == 1 || n % 100 == 0; }

// The door goes back to the pose it was released at and simulates again, as the swinger branch of the release
// does when no parent holds it.
void Let(Wait& w, void* door) {
    if (w.hasPose) {
        E::SetActorLocation(door, w.loc);
        E::SetActorRotation(door, w.rot);
    }
    coop::prop_park::Unpark(door, ue_wrap::prop::GetStaticMesh(door), /*rootSimulates=*/true);
}

}  // namespace

void Defer(void* door, void* parent) {
    UE_ASSERT_GAME_THREAD("prop_child_release::Defer");
    if (!door || !parent) return;
    Wait* w = nullptr;
    for (Wait& x : g_waits)
        if (x.door.Raw() == door) { w = &x; break; }
    if (!w) { g_waits.emplace_back(); w = &g_waits.back(); }
    w->door.Set(door);
    w->parent.Set(parent);
    w->hasPose = E::TryGetActorLocation(door, w->loc) && E::TryGetActorRotation(door, w->rot);
    w->sinceMs = coop::active_drive::NowMs();
    if (Said(g_deferred))
        UE_LOGI("[SWING-CHILD] door %p stays parked at its release pose until its parent %p is let go (x%u)",
                door, parent, g_deferred);
}

void Forget(void* door) {
    UE_ASSERT_GAME_THREAD("prop_child_release::Forget");
    for (size_t i = 0; i < g_waits.size(); ++i) {
        if (g_waits[i].door.Raw() != door) continue;
        g_waits.erase(g_waits.begin() + static_cast<std::ptrdiff_t>(i));
        return;
    }
}

void OnParentReleased(void* parent) {
    UE_ASSERT_GAME_THREAD("prop_child_release::OnParentReleased");
    if (g_waits.empty() || !parent) return;
    for (size_t i = 0; i < g_waits.size();) {
        Wait& w = g_waits[i];
        if (w.parent.Raw() != parent) { ++i; continue; }
        void* const door = w.door.Get();
        // A hand that took the door meanwhile owns it: its release restores the physics.
        if (door && !coop::prop_drive_host::InAHand(door)) {
            Let(w, door);
            if (Said(g_afterParent))
                UE_LOGI("[SWING-CHILD] door %p let go after its parent %p (x%u)", door, parent, g_afterParent);
        }
        g_waits.erase(g_waits.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

void Tick(uint64_t nowMs) {
    UE_ASSERT_GAME_THREAD("prop_child_release::Tick");
    for (size_t i = 0; i < g_waits.size();) {
        Wait& w = g_waits[i];
        void* const door = w.door.Get();
        if (!door || !w.parent.Get() || coop::prop_drive_host::InAHand(door)) {
            g_waits.erase(g_waits.begin() + static_cast<std::ptrdiff_t>(i));   // dead, or a new hold owns the door
            continue;
        }
        if (nowMs - w.sinceMs >= kChildWaitMs) {
            UE_LOGW("[SWING-CHILD] door %p: its parent %p was not let go within %llu ms -- the door is let go "
                    "anyway (x%u)", door, w.parent.Raw(), static_cast<unsigned long long>(kChildWaitMs),
                    ++g_boundHit);
            Let(w, door);
            g_waits.erase(g_waits.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        ++i;
    }
}

void OnDisconnect() {
    UE_ASSERT_GAME_THREAD("prop_child_release::OnDisconnect");
    for (Wait& w : g_waits)
        if (void* const door = w.door.Get())
            coop::prop_park::Unpark(door, ue_wrap::prop::GetStaticMesh(door), /*rootSimulates=*/true);
    g_waits.clear();
}

}  // namespace coop::prop_child_release
