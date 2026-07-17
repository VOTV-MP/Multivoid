// coop/prop_stick_sync.cpp -- see coop/prop_stick_sync.h for the design + RE.

#include "coop/props/prop_stick_sync.h"

#include "coop/element/mirror_manager.h"
#include "coop/element/prop.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/props/prop_element_tracker.h"
#include "coop/props/remote_prop.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace coop::prop_stick_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;
namespace E  = ue_wrap::engine;
namespace PT = coop::prop_element_tracker;

std::atomic<coop::net::Session*> g_session{nullptr};
std::atomic<bool> g_installed{false};
// Signature drift (recooked BP): installed-but-REFUSING. g_installed alone
// stops the install retries; without this second latch OnStickState would
// proceed and write frame[g_skipHoldingOff] past its 16-byte buffer on a
// stick from a peer whose game build still has the old signature.
std::atomic<bool> g_disabled{false};

coop::net::Session* LoadSession() {
    return g_session.load(std::memory_order_acquire);
}

// ---- resolved engine refs (written by Install on the game thread BEFORE the
// observer registers; read-only afterwards).
void* g_compClass        = nullptr;  // comp_wallAttachable_C
void* g_wallAttachClass  = nullptr;  // prop_wallAttachable_C (the owning prop lineage)
void* g_uberFn           = nullptr;  // comp_wallAttachable_C::ExecuteUbergraph_comp_wallAttachable
void* g_forceStickFn     = nullptr;  // comp_wallAttachable_C::forceStick(bool skipHolding)
int32_t g_entryParamOff  = -1;       // ExecuteUbergraph 'EntryPoint' int32 param offset
int32_t g_skipHoldingOff = -1;       // forceStick 'skipHolding' bool param offset
int32_t g_compPropOff    = -1;       // comp_wallAttachable_C::prop (Aprop_C*) field offset
int32_t g_propCompOff    = -1;       // prop_wallAttachable_C::comp_wallAttachable field offset
// NOTE deliberately NO prop init() dispatch anywhere in this module: init is
// overridden along the camera lineage (resolving the right override per
// instance is the kerfur PickDropPropFn problem again), and its only load-
// bearing effect for stick/unstick is the SetSimulatePhysics(NOT(static||
// frozen||sleep)) recompute -- which SetActorSimulatePhysics performs
// directly. The full-parity path (forceStick replay) runs the real BP's own
// init internally anyway; only the raw fallback + UnstickForDrive use the
// direct toggle.

// The commit's ubergraph entry: offset 45 in ExecuteUbergraph_comp_wallAttachable
// (kismet ground truth, header). The byte offset is part of the cooked BP the
// same way the keypad/door entry constants are; a game update that recooks the
// BP shifts it -- the install log prints it so a silent no-fire is diagnosable.
constexpr int32_t kStickCommitEntry = 45;

// ---- commit-pending list -------------------------------------------------------
// The POST observer (ProcessEvent dispatch thread -- possibly a parallel-anim
// worker) only RECORDS the commit; Tick() (game thread) verifies + broadcasts
// on the NEXT net-pump pass. Same record-then-act shape as kerfur_convert.
//
// Deliberately NO settle delay (audit v68 finding 1; was kSettleMs=450 to
// sample the post-glide pose): frozen/static are already final when the POST
// observer fires (the commit body ran inside the observed dispatch), the
// commit pose is exactly where the trace succeeded (the receiver only needs
// it to pre-position its own re-trace), and the receiver's forceStick replay
// re-derives the settled pose + plays its own glide/VFX. Any delay re-opens
// the release-beats-stick window: the sender's hold breaks 0-100 ms after
// the commit, and its PropRelease must NOT reach peers before the
// PropStickState (same reliable lane = FIFO -- session_lanes.h pins the
// pair). Draining next pass makes the order STRUCTURAL: TickGameplay (this
// drain) runs before local_streams' release edge within one net-pump pass,
// and the hold-break can never precede the commit.
struct PendingStick {
    void*   prop = nullptr;
    int32_t internalIdx = -1;
};
constexpr int kMaxPending = 8;
std::mutex g_pendingMutex;
PendingStick g_pending[kMaxPending];
int g_pendingCount = 0;

