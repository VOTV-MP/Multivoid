// coop/trash_collect_sync.cpp -- see coop/trash_collect_sync.h.
//
// One POST observer on trashBitsPile_C::playerTryToCollect. When a peer
// collects (press E), the BP has spawned an Aprop_C item and auto-grabbed it;
// we read that item off the collector's grabbing_actor, force-mint a stable
// Key on it (its BP UCS hasn't minted one yet), and broadcast a PropSpawn so
// every peer spawns a mirror. The existing held-prop pose stream (net_pump
// grabbing_actor path) then carries it into the collector's hands.

#include "coop/trash_collect_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/prop_element_tracker.h"
#include "coop/prop_synth_key.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"
#include "ue_wrap/prop.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/types.h"

#include <atomic>
#include <cstdint>
#include <string>

namespace coop::trash_collect_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;
namespace PT = coop::prop_element_tracker;

// Session pointer (atomic -- the POST observer can fire on a parallel-anim
// worker, same shape as garbage_sync).
std::atomic<coop::net::Session*> g_session{nullptr};
coop::net::Session* LoadSession() { return g_session.load(std::memory_order_acquire); }

// One-shot install latch + resolved UFunction / param offset.
std::atomic<bool> g_installed{false};
int32_t g_playerParamOff = -1;  // byte offset of the "Player" param in the frame

// Build + send a PropSpawn for the freshly-collected, auto-grabbed item so
// peers spawn a mirror; the held-prop pose stream then drives it into the
// collector's hands. Mirrors the prop_lifecycle Init-POST broadcast body, but
// force-mints the Aprop_C Key (the item is grabbed before its UCS mints one)
// and runs from the collect site rather than Init. Game thread only.
void BroadcastCollectedItem(void* item) {
    auto* s = LoadSession();
    if (!item || !s || !s->connected()) return;
    if (!R::IsLive(item)) return;
    // Dedupe vs the item's own Init-POST broadcast. That path skips on the None
    // key (which is the whole bug), so it normally hasn't fired -- but be safe.
    if (PT::HasProcessedInit(item)) return;

    const std::wstring cls = R::ClassNameOf(item);
    std::wstring keyStr = ue_wrap::prop::GetInteractableKeyString(item);
    keyStr = coop::prop_synth_key::EnsureKeyForBroadcast(item, keyStr, /*mintForAprop=*/true);
    if (keyStr.empty() || keyStr == L"None") {
        UE_LOGW("trash_collect: item %p cls='%ls' Key still None after force-mint -- cannot mirror",
                item, cls.c_str());
        return;
    }
    // Latch BEFORE the send so a re-entrant Init POST can't double-broadcast.
    PT::MarkProcessedInit(item);
    PT::MarkPropElement(item, keyStr, cls);

    coop::net::PropSpawnPayload p{};
    p.className.len = 0;
    for (size_t i = 0; i < cls.size() && i < 63; ++i)
        p.className.data[p.className.len++] = static_cast<char>(cls[i]);
    p.key.len = 0;
    for (size_t i = 0; i < keyStr.size() && i < 31; ++i)
        p.key.data[p.key.len++] = static_cast<char>(keyStr[i]);

    const ue_wrap::FVector  loc = ue_wrap::engine::GetActorLocation(item);
    const ue_wrap::FRotator rot = ue_wrap::engine::GetActorRotation(item);
    p.locX = loc.X; p.locY = loc.Y; p.locZ = loc.Z;
    p.rotPitch = ue_wrap::NormalizeAxis(rot.Pitch);
    p.rotYaw   = ue_wrap::NormalizeAxis(rot.Yaw);
    p.rotRoll  = ue_wrap::NormalizeAxis(rot.Roll);
    p.scaleX = p.scaleY = p.scaleZ = 1.f;
    p.physFlags = coop::net::propspawn_flags::kSimulatePhysics;
    if (ue_wrap::prop::IsHeavy(item))  p.physFlags |= coop::net::propspawn_flags::kIsHeavy;
    if (ue_wrap::prop::IsFrozen(item)) p.physFlags |= coop::net::propspawn_flags::kFrozen;
    p.initLinVelX = p.initLinVelY = p.initLinVelZ = 0.f;
    p.initAngVelX = p.initAngVelY = p.initAngVelZ = 0.f;
    {
        const coop::element::ElementId eid = PT::GetPropElementIdForActor(item);
        p.elementId = (eid == coop::element::kInvalidId) ? 0u : eid;
    }
    UE_LOGI("trash_collect: BROADCAST collected item cls='%ls' key='%ls' loc=(%.1f,%.1f,%.1f) "
            "-- held-pose stream now mirrors it into the collector's hands",
            cls.c_str(), keyStr.c_str(), p.locX, p.locY, p.locZ);
    s->SendPropSpawn(p);
}

