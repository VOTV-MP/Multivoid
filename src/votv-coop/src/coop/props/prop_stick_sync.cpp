// coop/props/prop_stick_sync.cpp -- see coop/props/prop_stick_sync.h for the design + RE.

#include "coop/props/prop_stick_sync.h"

#include "coop/element/mirror_manager.h"
#include "coop/element/prop.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/local_streams.h"  // CurrentHoldGen, the hold a stick ends
#include "coop/props/prop_drive_stream.h"  // IsParked: the host's channel moves an unstuck copy
#include "coop/props/prop_element_tracker.h"
#include "coop/props/remote_prop.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_physics.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/types.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace coop::prop_stick_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;
namespace E  = ue_wrap::engine;
namespace PT = coop::prop_element_tracker;
namespace sg = ue_wrap::script_gate;

std::atomic<coop::net::Session*> g_session{nullptr};
// The two halves latch on their own. Signature drift (a recooked BP) latches a half
// installed-but-REFUSING: the install stops retrying, and without the second latch a received
// message would write its frame past the 16-byte buffer on a peer whose game build still has the
// old signature.
std::atomic<bool> g_stickInstalled{false};
std::atomic<bool> g_stickDisabled{false};
std::atomic<bool> g_unstickInstalled{false};
std::atomic<bool> g_unstickDisabled{false};

coop::net::Session* LoadSession() {
    return g_session.load(std::memory_order_acquire);
}

// ---- resolved engine refs, written on the game thread by the install -- the throttled Install, or
// on demand when a join's converge or a live message needs the lane first -- before the observer
// and the watch register; read-only afterwards. The component class can be taken from a live
// component before either (WallAttachCompOf).
void* g_compClass        = nullptr;  // comp_wallAttachable_C
void* g_uberFn           = nullptr;  // comp_wallAttachable_C::ExecuteUbergraph_comp_wallAttachable
void* g_forceStickFn     = nullptr;  // comp_wallAttachable_C::forceStick(bool skipHolding)
void* g_unstickFn        = nullptr;  // comp_wallAttachable_C::unstick(bool withTool)
int32_t g_entryParamOff  = -1;       // ExecuteUbergraph 'EntryPoint' int32 param offset
int32_t g_skipHoldingOff = -1;       // forceStick 'skipHolding' bool param offset
int32_t g_withToolOff    = -1;       // unstick 'withTool' bool param offset
int32_t g_compPropOff    = -1;       // comp_wallAttachable_C::prop (Aprop_C*) field offset
// NOTE this module never dispatches a prop's init() itself: init is overridden
// along the camera lineage, and the component's own verbs -- the forceStick
// replay and the unstick -- run the right override inside the Blueprint.
// Only the raw fallback writes the flags and applies the simulate recompute
// (SetSimulatePhysics(NOT(static||frozen||sleep))) directly.

// The commit's ubergraph entry: offset 45 in ExecuteUbergraph_comp_wallAttachable, from
// the kismet bytecode. The byte offset is part of the cooked BP the same way the keypad
// and door entry constants are; a game update that recooks the BP shifts it, and the
// install log prints it so a silent no-fire is diagnosable.
constexpr int32_t kStickCommitEntry = 45;

constexpr int kTagUnstick = 0x554E5354;  // 'UNST'

// Inside ReplayUnstick: the copy's own unstick is this peer taking another's outcome, and is not
// mirrored back. Game thread, as the gate's callbacks are.
int g_replayDepth = 0;

// The offset of each class's comp_wallAttachable variable, or -1, resolved once per class. Every
// owner the game has names the component so: the wall-attachable lineage and the plasma TV, whose
// grab preludes both call comp_wallAttachable->unstick. The class is held by slot and serial, so a
// class freed and another loaded at its address resolves again. Game thread.
struct CompOffset {
    ue_wrap::CachedObjRef cls;
    int32_t off = -1;
};
std::unordered_map<void*, CompOffset> g_compOffsets;

// The on-demand install runs once a session; the throttled Install keeps retrying after it.
bool g_onDemandTried = false;