// POST observer on ExecuteUbergraph_comp_wallAttachable. Fires only for
// LATENT resumes (PE-dispatched): the 10 Hz sticking() poll while a wall-
// attachable is held, the re-arm entries, and the stick COMMIT (45). Cheap
// EntryPoint gate first; memory reads + the leaf mutex only (observer
// thread contract).
void OnCompUbergraphPost(void* self, void* /*function*/, void* params) {
    if (!self || !params || g_entryParamOff < 0 || g_compPropOff < 0) return;
    const int32_t entry =
        *reinterpret_cast<const int32_t*>(reinterpret_cast<uint8_t*>(params) + g_entryParamOff);
    if (entry != kStickCommitEntry) return;
    void* prop = *reinterpret_cast<void* const*>(
        reinterpret_cast<uint8_t*>(self) + g_compPropOff);
    if (!prop) return;
    auto* s = LoadSession();
    if (!s || !s->running() || !s->connected()) return;  // SP: nothing to mirror
    PendingStick ps{};
    ps.prop = prop;
    ps.internalIdx = R::InternalIndexOf(prop);  // live: it is the comp's owner mid-dispatch
    std::lock_guard<std::mutex> lk(g_pendingMutex);
    if (g_pendingCount >= kMaxPending) {
        static std::atomic<uint32_t> sDropped{0};
        UE_LOGW("prop_stick_sync: commit-pending full -- dropping commit (#%u)",
                sDropped.fetch_add(1, std::memory_order_relaxed) + 1);
        return;
    }
    // Dedupe a re-fired commit for the same prop still settling (the trace
    // can re-enter 45 after a failed first pass) -- keep the earliest stamp.
    for (int i = 0; i < g_pendingCount; ++i) {
        if (g_pending[i].prop == prop) return;
    }
    g_pending[g_pendingCount++] = ps;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_installed.load(std::memory_order_acquire)) return;
    // FindClass/FindFunction walk GUObjectArray -- throttle like the sibling
    // installs. No give-up cap: cameras/whiteboards can be acquired mid-game.
    static uint32_t sResolveN = 0;
    if ((sResolveN++ % 125) != 0) return;

    if (!g_compClass)       g_compClass       = R::FindClass(L"comp_wallAttachable_C");
    if (!g_wallAttachClass) g_wallAttachClass = R::FindClass(L"prop_wallAttachable_C");
    if (!g_compClass || !g_wallAttachClass) return;  // BP classes not loaded yet

    if (!g_uberFn) {
        g_uberFn = R::FindFunction(g_compClass, L"ExecuteUbergraph_comp_wallAttachable");
        if (g_uberFn) {
            g_entryParamOff = R::FindParamOffset(g_uberFn, L"EntryPoint");
        }
    }
    if (!g_forceStickFn) {
        g_forceStickFn = R::FindFunction(g_compClass, L"forceStick");
        if (g_forceStickFn) {
            g_skipHoldingOff = R::FindParamOffset(g_forceStickFn, L"skipHolding");
        }
    }
    if (g_compPropOff < 0) g_compPropOff = R::FindPropertyOffset(g_compClass, L"prop");
    if (g_propCompOff < 0)
        g_propCompOff = R::FindPropertyOffset(g_wallAttachClass, L"comp_wallAttachable");

    if (!g_uberFn || g_entryParamOff < 0 || !g_forceStickFn || g_skipHoldingOff < 0 ||
        g_compPropOff < 0 || g_propCompOff < 0) {
        UE_LOGW("prop_stick_sync: partial resolve (uber=%p entryOff=%d force=%p skipOff=%d compPropOff=%d propCompOff=%d) -- retrying",
                g_uberFn, g_entryParamOff, g_forceStickFn, g_skipHoldingOff,
                g_compPropOff, g_propCompOff);
        return;
    }
    const int32_t forceFrame = R::FunctionFrameSize(g_forceStickFn);
    if (g_skipHoldingOff >= 16 || forceFrame > 16) {
        // forceStick's dispatch frame is a 16-byte zeroed buffer; a param
        // offset OR a PropertiesSize past it means the signature changed
        // (game update) -- refuse rather than over-write (the offset) or let
        // ProcessEvent memcpy past our buffer (the frame size; the
        // DrivePropThrown house pattern).
        UE_LOGE("prop_stick_sync: forceStick signature drift (skipHoldingOff=%d frameSize=%d vs 16-byte frame) -- module DISABLED (re-RE comp_wallAttachable)",
                g_skipHoldingOff, forceFrame);
        g_disabled.store(true, std::memory_order_release);
        g_installed.store(true, std::memory_order_release);  // latch off
        return;
    }
    if (!GT::RegisterPostObserver(g_uberFn, &OnCompUbergraphPost)) {
        UE_LOGE("prop_stick_sync: RegisterPostObserver failed (table full?)");
        return;
    }
    g_installed.store(true, std::memory_order_release);
    UE_LOGI("prop_stick_sync: installed (commit entry %d, entryOff=%d, comp.prop@%d, prop.comp@%d, skipHoldingOff=%d)",
            kStickCommitEntry, g_entryParamOff, g_compPropOff, g_propCompOff, g_skipHoldingOff);
}

