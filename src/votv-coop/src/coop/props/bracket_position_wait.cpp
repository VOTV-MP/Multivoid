// coop/props/bracket_position_wait.cpp -- see coop/props/bracket_position_wait.h.

#include "coop/props/bracket_position_wait.h"

#include "remote_prop_drive.h"  // g_drives, g_holds, IsHoldClosed, ResolveAndStartDrive (private to coop/props)

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/element/bind_wait.h"
#include "coop/element/element.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_key_index.h"
#include "coop/props/prop_stick_sync.h"
#include "coop/props/remote_prop.h"
#include "coop/props/remote_prop_spawn.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace coop::props::bracket_position_wait {
namespace {

namespace BW = coop::element::bind_wait;
namespace EL = coop::element;
namespace R  = ue_wrap::reflection;

// A bracket that carries more waiting spawns than this binds the rest on arrival: every scan here is linear in it.
constexpr size_t kMaxWaiting = 256;

// One spawn kept for the bracket's end, with the newest release, held pose and stick state that named its key meanwhile.
struct Waiter {
    coop::net::PropSpawnPayload payload{};
    int                         slot = 0;

    bool                           haveRelease = false;
    coop::net::PropReleasePayload  release{};
    int                            releaseSlot = 0;
    uint32_t                       releaseOrder = 0;

    bool                           havePose = false;
    coop::net::PropPoseSnapshot    pose{};
    int                            poseSlot = 0;
    uint32_t                       poseOrder = 0;

    bool                           haveStick = false;
    coop::net::PropStickStatePayload stick{};
    int                            stickSlot = 0;
    uint32_t                       stickOrder = 0;
};

bool                 g_open = false;
bool                 g_fullSaid = false;   // the cap's warning, once per bracket
uint32_t             g_order = 0;      // arrival order of the kept messages
std::vector<Waiter>  g_waiters;        // in arrival order

// Never set in a shipped run: nothing waits, a position match binds on arrival (the red of the rekey arm).
bool Off() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::bracket_position_wait_off);
    return s;
}

bool SameKey(const coop::net::WireKey& a, const coop::net::WireKey& b) {
    return a.len == b.len && a.len <= sizeof(a.data) && std::memcmp(a.data, b.data, a.len) == 0;
}

bool HasEid(uint32_t eid) { return eid != 0 && eid != coop::element::kInvalidId; }

// A candidate is free for `payload` when no other wire eid owns it: the registry's row for the actor (one actor, one
// row) is not a wire mirror of another eid; a client's own census row does not own it. A kerfur prop's key is
// cross-peer stable, so a neighbour carrying another key is another kerfur, never adopted (the same gate as the
// arrival path's).
bool IsFree(void* actor, const coop::net::PropSpawnPayload& payload, const std::wstring& classW,
            const std::wstring& keyW) {
    const EL::ElementId owner = EL::Registry::Get().EidForActor(actor);
    if (owner != EL::kInvalidId && static_cast<uint32_t>(owner) != payload.elementId) {
        const EL::Element* row = EL::Registry::Get().Get(owner);
        if (row && row->IsMirror()) return false;
    }
    if (classW.find(L"prop_kerfurOmega") != std::wstring::npos) {
        const std::wstring own = ue_wrap::prop::GetKeyString(actor);
        if (!own.empty() && own != L"None" && own != keyW) return false;
    }
    return true;
}

struct Pair {
    float    distCm;
    uint32_t eid;
    int32_t  objIndex;
    size_t   waiter;
    void*    actor;
};

bool PairBefore(const Pair& a, const Pair& b) {
    if (a.distCm != b.distCm) return a.distCm < b.distCm;
    if (a.eid != b.eid) return a.eid < b.eid;
    return a.objIndex < b.objIndex;
}

void ReplayPose(const Waiter& w, const std::wstring& keyW) {
    const int slot = w.poseSlot;
    if (slot < 0 || slot >= static_cast<int>(coop::players::kMaxPeers)) return;
    // The release's own close is already in the hold gate: a pose of a closed hold starts nothing, and the kept release
    // (if it came after the pose) places the prop.
    if (coop::remote_prop::IsHoldClosed(coop::remote_prop::g_holds[slot], w.pose.holdGen)) {
        UE_LOGI("bracket_position_wait: the kept pose of '%ls' belongs to a hold that closed -- not replayed", keyW.c_str());
        return;
    }
    if (coop::remote_prop::g_drives[slot].actor) {
        UE_LOGI("bracket_position_wait: slot %d drives another prop -- the kept pose of '%ls' is not replayed", slot,
                keyW.c_str());
        return;
    }
    UE_LOGI("bracket_position_wait: replaying the kept pose of '%ls' (slot %d)", keyW.c_str(), slot);
    coop::remote_prop::ResolveAndStartDrive(slot, w.pose);
}

}  // namespace