// The wall-attach component `actor` carries, checked for its class, or null. Before the throttled
// install has found the class, a live component names it: a joiner's first snapshot can arrive
// before that install runs, and this takes the class without a walk of the object array.
void* WallAttachCompOf(void* actor) {
    UE_ASSERT_GAME_THREAD("prop_stick_sync::WallAttachCompOf");
    if (!actor) return nullptr;
    void* cls = R::ClassOf(actor);
    if (!cls) return nullptr;
    CompOffset& entry = g_compOffsets[cls];
    if (!entry.cls.Alive()) {
        entry.cls.Set(cls);
        entry.off = R::FindPropertyOffset(cls, L"comp_wallAttachable");
    }
    if (entry.off < 0) return nullptr;
    void* comp = *reinterpret_cast<void* const*>(reinterpret_cast<uint8_t*>(actor) + entry.off);
    if (!comp || !R::IsLive(comp)) return nullptr;
    void* compCls = R::ClassOf(comp);
    if (!compCls) return nullptr;
    if (!g_compClass) {
        if (R::ClassNameOf(comp) != L"comp_wallAttachable_C") return nullptr;
        g_compClass = compCls;
    }
    return R::IsDescendantOfAny(compCls, &g_compClass, 1) ? comp : nullptr;
}

// ---- pending list ----------------------------------------------------------------
// The observers only RECORD; Tick() (game thread) verifies + broadcasts on the NEXT net-pump pass.
// Same record-then-act shape as kerfur_convert.
//
// A stick: NO settle delay. frozen/static are already final when the POST observer fires (the
// commit body ran inside the observed dispatch), the commit pose is where the trace succeeded
// (the receiver only pre-positions its own re-trace), and the receiver's forceStick replay
// re-derives the settled pose and plays its own glide/VFX. A delay re-opens the
// release-beats-stick window: the hold breaks 0-100 ms after the commit and that PropRelease must
// not arrive before the PropStickState. GNS orders reliable delivery WITHIN a lane, so the pair is
// FIFO only because LaneForKind pins both to Lane::Normal (see the note on that pin in
// coop/net/session_lanes.h). Draining next pass makes the order structural as well: TickGameplay
// runs before local_streams' release edge.
struct Pending {
    void*   prop = nullptr;
    int32_t internalIdx = -1;
    bool    unstick = false;
};
constexpr int kMaxPending = 8;
std::mutex g_pendingMutex;
Pending g_pending[kMaxPending];
int g_pendingCount = 0;

void Record(void* prop, bool unstick) {
    Pending p{};
    p.prop = prop;
    p.internalIdx = R::InternalIndexOf(prop);  // live: it is the comp's owner mid-dispatch
    p.unstick = unstick;
    std::lock_guard<std::mutex> lk(g_pendingMutex);
    // Dedupe a re-fired record for the same prop and edge still settling (the trace can re-enter
    // 45 after a failed first pass) -- keep the earliest.
    for (int i = 0; i < g_pendingCount; ++i) {
        if (g_pending[i].prop == prop && g_pending[i].unstick == unstick) return;
    }
    if (g_pendingCount >= kMaxPending) {
        static std::atomic<uint32_t> sDropped{0};
        UE_LOGW("prop_stick_sync: pending full -- dropping a %s (#%u)", unstick ? "unstick" : "stick commit",
                sDropped.fetch_add(1, std::memory_order_relaxed) + 1);
        return;
    }
    g_pending[g_pendingCount++] = p;
}

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
    Record(prop, /*unstick=*/false);
}

// The script gate's pre on comp_wallAttachable_C::unstick, whatever called it: a stuck prop is
// recorded, and the next pass broadcasts it if the body freed it. That pass is also after whatever
// the calling Blueprint did next, which for a pry is the kick (crowbarOpen sets the angular velocity
// once unstick returns), so the broadcast carries the velocity the prop really left the wall with.
// A prop already free has nothing to mirror, and neither has this peer's own replay.
sg::Verdict OnUnstickPre(const sg::Call& c) {
    if (g_replayDepth > 0 || !c.object || g_compPropOff < 0) return sg::Verdict::Run;
    void* prop = *reinterpret_cast<void* const*>(reinterpret_cast<uint8_t*>(c.object) + g_compPropOff);
    if (!prop || !(ue_wrap::prop::IsFrozen(prop) || ue_wrap::prop::IsStatic(prop))) return sg::Verdict::Run;
    auto* s = LoadSession();
    if (!s || !s->running() || !s->connected()) return sg::Verdict::Run;
    Record(prop, /*unstick=*/true);
    return sg::Verdict::Run;
}