void Tick() {
    // Drain every commit recorded since the last pass. Game thread; MUST run
    // before local_streams' release edge in the pump pass (header note on the
    // stick-before-release ordering).
    PendingStick ready[kMaxPending];
    int nReady = 0;
    {
        std::lock_guard<std::mutex> lk(g_pendingMutex);
        if (g_pendingCount == 0) return;
        nReady = g_pendingCount;
        for (int i = 0; i < g_pendingCount; ++i) ready[i] = g_pending[i];
        g_pendingCount = 0;
    }
    auto* s = LoadSession();
    for (int i = 0; i < nReady; ++i) {
        void* prop = ready[i].prop;
        if (!prop || !R::IsLiveByIndex(prop, ready[i].internalIdx)) continue;  // died since the commit
        const bool frozen  = ue_wrap::prop::IsFrozen(prop);
        const bool statiq  = ue_wrap::prop::IsStatic(prop);
        if (!frozen && !statiq) continue;  // commit bailed (the 45 body re-traces and can exit before the flag write)
        if (!s || !s->connected()) continue;
        coop::net::PropStickStatePayload p{};
        const std::wstring keyW = ue_wrap::prop::GetInteractableKeyString(prop);
        p.key.len = 0;
        if (!keyW.empty() && keyW != L"None") {
            for (size_t k = 0; k < keyW.size() && k < 31; ++k)
                p.key.data[p.key.len++] = static_cast<char>(keyW[k]);
        }
        const coop::element::ElementId eid = PT::GetPropElementIdForActor(prop);
        p.elementId = (eid == coop::element::kInvalidId) ? 0u : static_cast<uint32_t>(eid);
        if (p.key.len == 0 && p.elementId == 0) {
            UE_LOGW("prop_stick_sync: stuck prop %p has neither key nor eid -- not broadcast", prop);
            continue;
        }
        p.flags = (frozen ? 1u : 0u) | (statiq ? 2u : 0u);
        const auto loc = E::GetActorLocation(prop);
        const auto rot = E::GetActorRotation(prop);
        p.locX = loc.X; p.locY = loc.Y; p.locZ = loc.Z;
        p.rotPitch = rot.Pitch; p.rotYaw = rot.Yaw; p.rotRoll = rot.Roll;
        s->SendReliable(coop::net::ReliableKind::PropStickState, &p, sizeof(p));
        UE_LOGI("prop_stick_sync: broadcast STICK key='%ls' eid=%u flags=%u pose=(%.0f,%.0f,%.0f)",
                keyW.c_str(), p.elementId, p.flags, p.locX, p.locY, p.locZ);
    }
}