void BeginBracket() {
    Reset();
    g_open = !Off();
}

bool IsWaiting() { return g_open; }

size_t WaitingCount() { return g_waiters.size(); }

bool Wait(const coop::net::PropSpawnPayload& payload, int senderSlot) {
    for (Waiter& w : g_waiters) {
        if (w.payload.elementId != payload.elementId) continue;
        w.payload = payload;
        w.slot = senderSlot;
        return true;
    }
    if (g_waiters.size() >= kMaxWaiting) {
        if (!g_fullSaid) {
            g_fullSaid = true;
            UE_LOGW("bracket_position_wait: %zu spawns wait already -- the rest of this bracket bind on arrival",
                    g_waiters.size());
        }
        return false;
    }
    Waiter w;
    w.payload = payload;
    w.slot = senderSlot;
    g_waiters.push_back(std::move(w));
    const uint32_t eid = payload.elementId;
    // An eid with a row here is bound already: nothing waits on its bind.
    if (HasEid(eid) && !coop::element::Registry::Get().Get(eid)) BW::MarkDeferred(eid, "spawn position wait");
    UE_LOGI("bracket_position_wait: eid=%u key='%ls' waits for the bracket's end (%zu waiting)", eid,
            coop::remote_prop::KeyToWString(payload.key).c_str(), g_waiters.size());
    return true;
}

bool OnDestroy(uint32_t eid, const coop::net::WireKey& key) {
    if (g_waiters.empty()) return false;
    const bool byEid = HasEid(eid);
    for (auto it = g_waiters.begin(); it != g_waiters.end(); ++it) {
        if (byEid ? (it->payload.elementId != eid) : !SameKey(it->payload.key, key)) continue;
        const uint32_t waitingEid = it->payload.elementId;
        UE_LOGI("bracket_position_wait: eid=%u key='%ls' destroyed while it waited -- the wait is dropped", waitingEid,
                coop::remote_prop::KeyToWString(it->payload.key).c_str());
        g_waiters.erase(it);
        if (HasEid(waitingEid)) BW::GiveUp(waitingEid, "spawn position wait destroyed");
        return true;
    }
    return false;
}

bool KeepIfWaiting(const coop::net::WireKey& key, Kept what, const void* bytes, size_t n, int senderSlot) {
    if (g_waiters.empty()) return false;
    for (Waiter& w : g_waiters) {
        if (!SameKey(w.payload.key, key)) continue;
        if (what == Kept::Release && n == sizeof(w.release)) {
            std::memcpy(&w.release, bytes, n);
            w.haveRelease = true;
            w.releaseSlot = senderSlot;
            w.releaseOrder = ++g_order;
            return true;
        }
        if (what == Kept::Pose && n == sizeof(w.pose)) {
            std::memcpy(&w.pose, bytes, n);
            w.havePose = true;
            w.poseSlot = senderSlot;
            w.poseOrder = ++g_order;
            return true;
        }
        if (what == Kept::Stick && n == sizeof(w.stick)) {
            std::memcpy(&w.stick, bytes, n);
            w.haveStick = true;
            w.stickSlot = senderSlot;
            w.stickOrder = ++g_order;
            return true;
        }
        return false;
    }
    return false;
}