// The prop by key, its eid as the fallback, and its pose, for either edge. False when the prop has
// neither key nor eid.
bool FillIdentityAndPose(coop::net::PropStickStatePayload& p, void* prop, std::wstring& keyW) {
    keyW = ue_wrap::prop::GetInteractableKeyString(prop);
    p.key.len = 0;
    if (!keyW.empty() && keyW != L"None") {
        for (size_t k = 0; k < keyW.size() && k < 31; ++k)
            p.key.data[p.key.len++] = static_cast<char>(keyW[k]);
    }
    const coop::element::ElementId eid = PT::GetPropElementIdForActor(prop);
    p.elementId = (eid == coop::element::kInvalidId) ? 0u : static_cast<uint32_t>(eid);
    if (p.key.len == 0 && p.elementId == 0) return false;
    const auto loc = E::GetActorLocation(prop);
    const auto rot = E::GetActorRotation(prop);
    p.locX = loc.X; p.locY = loc.Y; p.locZ = loc.Z;
    p.rotPitch = rot.Pitch; p.rotYaw = rot.Yaw; p.rotRoll = rot.Roll;
    return true;
}

void InstallStickHalf() {
    if (!g_uberFn) {
        g_uberFn = R::FindFunction(g_compClass, L"ExecuteUbergraph_comp_wallAttachable");
        if (g_uberFn) g_entryParamOff = R::FindParamOffset(g_uberFn, L"EntryPoint");
    }
    if (!g_forceStickFn) {
        g_forceStickFn = R::FindFunction(g_compClass, L"forceStick");
        if (g_forceStickFn) g_skipHoldingOff = R::FindParamOffset(g_forceStickFn, L"skipHolding");
    }
    if (!g_uberFn || g_entryParamOff < 0 || !g_forceStickFn || g_skipHoldingOff < 0) {
        static bool sSaid = false;
        if (!sSaid) {
            sSaid = true;
            UE_LOGW("prop_stick_sync: partial stick resolve (uber=%p entryOff=%d force=%p skipOff=%d) -- retrying",
                    g_uberFn, g_entryParamOff, g_forceStickFn, g_skipHoldingOff);
        }
        return;
    }
    const int32_t forceFrame = R::FunctionFrameSize(g_forceStickFn);
    if (g_skipHoldingOff >= 16 || forceFrame > 16) {
        // forceStick's dispatch frame is a 16-byte zeroed buffer; a param offset OR a
        // PropertiesSize past it means the signature changed (game update) -- refuse rather than
        // over-write (the offset) or let ProcessEvent memcpy past our buffer (the frame size; the
        // house pattern of ue_wrap/engine/engine_physics).
        UE_LOGE("prop_stick_sync: forceStick signature drift (skipHoldingOff=%d frameSize=%d vs a 16-byte "
                "frame) -- stick half DISABLED (re-RE comp_wallAttachable)", g_skipHoldingOff, forceFrame);
        g_stickDisabled.store(true, std::memory_order_release);
        g_stickInstalled.store(true, std::memory_order_release);  // latch off
        return;
    }
    if (!GT::RegisterPostObserver(g_uberFn, &OnCompUbergraphPost)) {
        static bool sSaid = false;
        if (!sSaid) {
            sSaid = true;
            UE_LOGE("prop_stick_sync: RegisterPostObserver failed (table full?) -- retrying");
        }
        return;
    }
    g_stickInstalled.store(true, std::memory_order_release);
    UE_LOGI("prop_stick_sync: stick half installed (commit entry %d, entryOff=%d, comp.prop@%d, skipHoldingOff=%d)",
            kStickCommitEntry, g_entryParamOff, g_compPropOff, g_skipHoldingOff);
}