void OnStickState(const coop::net::PropStickStatePayload& payload,
                  uint8_t senderPeerSlot) {
    // Game thread (event_feed drain). Resolve key-first, eid fallback (the
    // PropDestroy shape).
    if (!g_installed.load(std::memory_order_acquire)) return;
    if (g_disabled.load(std::memory_order_acquire)) {
        static std::atomic<bool> sWarned{false};
        if (!sWarned.exchange(true))
            UE_LOGW("prop_stick_sync: STICK received while signature-disabled -- dropping (peer game builds differ)");
        return;
    }
    const std::wstring keyW = coop::remote_prop::KeyToWString(payload.key);
    void* prop = nullptr;
    if (!keyW.empty() && keyW != L"None")
        prop = PT::ResolveLiveActorByKey(keyW);
    if (!prop && payload.elementId != 0) {
        auto* el = coop::element::MirrorManager<coop::element::Prop>::Instance().Get(
            static_cast<coop::element::ElementId>(payload.elementId));
        if (el) {
            void* a = el->GetActor();
            if (a && R::IsLiveByIndex(a, el->GetInternalIdx())) prop = a;
        }
    }
    if (!prop) {
        UE_LOGW("prop_stick_sync: STICK for key='%ls' eid=%u -- no local match (slot %u)",
                keyW.c_str(), payload.elementId, senderPeerSlot);
        return;
    }
    if (!IsWallAttachable(prop)) {
        UE_LOGW("prop_stick_sync: STICK target %p is not a wall-attachable -- dropped", prop);
        return;
    }
    // 1. Stop any kinematic drive on it (the sticking peer was holding it, so
    //    its PropPose stream was driving our copy). Cache clear only -- no
    //    physics re-enable (that is exactly the falling bug).
    coop::remote_prop::ClearAnyDriveFor(prop);
    // 2. Pre-pose at the sender's commit transform (where its stick trace
    //    succeeded) so the SP replay's re-trace scans the same surface; the
    //    replay's own glide settles the final pose.
    ue_wrap::FVector  loc{payload.locX, payload.locY, payload.locZ};
    ue_wrap::FRotator rot{payload.rotPitch, payload.rotYaw, payload.rotRoll};
    E::SetActorLocation(prop, loc);
    E::SetActorRotation(prop, rot);
    // 3. SP replay: re-enable simulate (the BP's canStick precondition -- the
    //    drive had it kinematic), then dispatch the comp's OWN forceStick
    //    (skipHolding=true): SP performs the field write + KeepWorld attach +
    //    OnDestroyed binding + eff_OC_freeze VFX + glide. The receiver-side
    //    forceStick enters the ubergraph LOCALLY -- our own POST observer does
    //    NOT fire (no echo by construction).
    E::SetActorSimulatePhysics(prop, true);
    void* comp = *reinterpret_cast<void* const*>(
        reinterpret_cast<uint8_t*>(prop) + g_propCompOff);
    if (comp) {
        uint8_t frame[16] = {};
        frame[g_skipHoldingOff] = 1;  // skipHolding=true (nobody holds the mirror)
        R::CallFunction(comp, g_forceStickFn, frame);
    }
    const bool stuck = ue_wrap::prop::IsFrozen(prop) || ue_wrap::prop::IsStatic(prop);
    if (!stuck) {
        // 4. Trace divergence (different geometry state on this peer) -- the
        //    raw fallback: write the flagged field + physics off + re-pose.
        //    This is SP's own save-load degraded mode (frozen at pose,
        //    un-attached); the direct simulate toggle IS init()'s only
        //    relevant effect here (header note on the init overrides).
        if (payload.flags & 2u) ue_wrap::prop::WriteStatic(prop, true);
        else                    ue_wrap::prop::WriteFrozen(prop, true);
        E::SetActorSimulatePhysics(prop, false);
        E::SetActorLocation(prop, loc);
        E::SetActorRotation(prop, rot);
        UE_LOGW("prop_stick_sync: forceStick replay diverged for key='%ls' -- raw frozen-write fallback applied",
                keyW.c_str());
    } else {
        UE_LOGI("prop_stick_sync: STICK applied key='%ls' eid=%u (SP replay, slot %u)",
                keyW.c_str(), payload.elementId, senderPeerSlot);
    }
}

bool IsWallAttachable(void* actor) {
    if (!actor || !g_wallAttachClass) return false;
    void* cls = R::ClassOf(actor);
    return cls && R::IsDescendantOfAny(cls, &g_wallAttachClass, 1);
}

bool UnstickForDrive(void* actor) {
    if (!actor) return false;
    const bool frozen = ue_wrap::prop::IsFrozen(actor);
    const bool statiq = ue_wrap::prop::IsStatic(actor);
    if (!frozen && !statiq) return false;
    // The SP unstick shape (comp_wallAttachable.unstick = clear flags +
    // init()), with the simulate recompute applied directly (header note on
    // the init overrides): SetSimulatePhysics(true) also detaches the
    // attached root (UE4.27 behavior the BP's own unstick relies on). The
    // caller immediately re-disables simulate for its kinematic drive.
    ue_wrap::prop::WriteFrozen(actor, false);
    ue_wrap::prop::WriteStatic(actor, false);
    E::SetActorSimulatePhysics(actor, true);
    UE_LOGI("prop_stick_sync: UNSTUCK %p for incoming drive (sustained re-grab stream)", actor);
    return true;
}

void OnDisconnect() {
    std::lock_guard<std::mutex> lk(g_pendingMutex);
    g_pendingCount = 0;
}

}  // namespace coop::prop_stick_sync