void DrainAtComplete(void* localPlayer) {
    g_open = false;   // before any bind: a spawn the drain replays must not wait again
    std::vector<Waiter> waiters;
    waiters.swap(g_waiters);
    const auto t0 = std::chrono::steady_clock::now();

    // The query reads the object index as it stands.
    if (!waiters.empty()) ue_wrap::object_index::DrainAll();
    void* handAxis[1 + coop::players::kMaxPeers];
    const ue_wrap::prop::NearbyExclusion notAdoptable{
        handAxis, coop::hand_item::CollectHandAxisActors(handAxis, 1 + coop::players::kMaxPeers)};

    std::vector<std::wstring> keys(waiters.size());
    std::vector<std::wstring> classes(waiters.size());
    for (size_t i = 0; i < waiters.size(); ++i) {
        keys[i] = coop::remote_prop::KeyToWString(waiters[i].payload.key);
        classes[i] = coop::remote_prop_spawn::ClassNameToWString(waiters[i].payload.className);
    }

    // The ordinary spawn path (the exact key, the kerfur deferral, a fresh mirror). A spawn that left no row behind and
    // armed no deferral of its own (a kerfur prop arms its own) gives up the mark the wait put on its eid.
    std::vector<char> bound(waiters.size(), 0);
    std::vector<void*> taken;
    size_t fresh = 0, exact = 0;
    auto ordinary = [&](size_t i) {
        const coop::net::PropSpawnPayload& p = waiters[i].payload;
        coop::remote_prop_spawn::OnSpawn(p, waiters[i].slot, localPlayer);
        if (HasEid(p.elementId) && !EL::Registry::Get().Get(p.elementId) &&
            classes[i].find(L"prop_kerfurOmega") == std::wstring::npos)
            BW::GiveUp(p.elementId, "bracket wait: the leftover did not bind");
    };

    // A key that resolves now (a keyless twin's key restored since the spawn waited) binds exactly, before any
    // position is weighed: nearest-first would rekey a nearer neighbour and leave two actors on one key.
    for (size_t i = 0; i < waiters.size(); ++i) {
        if (keys[i].empty() || keys[i] == L"None") continue;
        void* holder = coop::prop_key_index::Find(keys[i]);
        if (!holder) continue;
        bound[i] = 1;
        taken.push_back(holder);
        ++exact;
        ordinary(i);
    }

    // Every (spawn, free candidate) pair, then the nearest first: a twin at d=0 of a per-peer-key population binds to
    // its own spawn before a neighbour at 13 cm can take it.
    std::vector<Pair> pairs;
    for (size_t i = 0; i < waiters.size(); ++i) {
        if (bound[i]) continue;
        const coop::net::PropSpawnPayload& p = waiters[i].payload;
        const std::wstring propName = coop::remote_prop::KeyToWString(p.propName);
        ue_wrap::prop::NearbyTrace trace;
        ue_wrap::prop::FindNearbySameClass(classes[i], ue_wrap::FVector{p.locX, p.locY, p.locZ},
                                           coop::remote_prop_spawn::kFuzzyRadiusCm,
                                           propName == L"None" ? std::wstring() : propName, notAdoptable, &trace);
        for (const ue_wrap::prop::NearbyCandidate& c : trace.candidates)
            if (IsFree(c.actor, p, classes[i], keys[i]))
                pairs.push_back(Pair{c.distCm, p.elementId, c.objIndex, i, c.actor});
    }
    std::sort(pairs.begin(), pairs.end(), PairBefore);

    size_t adopted = 0;
    for (const Pair& pr : pairs) {
        if (bound[pr.waiter] || std::find(taken.begin(), taken.end(), pr.actor) != taken.end()) continue;
        // An earlier adopt ran engine calls: a candidate that died meanwhile is skipped, its waiter takes the next.
        if (!R::IsLiveByIndex(pr.actor, pr.objIndex)) continue;
        bound[pr.waiter] = 1;
        taken.push_back(pr.actor);
        ++adopted;
        coop::remote_prop_spawn::AdoptByPosition(waiters[pr.waiter].payload, waiters[pr.waiter].slot, pr.actor,
                                                 localPlayer);
    }
    // No free candidate left: the ordinary spawn path.
    for (size_t i = 0; i < waiters.size(); ++i) {
        if (bound[i]) continue;
        ++fresh;
        ordinary(i);
    }

    // What named a waiting key meanwhile, in the order it arrived.
    enum class What : uint8_t { Release, Pose, Stick };
    struct Kept1 { uint32_t order; size_t waiter; What what; };
    std::vector<Kept1> kept;
    for (size_t i = 0; i < waiters.size(); ++i) {
        if (waiters[i].haveRelease) kept.push_back(Kept1{waiters[i].releaseOrder, i, What::Release});
        if (waiters[i].havePose) kept.push_back(Kept1{waiters[i].poseOrder, i, What::Pose});
        if (waiters[i].haveStick) kept.push_back(Kept1{waiters[i].stickOrder, i, What::Stick});
    }
    std::sort(kept.begin(), kept.end(), [](const Kept1& a, const Kept1& b) { return a.order < b.order; });
    for (const Kept1& k : kept) {
        const Waiter& w = waiters[k.waiter];
        switch (k.what) {
        case What::Release:
            UE_LOGI("bracket_position_wait: replaying the kept release of '%ls' (slot %d)", keys[k.waiter].c_str(),
                    w.releaseSlot);
            coop::remote_prop::OnRelease(w.releaseSlot, w.release, localPlayer, /*replayed=*/true);
            break;
        case What::Pose:
            ReplayPose(w, keys[k.waiter]);
            break;
        case What::Stick:
            UE_LOGI("bracket_position_wait: replaying the kept stick state of '%ls' (slot %d)", keys[k.waiter].c_str(),
                    w.stickSlot);
            coop::prop_stick_sync::OnStickState(w.stick, static_cast<uint8_t>(w.stickSlot));
            break;
        }
    }

    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
    UE_LOGI("bracket_position_wait: drained %zu spawns, adopted %zu, fresh %zu, exact %zu in %lld us", waiters.size(),
            adopted, fresh, exact, static_cast<long long>(us));
}

void Reset() {
    g_open = false;
    g_fullSaid = false;
    std::vector<Waiter> waiters;
    waiters.swap(g_waiters);
    for (const Waiter& w : waiters)
        if (HasEid(w.payload.elementId)) BW::GiveUp(w.payload.elementId, "bracket wait reset");
    g_order = 0;
}

}  // namespace coop::props::bracket_position_wait