void InstallUnstickHalf() {
    if (!g_unstickFn) {
        g_unstickFn = R::FindFunction(g_compClass, L"unstick");
        if (g_unstickFn) g_withToolOff = R::FindParamOffset(g_unstickFn, L"withTool");
    }
    if (!g_unstickFn || g_withToolOff < 0) {
        static bool sSaid = false;
        if (!sSaid) {
            sSaid = true;
            UE_LOGW("prop_stick_sync: partial unstick resolve (unstick=%p toolOff=%d) -- retrying", g_unstickFn,
                    g_withToolOff);
        }
        return;
    }
    const int32_t unstickFrame = R::FunctionFrameSize(g_unstickFn);
    if (g_withToolOff >= 16 || unstickFrame > 16) {
        UE_LOGE("prop_stick_sync: unstick signature drift (withToolOff=%d frameSize=%d vs a 16-byte frame) -- "
                "unstick half DISABLED (re-RE comp_wallAttachable)", g_withToolOff, unstickFrame);
        g_unstickDisabled.store(true, std::memory_order_release);
        g_unstickInstalled.store(true, std::memory_order_release);  // latch off
        return;
    }
    if (!sg::Watch(g_unstickFn, kTagUnstick, &OnUnstickPre, nullptr)) {
        static bool sSaid = false;
        if (!sSaid) {
            sSaid = true;
            UE_LOGW("prop_stick_sync: the script gate refused the unstick watch -- retrying");
        }
        return;
    }
    sg::SetEnabled(true);  // each lane asserts its own enable
    g_unstickInstalled.store(true, std::memory_order_release);
    UE_LOGI("prop_stick_sync: unstick half installed (withToolOff=%d, a script-gate watch)", g_withToolOff);
}

// The component's pieces and each half not yet installed. Needs the class. Game thread.
void InstallHalves() {
    if (!g_compClass) return;
    if (g_compPropOff < 0) g_compPropOff = R::FindPropertyOffset(g_compClass, L"prop");
    if (g_compPropOff < 0) {
        static bool sSaid = false;
        if (!sSaid) {
            sSaid = true;
            UE_LOGW("prop_stick_sync: comp_wallAttachable_C::prop did not resolve -- retrying");
        }
        return;
    }
    if (!g_stickInstalled.load(std::memory_order_acquire)) InstallStickHalf();
    if (!g_unstickInstalled.load(std::memory_order_acquire)) InstallUnstickHalf();
}

// A join's converge or a live message needs the lane before the throttled Install has run: one
// attempt a session, after which the throttle carries the retries. Game thread.
void InstallOnDemand() {
    if (g_onDemandTried) return;
    if (g_stickInstalled.load(std::memory_order_acquire) && g_unstickInstalled.load(std::memory_order_acquire))
        return;
    g_onDemandTried = true;
    InstallHalves();
}

// The component's own forceStick on this peer's copy at the pose it now has (skipHolding: nobody
// holds the copy), and the raw fallback when this peer's re-trace finds no surface there. True when
// the replay stuck it. Game thread.
bool ReplayStick(void* prop, void* comp, uint8_t flags, const std::wstring& keyW) {
    const ue_wrap::FVector loc = E::GetActorLocation(prop);
    const ue_wrap::FRotator rot = E::GetActorRotation(prop);
    // Simulate on first: the Blueprint's canStick precondition, which a drive or a park took away.
    E::SetActorSimulatePhysics(prop, true);
    uint8_t frame[16] = {};
    frame[g_skipHoldingOff] = 1;  // skipHolding=true (nobody holds the mirror)
    R::CallFunction(comp, g_forceStickFn, frame);
    if (ue_wrap::prop::IsFrozen(prop) || ue_wrap::prop::IsStatic(prop)) return true;
    // Trace divergence (different geometry state on this peer) -- the raw fallback: write the
    // flagged field + physics off + re-pose. This is SP's own save-load degraded mode (frozen at
    // pose, un-attached); the direct simulate toggle IS init()'s only relevant effect here (the note
    // on the init overrides).
    if (flags & 2u) ue_wrap::prop::WriteStatic(prop, true);
    else            ue_wrap::prop::WriteFrozen(prop, true);
    E::SetActorSimulatePhysics(prop, false);
    E::SetActorLocation(prop, loc);
    E::SetActorRotation(prop, rot);
    UE_LOGW("prop_stick_sync: forceStick replay diverged for key='%ls' -- raw frozen-write fallback applied",
            keyW.c_str());
    return false;
}