// Read the collector's freshly-grabbed item out of the playerTryToCollect frame
// (params->Player -> grabbing_actor). Returns null if nothing is in hand yet.
void* ResolveCollectedItem(void* params) {
    if (g_playerParamOff < 0 || !params) return nullptr;
    void* player = *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(params) + g_playerParamOff);
    if (!player || !R::IsLive(player)) return nullptr;
    ue_wrap::engine::MainPlayerGrabState gs{};
    if (!ue_wrap::engine::ReadMainPlayerGrabState(player, gs)) return nullptr;
    // ONLY grabbing_actor (the PHC-grabbed Aprop_C the collect just spawned).
    // holding_actor is the TRANSIENT chip/clump morph carry -- broadcasting +
    // driving that on the peer is exactly the 2a use-after-free crash
    // ([[project-bug-trash-chippile-uaf-crash]]: chipPile/clump self-morph and
    // free). If grabbing_actor is null (deferred-grab window), return null and
    // let the miss counter log it -- NEVER fall back to holding_actor here.
    void* item = gs.grabbingActor;
    return (item && R::IsLive(item)) ? item : nullptr;
}

void OnPlayerTryToCollectPost(void* /*self*/, void* /*function*/, void* params) {
    if (!LoadSession()) return;
    void* item = ResolveCollectedItem(params);
    if (!item) {
        // Collect fired but nothing grabbed (empty pile, or the grab is a frame
        // late). Throttled so a held-down E doesn't spam the log.
        static std::atomic<uint64_t> sMiss{0};
        const uint64_t n = sMiss.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 3 || (n % 60) == 0)
            UE_LOGI("trash_collect: collect POST but no grabbed item (call #%llu) -- empty pile or deferred grab",
                    static_cast<unsigned long long>(n));
        return;
    }
    // BroadcastCollectedItem calls UFunctions (setKey / GetActorLocation) -- GT
    // only. Defer if the observer fired on a parallel-anim worker; re-validate
    // the captured pointer inside the deferred body.
    if (!GT::IsGameThread()) {
        GT::Post([item] { if (R::IsLive(item)) BroadcastCollectedItem(item); });
        return;
    }
    BroadcastCollectedItem(item);
}

}  // namespace

void SetSession(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void Install() {
    if (g_installed.load(std::memory_order_acquire)) return;
    void* cls = R::FindClass(L"trashBitsPile_C");
    if (!cls) return;  // BP class not loaded yet -- retry on the next Install()
    void* fn = R::FindFunction(cls, L"playerTryToCollect");
    if (!fn) {
        UE_LOGW("trash_collect: trashBitsPile_C loaded but playerTryToCollect not found -- CXX dump may be stale");
        return;
    }
    const int32_t off = R::FindParamOffset(fn, L"Player");
    if (off < 0) {
        UE_LOGW("trash_collect: 'Player' param not found on playerTryToCollect -- cannot resolve collected item");
        return;
    }
    g_playerParamOff = off;
    if (!GT::RegisterPostObserver(fn, &OnPlayerTryToCollectPost)) {
        UE_LOGE("trash_collect: RegisterPostObserver failed (observer table full?)");
        return;
    }
    g_installed.store(true, std::memory_order_release);
    UE_LOGI("trash_collect: installed -- trashBitsPile_C::playerTryToCollect POST observer (Player param @%d); trash-stack collect will now mirror",
            off);
}

}  // namespace coop::trash_collect_sync
