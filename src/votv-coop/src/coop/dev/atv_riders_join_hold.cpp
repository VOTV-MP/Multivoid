// coop/dev/atv_riders_join_hold.cpp -- see coop/dev/atv_riders_join_hold.h.

#include "coop/dev/atv_riders_join_hold.h"

#include "coop/config/config.h"
#include "coop/creatures/npc_mirror.h"
#include "coop/dev/atv_riders_arm.h"
#include "coop/element/bind_wait.h"
#include "coop/element/class_load_wait.h"
#include "coop/element/mirror_managers.h"
#include "coop/net/protocol.h"

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"

#include <atomic>
#include <cstddef>
#include <vector>

namespace coop::dev::atv_riders_join_hold {
namespace {

struct Parked {
    coop::net::EntitySpawnPayload payload;
    bool ours;   // the bind_wait mark was this park's own, not another lane's
};
std::vector<Parked> g_parked;   // game thread only

// Resolved on the first ask: both holds start held in the drill's rejoined life, free everywhere else.
struct Holds {
    std::atomic<bool> birth{false};
    std::atomic<bool> bind{false};
    Holds() {
        const bool on = RejoinedLife() && !coop::dev::atv_riders_arm::Is("off");
        birth.store(on);
        bind.store(on);
    }
};

Holds& H() {
    static Holds h;
    return h;
}

// The release's replay, on the game thread: each parked spawn runs again through the product's own path. A parked eid whose
// mark was cleared meanwhile (a world change, the session's end) is dropped. Run again, the spawn ends bound (its held
// messages are released), parked anew on a class load (which marked the eid itself) or not bound (its mark is given up).
void Replay() {
    UE_ASSERT_GAME_THREAD("atv_riders_join_hold::Replay");
    std::vector<Parked> replay;
    replay.swap(g_parked);
    for (const Parked& p : replay) {
        const uint32_t eid = p.payload.elementId;
        if (!coop::element::bind_wait::IsDeferred(eid)) {
            UE_LOGI("atv-riders-join-hold: fresh spawn eid=%u dropped (its bind mark was cleared while it waited)", eid);
            continue;
        }
        if (p.ours) coop::element::bind_wait::Unmark(eid);
        coop::npc_mirror::OnEntitySpawn(p.payload);
        if (coop::element::class_load_wait::IsParked("EntitySpawn", eid)) continue;
        if (coop::element::NpcMirrors().Get(eid) != nullptr) coop::element::bind_wait::OnBound(eid);
        else if (p.ours && !coop::element::bind_wait::IsDeferred(eid))
            coop::element::bind_wait::GiveUp(eid, "its spawn did not bind once the riders drill released it");
    }
}

}  // namespace

bool RejoinedLife() {
    static const bool rejoined = coop::config::ReadEnv("VOTVCOOP_ATV_RIDERS_REJOIN") == "1";
    return rejoined;
}

bool BirthHeld(uint8_t slot) { return slot == kDriverSlot && H().birth.load(std::memory_order_acquire); }

bool BindHeld() { return H().bind.load(std::memory_order_acquire); }

bool ParkSpawn(const coop::net::EntitySpawnPayload& payload) {
    if (!BindHeld()) return false;
    UE_ASSERT_GAME_THREAD("atv_riders_join_hold::ParkSpawn");  // no-mutex: g_parked is GT-only
    for (Parked& held : g_parked) {
        if (held.payload.elementId != payload.elementId) continue;
        held.payload = payload;
        // The host's connect broadcast sends the set again: a mark cleared since the first park (Clear) is taken anew.
        if (!coop::element::bind_wait::IsDeferred(payload.elementId)) {
            coop::element::bind_wait::MarkDeferred(payload.elementId, "the riders drill holds its bind");
            held.ours = true;
        }
        return true;
    }
    const bool ours = !coop::element::bind_wait::IsDeferred(payload.elementId);
    coop::element::bind_wait::MarkDeferred(payload.elementId, "the riders drill holds its bind");
    g_parked.push_back(Parked{payload, ours});
    UE_LOGI("atv-riders-join-hold: fresh spawn eid=%u held until the riders drill releases the binds", payload.elementId);
    return true;
}

bool DropParked(uint32_t eid) {
    if (g_parked.empty()) return false;
    UE_ASSERT_GAME_THREAD("atv_riders_join_hold::DropParked");
    for (size_t i = 0; i < g_parked.size(); ++i) {
        if (g_parked[i].payload.elementId != eid) continue;
        // Erased, so the replay (posted by the release, not yet run) skips it: no mirror is made for a destroyed entity. A
        // mirror that a re-announced spawn made directly after the release is not this payload's: the normal destroy takes it.
        g_parked.erase(g_parked.begin() + static_cast<std::ptrdiff_t>(i));
        coop::element::bind_wait::GiveUp(eid, "destroyed while the riders drill held its bind");
        return coop::element::NpcMirrors().Get(eid) == nullptr;
    }
    return false;
}

void Clear() {
    UE_ASSERT_GAME_THREAD("atv_riders_join_hold::Clear");
    g_parked.clear();
}

void ReleaseBirth() { H().birth.store(false, std::memory_order_release); }

void ReleaseBind() {
    H().bind.store(false, std::memory_order_release);
    ue_wrap::game_thread::Post(Replay);
}

}  // namespace coop::dev::atv_riders_join_hold