void BroadcastStick(coop::net::Session* s, void* prop, bool frozen, bool statiq) {
    coop::net::PropStickStatePayload p{};
    std::wstring keyW;
    if (!FillIdentityAndPose(p, prop, keyW)) {
        UE_LOGW("prop_stick_sync: stuck prop %p has neither key nor eid -- not broadcast", prop);
        return;
    }
    p.flags = (frozen ? 1u : 0u) | (statiq ? 2u : 0u);
    // The hold this stick ends, when the stuck prop is the one this peer holds (the release edge
    // runs after this pass); 0 closes nothing.
    p.holdGen = (coop::local_streams::LastHeldActor() == prop) ? coop::local_streams::CurrentHoldGen()
                                                                : uint16_t{0};
    s->SendReliable(coop::net::ReliableKind::PropStickState, &p, sizeof(p));
    UE_LOGI("prop_stick_sync: broadcast STICK key='%ls' eid=%u flags=%u pose=(%.0f,%.0f,%.0f)",
            keyW.c_str(), p.elementId, p.flags, p.locX, p.locY, p.locZ);
}

void BroadcastUnstick(coop::net::Session* s, void* prop) {
    coop::net::PropStickStatePayload p{};
    std::wstring keyW;
    if (!FillIdentityAndPose(p, prop, keyW)) {
        UE_LOGW("prop_stick_sync: unstuck prop %p has neither key nor eid -- not broadcast", prop);
        return;
    }
    p.flags = 0;
    p.holdGen = 0;  // an unstick ends no hold; a grab's starts one, which its poses carry
    const ue_wrap::prop::VelocityState v = ue_wrap::prop::GetPhysicsVelocity(prop);
    if (v.ok) {
        p.linVelX = v.linearCmS.X;   p.linVelY = v.linearCmS.Y;   p.linVelZ = v.linearCmS.Z;
        p.angVelX = v.angularDegS.X; p.angVelY = v.angularDegS.Y; p.angVelZ = v.angularDegS.Z;
    }
    s->SendReliable(coop::net::ReliableKind::PropStickState, &p, sizeof(p));
    UE_LOGI("prop_stick_sync: broadcast UNSTICK key='%ls' eid=%u pose=(%.0f,%.0f,%.0f) angVel=(%.0f,%.0f,%.0f) deg/s",
            keyW.c_str(), p.elementId, p.locX, p.locY, p.locZ, p.angVelX, p.angVelY, p.angVelZ);
}

// The copy is freed by the component's own unstick. Unless another owner moves it -- a peer's
// hold, the host's driven-prop channel, this peer's own grab -- it lets go from the unsticking
// peer's pose with that peer's velocity, a pry's kick included.
void ApplyUnstick(void* prop, const coop::net::PropStickStatePayload& p, const std::wstring& keyW,
                  uint8_t senderPeerSlot, void* localPlayer) {
    // A copy already free -- the hold's first pose ran the unstick before this message came -- gets no
    // second one: its init() would switch the copy's simulation on under the drive that owns it.
    const bool wasStuck = ue_wrap::prop::IsFrozen(prop) || ue_wrap::prop::IsStatic(prop);
    if (wasStuck && !ReplayUnstick(prop)) {
        UE_LOGW("prop_stick_sync: UNSTICK key='%ls' eid=%u -- the component's unstick did not run here; the copy "
                "stays as it is", keyW.c_str(), p.elementId);
        return;
    }
    const bool owned = coop::remote_prop::IsActorUnderAnyDrive(prop) || coop::prop_drive_stream::IsParked(prop) ||
                       (localPlayer && E::IsMainPlayerGrabbing(localPlayer, prop));
    if (!owned) {
        E::SetActorLocation(prop, ue_wrap::FVector{p.locX, p.locY, p.locZ});
        E::SetActorRotation(prop, ue_wrap::FRotator{p.rotPitch, p.rotYaw, p.rotRoll});
        if (void* mesh = ue_wrap::prop::GetStaticMesh(prop)) {
            E::SetComponentLinearVelocity(mesh, p.linVelX, p.linVelY, p.linVelZ);
            E::SetComponentAngularVelocity(mesh, p.angVelX, p.angVelY, p.angVelZ);
        }
    }
    UE_LOGI("prop_stick_sync: UNSTICK applied key='%ls' eid=%u (was %s here; %s; slot %u)", keyW.c_str(),
            p.elementId, wasStuck ? "stuck" : "already free",
            owned ? "another hold moves it" : "let go from the sender's pose", senderPeerSlot);
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_stickInstalled.load(std::memory_order_acquire) && g_unstickInstalled.load(std::memory_order_acquire))
        return;
    // FindClass/FindFunction walk GUObjectArray -- throttle like the sibling
    // installs. No give-up cap: cameras/whiteboards can be acquired mid-game.
    static uint32_t sResolveN = 0;
    if ((sResolveN++ % 125) != 0) return;

    if (!g_compClass) g_compClass = R::FindClass(L"comp_wallAttachable_C");
    if (!g_compClass) return;  // the BP class is not loaded yet
    InstallHalves();
}

void Tick() {
    // Drain every record since the last pass. Game thread; MUST run before local_streams' release
    // edge in the pump pass (header note on the stick-before-release ordering).
    Pending ready[kMaxPending];
    int nReady = 0;
    {
        std::lock_guard<std::mutex> lk(g_pendingMutex);
        if (g_pendingCount == 0) return;
        nReady = g_pendingCount;
        for (int i = 0; i < g_pendingCount; ++i) ready[i] = g_pending[i];
        g_pendingCount = 0;
    }
    auto* s = LoadSession();
    if (!s || !s->connected()) return;
    for (int i = 0; i < nReady; ++i) {
        void* prop = ready[i].prop;
        if (!prop || !R::IsLiveByIndex(prop, ready[i].internalIdx)) continue;  // died since the record
        const bool frozen = ue_wrap::prop::IsFrozen(prop);
        const bool statiq = ue_wrap::prop::IsStatic(prop);
        if (ready[i].unstick) {
            // Still stuck: the body refused, as a hand grab of a pried-on prop does (the "Tool
            // required" hint), and there is nothing to mirror.
            if (frozen || statiq) continue;
            BroadcastUnstick(s, prop);
        } else {
            if (!frozen && !statiq) continue;  // commit bailed (the 45 body re-traces and can exit before the flag write)
            BroadcastStick(s, prop, frozen, statiq);
        }
    }
}

void OnStickState(const coop::net::PropStickStatePayload& payload, uint8_t senderPeerSlot,
                  void* localPlayer) {
    // Game thread (event_feed drain). Resolve key-first, eid fallback (the
    // PropDestroy shape). A stick ends the sticking peer's hold whatever this peer makes of the
    // stick itself -- a copy it cannot resolve, a half that did not install -- so the hold closes
    // first, and a pose of it still in flight starts nothing here. An unstick carries 0 and closes
    // nothing.
    UE_ASSERT_GAME_THREAD("prop_stick_sync::OnStickState");
    coop::remote_prop::CloseHold(senderPeerSlot, payload.holdGen);
    const bool unstick = payload.flags == 0;
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
        UE_LOGW("prop_stick_sync: %s for key='%ls' eid=%u -- no local match (slot %u)",
                unstick ? "UNSTICK" : "STICK", keyW.c_str(), payload.elementId, senderPeerSlot);
        return;
    }
    void* comp = WallAttachCompOf(prop);  // takes the component class from the copy if nothing has yet
    if (!comp) {
        UE_LOGW("prop_stick_sync: %s target %p carries no wall-attach component -- dropped",
                unstick ? "UNSTICK" : "STICK", prop);
        return;
    }
    const std::atomic<bool>& installed = unstick ? g_unstickInstalled : g_stickInstalled;
    const std::atomic<bool>& disabled = unstick ? g_unstickDisabled : g_stickDisabled;
    if (!installed.load(std::memory_order_acquire)) InstallOnDemand();
    if (!installed.load(std::memory_order_acquire)) {
        static bool sSaid = false;
        if (!sSaid) {
            sSaid = true;
            UE_LOGW("prop_stick_sync: %s for key='%ls' before the lane installed -- dropped (said once)",
                    unstick ? "UNSTICK" : "STICK", keyW.c_str());
        }
        return;
    }
    if (disabled.load(std::memory_order_acquire)) {
        static bool sWarned = false;
        if (!sWarned) {
            sWarned = true;
            UE_LOGW("prop_stick_sync: %s received while signature-disabled -- dropping (peer game builds differ)",
                    unstick ? "UNSTICK" : "STICK");
        }
        return;
    }
    if (unstick) {
        ApplyUnstick(prop, payload, keyW, senderPeerSlot, localPlayer);
        return;
    }
    // 1. Stop any kinematic drive on it (the sticking peer was holding it, so its PropPose stream
    //    was driving our copy). Cache clear only -- no physics re-enable (that is exactly the falling
    //    bug).
    coop::remote_prop::ClearAnyDriveFor(prop);
    // 2. Pre-pose at the sender's commit transform (where its stick trace
    //    succeeded) so the SP replay's re-trace scans the same surface; the
    //    replay's own glide settles the final pose.
    ue_wrap::FVector  loc{payload.locX, payload.locY, payload.locZ};
    ue_wrap::FRotator rot{payload.rotPitch, payload.rotYaw, payload.rotRoll};
    E::SetActorLocation(prop, loc);
    E::SetActorRotation(prop, rot);
    // 3. SP replay: dispatch the comp's OWN forceStick (skipHolding=true): SP performs the field
    //    write + KeepWorld attach + OnDestroyed binding + eff_OC_freeze VFX + glide. The
    //    receiver-side forceStick enters the ubergraph LOCALLY -- our own POST observer does NOT
    //    fire (no echo by construction).
    if (ReplayStick(prop, comp, payload.flags, keyW))
        UE_LOGI("prop_stick_sync: STICK applied key='%ls' eid=%u (SP replay, slot %u)",
                keyW.c_str(), payload.elementId, senderPeerSlot);
}

void ConvergeStuck(void* actor, uint8_t physFlags) {
    namespace pf = coop::net::propspawn_flags;
    if (!(physFlags & pf::kLiveState) || !actor || !ue_wrap::prop::IsDescendantOfProp(actor)) return;
    const bool hostStuck = (physFlags & (pf::kFrozen | pf::kStatic)) != 0;
    const bool copyStuck = ue_wrap::prop::IsFrozen(actor) || ue_wrap::prop::IsStatic(actor);
    if (hostStuck == copyStuck) return;  // nearly every row: two field reads and out
    void* comp = WallAttachCompOf(actor);
    if (!comp) return;
    InstallOnDemand();  // a join's snapshot can arrive before the throttled install has run
    const std::wstring keyW = ue_wrap::prop::GetInteractableKeyString(actor);
    if (!hostStuck) {
        const bool ok = ReplayUnstick(actor);
        UE_LOGI("prop_stick_sync: join converge key='%ls' -- stuck here, free on the host: the component's unstick "
                "%s", keyW.c_str(), ok ? "ran" : "did not run");
        return;
    }
    if (!g_stickInstalled.load(std::memory_order_acquire) || g_stickDisabled.load(std::memory_order_acquire)) {
        UE_LOGW("prop_stick_sync: join converge key='%ls' -- stuck on the host, free here, and the stick half is "
                "not installed; the copy stays free", keyW.c_str());
        return;
    }
    const uint8_t flags = (physFlags & pf::kStatic) ? 2u : 1u;
    if (ReplayStick(actor, comp, flags, keyW))
        UE_LOGI("prop_stick_sync: join converge key='%ls' -- free here, stuck on the host: the component's "
                "forceStick ran at the host's pose", keyW.c_str());
}

bool IsWallAttachable(void* actor) {
    UE_ASSERT_GAME_THREAD("prop_stick_sync::IsWallAttachable");
    return WallAttachCompOf(actor) != nullptr;
}

bool ReplayUnstick(void* actor) {
    if (!actor || !g_unstickInstalled.load(std::memory_order_acquire) ||
        g_unstickDisabled.load(std::memory_order_acquire))
        return false;
    void* comp = WallAttachCompOf(actor);
    if (!comp) return false;
    unsigned char frame[16] = {};
    frame[g_withToolOff] = 1;
    ++g_replayDepth;
    const bool ok = R::CallFunction(comp, g_unstickFn, frame);
    --g_replayDepth;
    return ok;
}

void OnDisconnect() {
    g_onDemandTried = false;
    std::lock_guard<std::mutex> lk(g_pendingMutex);
    g_pendingCount = 0;
}

}  // namespace coop::prop_stick_sync
