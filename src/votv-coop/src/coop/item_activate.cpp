// coop/item_activate.cpp -- Phase 5F flashlight (and future item-activation)
// sync. See coop/item_activate.h for the public interface and the RE doc
// at research/findings/votv-flashlight-RE-2026-05-25.md for the full
// rationale (Case-b verdict, save-persistence gap, etc).
//
// Implementation shape mirrors coop/grab_observer.cpp:
//   1) Install() resolves mainPlayer_C + updateFlashlight + caches a
//      per-class hash. Idempotent + retried every NetPumpTick.
//   2) The POST observer is the SENDER path. It reads mp.flashlight
//      AFTER the BP function ran (so the bool reflects the new state)
//      and pushes an ItemActivatePayload onto the reliable channel.
//   3) ApplyToPuppet() is the RECEIVER path -- invoked from event_feed
//      ::Update's drain loop with the puppet's actor pointer.

#include "coop/item_activate.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/players_registry.h"
#include "dev/common.h"
#include "ue_wrap/call.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"
#include "ue_wrap/types.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <unordered_map>

namespace coop::item_activate {
namespace {

namespace P = ue_wrap::profile;
namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;
namespace GT = ue_wrap::game_thread;

bool g_installed = false;

// Resolved once at Install: pointers to BOTH candidate UFunctions in the
// flashlight call chain. Hands-on 2026-05-25 NIGHT-3 found that
// `updateFlashlight` is BP-INLINED into `Flashlight Update` and never
// reaches ProcessEvent -- so we register a POST observer on both, and
// whichever the BP compiler actually dispatches wins. If both fire in
// the same press (which the inlining argument says won't happen), the
// receiver tolerates the duplicate (writing the same bool twice = no-op
// + at human-press cadence the doubled wire traffic is negligible).
void* g_updateFlashlightFn = nullptr;
void* g_flashlightUpdateFn = nullptr;
void* g_flashlightInput13Fn = nullptr;
void* g_flashlightInput14Fn = nullptr;

// Last sent state so the input-event hook doesn't send a packet for
// the F-RELEASE event (which fires the InpActEvt_14 UFunction but
// doesn't change `mp.flashlight`). One value per local process is
// enough; the sender is always the local player. -1 = no send yet.
std::atomic<int> g_lastSentState{-1};

// Latched "on" intensity. Sampled from the LOCAL player's light_R the
// first time we observe it greater than the off-state default (which
// is 0.2 in Unitless mode -- VOTV's flashlight). The receiver uses
// this to drive the puppet's light_R intensity.
//
// CRITICAL: VOTV's light_R uses ELightUnits::Unitless (offset 0x0328
// on ULocalLightComponent). In that mode Intensity is a small
// multiplier (we see 0.2 in default state; real "on" is roughly
// 5-10). Stock UE4.27 default of 5000 lumens DOES NOT APPLY -- a
// 5000 Unitless multiplier is wildly different. The g_intensityOnFallback
// is set to a safe Unitless value used only until the latch sees a
// real on value.
//
// 0 = not yet sampled. Cross-peer assumption: the "on" intensity is
// the same on both peers because both run the same VOTV BP defaults
// (the latch will reach the SAME value on each peer once each peer's
// user toggles their own flashlight even once).
std::atomic<float> g_latchedOnIntensity{0.f};
inline constexpr float kIntensityOnFallback = 5.f;  // Unitless safe default
inline constexpr float kIntensityOffDefault = 0.f;  // turn light fully off

// Hash of "prop_equipment_flashlight_C" -- the class the wire packet's
// itemClassHash field carries for flashlight events. Both _a and _b
// variants use this same hash (they're identical from the world-effect
// perspective; the puppet's light_R toggles either way). Resolved at
// Install + then constant.
uint32_t g_flashlightClassHash = 0;

// Session pointer (atomic so the observer's BG read can't race with
// a setter on another thread -- same pattern as dev::teleport_client).
std::atomic<coop::net::Session*> g_session{nullptr};

// Echo-suppression: when we APPLY a remote flashlight state to the
// puppet, we don't directly invoke updateFlashlight (which would
// re-dispatch and re-broadcast); we write the bool + toggle the
// component directly. But future generalizations might invoke a
// UFunction on the puppet, so the flag is here pre-emptively. Set
// before invoking, cleared after. The observer checks it and skips
// the send. Atomic for the same reason as g_session above.
std::atomic<bool> g_echoSuppress{false};

bool ProbeLogEnabled() {
    // Read once; ini parsing is cheap but the observer is hot. Static
    // initialization means we resolve this ONCE per process lifetime,
    // which is acceptable for a dev-only flag (restart to flip it).
    static const bool s_enabled = ::dev::IsIniKeyTrue("flashlight_log");
    return s_enabled;
}

void OnUpdateFlashlightPost(void* self, void* function, void* /*params*/) {
    if (!self) return;
    // Identify which of the two candidate hooks fired -- diagnostic in
    // probe mode so we can prove which BP entry actually dispatches.
    const char* which =
        (function == g_flashlightUpdateFn) ? "Flashlight_Update" :
        (function == g_updateFlashlightFn) ? "updateFlashlight" :
        (function == g_flashlightInput13Fn) ? "InpActEvt_13" :
        (function == g_flashlightInput14Fn) ? "InpActEvt_14" :
        "<unknown>";

    // Echo-suppress: a receiver-applied state change going through any
    // future path that invokes updateFlashlight on the puppet should
    // NOT bounce back as a wire packet.
    if (g_echoSuppress.load(std::memory_order_acquire)) return;

    // Skip puppets entirely. If updateFlashlight ever fires on an
    // orphan puppet (it shouldn't -- puppets have no controller, no
    // input bindings), we'd send the puppet's spurious state back to
    // the very peer that authored it. GetController()!=nullptr is
    // the local-vs-puppet discriminator per CLAUDE.md.
    if (!E::GetController(self)) return;

    // Read state AFTER the BP function ran -- the bool now reflects
    // the new on/off value.
    const bool flashlight = *reinterpret_cast<bool*>(
        reinterpret_cast<uint8_t*>(self) + P::off::AmainPlayer_flashlight);
    const bool hasFlashlight = *reinterpret_cast<bool*>(
        reinterpret_cast<uint8_t*>(self) + P::off::AmainPlayer_hasFlashlight);
    const bool crankFlashlight = *reinterpret_cast<bool*>(
        reinterpret_cast<uint8_t*>(self) + P::off::AmainPlayer_crankFlashlight);

    if (ProbeLogEnabled()) {
        // [probe] flashlight_log=1 hands-on verification: see whether
        // the BP early-returns when !hasFlashlight (the bool should
        // not change in that case) and confirm light_R visibility is
        // in lockstep with the bool. The crank-flashlight (_c) path
        // is logged separately because we currently skip the wire
        // send for it (see below).
        void* light_R = *reinterpret_cast<void**>(
            reinterpret_cast<uint8_t*>(self) + P::off::AmainPlayer_light_R);
        bool lightVisible = false;
        float lightIntensity = -1.f;
        if (light_R && R::IsLive(light_R)) {
            const uint8_t flagsByte = *reinterpret_cast<uint8_t*>(
                reinterpret_cast<uint8_t*>(light_R) + P::off::USceneComponent_VisFlagsByte);
            lightVisible = (flagsByte & 0x10) != 0;
            lightIntensity = *reinterpret_cast<float*>(
                reinterpret_cast<uint8_t*>(light_R) + P::off::ULightComponentBase_Intensity);
        }
        UE_LOGI("flashlight[POST %s] self=%p flashlight=%d hasFL=%d crankFL=%d "
                "light_R=%p bVisible=%d Intensity=%.1f", which, self,
                flashlight ? 1 : 0, hasFlashlight ? 1 : 0,
                crankFlashlight ? 1 : 0, light_R, lightVisible ? 1 : 0,
                lightIntensity);
    }

    // Defer the _c crank lantern variant (its own light components on the
    // item actor; the player's light_R is NOT what toggles for it) --
    // covered by Inc6 of the 5F plan.
    if (crankFlashlight) {
        if (ProbeLogEnabled()) {
            UE_LOGI("flashlight: crank lantern (_c) -- wire send deferred to Inc6");
        }
        return;
    }

    // Don't broadcast if the BP guard early-returned because no
    // flashlight is equipped. Without this guard a future re-cook
    // could have updateFlashlight() always set flashlight=true on
    // F-press and we'd happily ship spurious activations.
    if (!hasFlashlight) {
        if (ProbeLogEnabled()) {
            UE_LOGI("flashlight: hasFlashlight=false -- BP early-returned, no wire send");
        }
        return;
    }

    // Latch the local "on" intensity the first time we see it clearly
    // above the off-state value. VOTV's flashlight Unitless default in
    // off state is ~0.2 (not 0 -- a sentinel "barely visible" value).
    // The on state is much higher (Unitless scale, roughly 5-10 per
    // Agent 1's research). Require >1.0 to filter out the off-state
    // sentinel.
    if (flashlight) {
        void* light_R = *reinterpret_cast<void**>(
            reinterpret_cast<uint8_t*>(self) + P::off::AmainPlayer_light_R);
        if (light_R && R::IsLive(light_R)) {
            const float intensity = *reinterpret_cast<float*>(
                reinterpret_cast<uint8_t*>(light_R) + P::off::ULightComponentBase_Intensity);
            if (intensity > 1.f && g_latchedOnIntensity.load(std::memory_order_acquire) == 0.f) {
                g_latchedOnIntensity.store(intensity, std::memory_order_release);
                UE_LOGI("flashlight: latched local 'on' intensity = %.2f (will drive puppet)", intensity);
            }
        }
    }

    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;

    // Dedup: if InpActEvt_14 (release) fires after InpActEvt_13 (press),
    // mp.flashlight didn't change between them -- last-sent-state catches
    // that and skips the redundant send. Also catches a future BP recook
    // where some other handler fires repeatedly with the same state.
    const int newState = flashlight ? 1 : 0;
    if (g_lastSentState.exchange(newState, std::memory_order_acq_rel) == newState) {
        if (ProbeLogEnabled()) {
            UE_LOGI("flashlight: state unchanged (still %d) -- no wire send", newState);
        }
        return;
    }

    coop::net::ItemActivatePayload p{};
    p.itemClassHash = g_flashlightClassHash;
    // 1v1 session: host=0, client=1. Once we scale to N peers, this gets
    // replaced with a real session id allocated by the host.
    p.peerSessionId = (s->role() == coop::net::Role::Host) ? 0 : 1;
    p.state = flashlight ? 1 : 0;
    p.flags = 0;          // Case (b) -- no actor key
    p._pad = 0;
    p.actorKeyHash = 0;
    // p.paramBlob stays zero.

    const bool sent = s->SendReliable(coop::net::ReliableKind::ItemActivate, &p, sizeof(p));
    if (!sent) {
        UE_LOGW("flashlight: SendReliable failed (channel busy or not connected)");
    } else {
        UE_LOGI("flashlight: sent state=%d (peer=%u)", p.state, p.peerSessionId);
    }
}

}  // namespace

bool DebugForceToggle(void* mp) {
    if (!mp) return false;

    // Field flip MUST run on the game thread (UObject memory). The
    // wire-send retry that follows can sleep, so it CANNOT run on the
    // game thread or it would block the ack pump and deadlock the
    // reliable channel. So: GT::Post the flip, wait for it, then
    // retry the send from THIS (worker) thread.
    //
    // The autotest already calls DebugForceToggle from its worker
    // thread (NOT inside a GT::Post), so this layering works.
    //
    // 2026-05-26: also drive the LOCAL light_R.Intensity so the
    // SENDER's flashlight is visually on too -- the BP toggle path is
    // gated by reflection-untouchable input state, so calling
    // Flashlight Update / updateFlashlight via reflection produces no
    // visible toggle. Writing flashlight bool + Intensity is the same
    // pair of fields the BP would have written; we just skip the BP
    // graph. This makes the autotest's wire path also visually exercise
    // both peers' lights end-to-end.
    auto done = std::make_shared<std::atomic<int>>(0);
    auto newStateOut = std::make_shared<std::atomic<bool>>(false);
    GT::Post([mp, done, newStateOut] {
        bool* fl = reinterpret_cast<bool*>(
            reinterpret_cast<uint8_t*>(mp) + P::off::AmainPlayer_flashlight);
        const bool newState = !(*fl);
        *fl = newState;

        // Drive the local light_R's Intensity to match the new state.
        // Mirrors the BP's toggle effect so the SENDER also visually
        // toggles. Use the same SetIntensity reflection path the
        // puppet receiver uses (MarkRenderStateDirty internally).
        void* light_R = *reinterpret_cast<void**>(
            reinterpret_cast<uint8_t*>(mp) + P::off::AmainPlayer_light_R);
        float localTarget = kIntensityOffDefault;
        if (newState) {
            localTarget = g_latchedOnIntensity.load(std::memory_order_acquire);
            if (localTarget == 0.f) localTarget = kIntensityOnFallback;
        }
        static void* sLocalSetIntensityFn = nullptr;
        if (!sLocalSetIntensityFn) {
            void* lightCls = R::FindClass(L"LightComponent");
            if (lightCls) sLocalSetIntensityFn = R::FindFunction(lightCls, P::name::SetIntensityFn);
        }
        if (light_R && R::IsLive(light_R) && sLocalSetIntensityFn) {
            ue_wrap::ParamFrame f(sLocalSetIntensityFn);
            f.Set<float>(L"NewIntensity", localTarget);
            ue_wrap::Call(light_R, f);
        }
        // Also latch the on-value so the receiver gets the same intensity
        // we're using locally (instead of falling back to 5.0).
        if (newState && localTarget > 1.f) {
            g_latchedOnIntensity.store(localTarget, std::memory_order_release);
        }
        newStateOut->store(newState, std::memory_order_release);
        UE_LOGI("flashlight: DebugForceToggle wrote flashlight=%d Intensity=%.2f on %p (LAN test path)",
                newState ? 1 : 0, localTarget, mp);
        done->store(1, std::memory_order_release);
    });
    while (done->load(std::memory_order_acquire) == 0) ::Sleep(1);
    const bool newState = newStateOut->load(std::memory_order_acquire);

    // The reliable channel is stop-and-wait (max 1 in-flight). The HOST
    // has continuous PropSpawn / EntitySpawn traffic from Phase 5S0
    // continuous-spawn observers, so any SendReliable can land while
    // the channel is busy and return false. The autonomous test can't
    // tolerate that -- we need the packet to fly. Retry up to 200x
    // with 25 ms backoff (5 s max wait per toggle) -- on LAN ack RTT
    // is ~1-2 ms so a free slot opens within a few tries, but if the
    // host's PropSpawn snapshot is mid-burst we may need longer.
    //
    // Bypass the dedup (last-sent-state) for retry purposes: directly
    // call SendReliable here with the exact same payload shape the
    // observer would have sent. The receiver path is identical.
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) {
        UE_LOGW("flashlight: DebugForceToggle session not connected -- skipping wire send");
        return newState;
    }

    coop::net::ItemActivatePayload p{};
    p.itemClassHash = g_flashlightClassHash;
    p.peerSessionId = (s->role() == coop::net::Role::Host) ? 0 : 1;
    p.state = newState ? 1 : 0;
    p.flags = 0;
    p._pad = 0;
    p.actorKeyHash = 0;

    bool sent = false;
    int attempts = 0;
    for (; attempts < 200; ++attempts) {
        sent = s->SendReliable(coop::net::ReliableKind::ItemActivate, &p, sizeof(p));
        if (sent) break;
        ::Sleep(25);
    }
    if (sent) {
        // Update the dedup tracker so the regular observer path (if any
        // BP-driven fire happens later) doesn't re-send the same value.
        g_lastSentState.store(p.state, std::memory_order_release);
        UE_LOGI("flashlight: DebugForceToggle sent state=%d (peer=%u, after %d retr%s)",
                p.state, p.peerSessionId, attempts,
                attempts == 1 ? "y" : "ies");
    } else {
        UE_LOGW("flashlight: DebugForceToggle FAILED to send after 200 retries -- "
                "reliable channel never freed (state=%d)", p.state);
    }
    return newState;
}

uint32_t HashClassName(const wchar_t* s) {
    // FNV-1a 32-bit. Operates byte-by-byte on the UTF-16 bytes (each
    // wchar_t = 2 bytes on Windows). Cross-peer stable because the
    // string is the same UTF-16 encoding everywhere.
    uint32_t h = 0x811c9dc5u;
    if (!s) return h;
    for (; *s; ++s) {
        const wchar_t w = *s;
        h ^= static_cast<uint8_t>(w & 0xFF);
        h *= 0x01000193u;
        h ^= static_cast<uint8_t>((w >> 8) & 0xFF);
        h *= 0x01000193u;
    }
    return h;
}

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_installed) return;

    void* playerCls = R::FindClass(P::name::MainPlayerClass);
    if (!playerCls) {
        // mainPlayer_C BP not yet loaded (still in OMEGA / menu) --
        // retry on the next NetPumpTick. Match grab_observer's
        // wait-and-retry shape.
        return;
    }

    // Register POST observers on BOTH candidate UFunctions. The BP graph
    // dispatch path is:
    //   InpActEvt_flashlight_...  -> "Flashlight Update"  -> updateFlashlight
    // Hands-on 2026-05-25 NIGHT-3 showed that only the OUTER function is
    // actually ProcessEvent-dispatched; the inner one is BP-inlined into
    // it and never fires the observer. Registering both is cheap (2 of
    // the 64 observer slots) and survives a future BP recook that might
    // un-inline one or the other.
    // Try all four candidate UFunctions. The dispatch chain per the RE
    // doc is:
    //   InpActEvt_flashlight_K2Node_InputActionEvent_13/14
    //     -> Flashlight Update()
    //        -> updateFlashlight()
    // Hands-on showed both inner functions are BP-inlined into the input
    // events. We hook the input events too -- they are ProcessEvent-
    // dispatched by the engine input system (same as grab_observer's
    // InpActEvt_use, which we already know fires). last-sent-state
    // dedups duplicates if more than one fires for a single F-press.
    struct Candidate { const wchar_t* name; void** outPtr; };
    Candidate cs[] = {
        { P::name::MainPlayerUpdateFlashlightFn,  &g_updateFlashlightFn  },
        { P::name::MainPlayerFlashlightUpdateFn,  &g_flashlightUpdateFn  },
        { P::name::MainPlayerFlashlightInput13Fn, &g_flashlightInput13Fn },
        { P::name::MainPlayerFlashlightInput14Fn, &g_flashlightInput14Fn },
    };
    int registered = 0;
    for (auto& c : cs) {
        void* fn = R::FindFunction(playerCls, c.name);
        if (!fn) {
            UE_LOGW("flashlight: UFunction '%ls' not found on %ls", c.name,
                    P::name::MainPlayerClass);
            continue;
        }
        if (!GT::RegisterPostObserver(fn, &OnUpdateFlashlightPost)) {
            UE_LOGW("flashlight: RegisterPostObserver(%ls) failed", c.name);
            continue;
        }
        *c.outPtr = fn;
        ++registered;
    }
    if (registered == 0) {
        UE_LOGW("flashlight: no candidate observers registered -- aborting install");
        return;
    }

    g_flashlightClassHash = HashClassName(L"prop_equipment_flashlight_C");
    g_installed = true;
    UE_LOGI("flashlight: %d POST observer(s) installed (updateFlashlight=%p, "
            "'Flashlight Update'=%p, InpActEvt_13=%p, InpActEvt_14=%p, "
            "classHash=0x%08X, probe_log=%d)",
            registered, g_updateFlashlightFn, g_flashlightUpdateFn,
            g_flashlightInput13Fn, g_flashlightInput14Fn,
            g_flashlightClassHash, ProbeLogEnabled() ? 1 : 0);
}

void ApplyToPuppet(void* puppetActor, uint32_t classHash, uint8_t state) {
    if (!puppetActor || !R::IsLive(puppetActor)) {
        UE_LOGW("flashlight: ApplyToPuppet called with invalid puppet=%p", puppetActor);
        return;
    }
    if (classHash != g_flashlightClassHash) {
        // Unknown item class -- could be a future radio/torch entry we
        // don't handle yet. Surface it as a warning rather than silently
        // dropping.
        UE_LOGW("flashlight: ApplyToPuppet classHash mismatch (got 0x%08X, "
                "expected 0x%08X = flashlight) -- dropping", classHash,
                g_flashlightClassHash);
        return;
    }

    // Write the canonical state bool first so any code that reads
    // flashlight @0x0838 sees the same value before light_R is touched.
    bool* flashlightBool = reinterpret_cast<bool*>(
        reinterpret_cast<uint8_t*>(puppetActor) + P::off::AmainPlayer_flashlight);
    const bool newState = (state != 0);
    *flashlightBool = newState;

    // Toggle the puppet's spot light. Echo-suppress around the call --
    // SetComponentVisible internally uses reflection UFunctions
    // (SetVisibility / SetHiddenInGame) which DO dispatch through
    // ProcessEvent. None of those is updateFlashlight so the observer
    // wouldn't fire anyway, but the suppress flag is cheap insurance
    // for future code that calls the higher-level path.
    void* light_R = *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(puppetActor) + P::off::AmainPlayer_light_R);
    if (!light_R || !R::IsLive(light_R)) {
        UE_LOGW("flashlight: puppet light_R missing or dead -- bool written, "
                "light not toggled (puppet=%p light=%p)", puppetActor, light_R);
        return;
    }
    g_echoSuppress.store(true, std::memory_order_release);

    // 2026-05-26 deep-RE TERMINAL FIX (per 3-agent convergence, see
    // research/findings/votv-flashlight-cone-deep-RE-2026-05-26.md):
    //
    // Why we don't drive the puppet's class-default light_R: its
    // SceneProxy@+0x3F8 is permanently null because the engine's
    // proxy-creation gate (sub_142A7B590 visibility cascade) fails
    // for the orphan puppet's component chain, and there's no
    // reflection-callable path to force registration (BP's
    // `Flashlight Update` and `InpActEvt_13` are inlined-only stubs;
    // multicast delegate broadcast helper isn't a UFunction;
    // RegisterComponent is native-only in UE4.27).
    //
    // The ONLY reflection-callable mechanism that internally runs
    // UActorComponent::RegisterComponent is the AActor BlueprintCallable
    // pair `AddComponentByClass(deferred=true)` + `FinishAddComponent`.
    // So we spawn a FRESH USpotLightComponent on the puppet, attach
    // it to the actor's root with an upward offset (so the light
    // emanates from head height), and drive its Intensity from the
    // wire. The original puppet.light_R stays untouched and inert.
    //
    // RULE 1 root-cause: AddComponent is THE engine-blessed path for
    // adding a render-state-registered component at runtime. RULE 3:
    // pure reflection, no UE4SS at runtime. RULE 2: original
    // light_R reflection-write logic is removed -- proven dead.
    auto getOrCreateExtraLight = [puppetActor]() -> void* {
        // Per-puppet cache so we only spawn one extra SpotLight per
        // orphan. Re-resolved if the actor is destroyed + respawned
        // (different actor pointer -> different cache entry).
        static std::unordered_map<void*, void*> sExtraLights;
        auto it = sExtraLights.find(puppetActor);
        if (it != sExtraLights.end() && R::IsLive(it->second)) {
            return it->second;
        }
        if (it != sExtraLights.end()) sExtraLights.erase(it);  // stale

        // Resolve UClasses + UFunctions once. AddComponentByClass +
        // FinishAddComponent live on AActor (BlueprintCallable).
        static void* sActorCls = nullptr;
        static void* sSpotCls = nullptr;
        static void* sAddFn = nullptr;
        static void* sFinishFn = nullptr;
        if (!sActorCls) sActorCls = R::FindClass(P::name::ActorClassName);
        if (!sSpotCls) sSpotCls = R::FindClass(P::name::SpotLightComponentClass);
        if (sActorCls && !sAddFn) sAddFn = R::FindFunction(sActorCls, P::name::AddComponentByClassFn);
        if (sActorCls && !sFinishFn) sFinishFn = R::FindFunction(sActorCls, P::name::FinishAddComponentFn);
        if (!sSpotCls || !sAddFn || !sFinishFn) {
            UE_LOGE("flashlight: extra-light resolve failed (spotCls=%p addFn=%p finishFn=%p)",
                    sSpotCls, sAddFn, sFinishFn);
            return nullptr;
        }

        // FTransform identity with Z+85cm offset (head height above
        // the actor pivot at feet). FTransform layout in UE4.27 is
        // {FQuat Rotation@0, FVector Translation@0x10, FVector
        // Scale3D@0x20}. Total 0x30 bytes, 16-byte aligned. Quat
        // identity = (0,0,0,1) -- W last. Scale = (1,1,1).
        alignas(16) uint8_t xform[0x30] = {};
        // Quat (0,0,0,1)
        *reinterpret_cast<float*>(xform + 0x00) = 0.f;
        *reinterpret_cast<float*>(xform + 0x04) = 0.f;
        *reinterpret_cast<float*>(xform + 0x08) = 0.f;
        *reinterpret_cast<float*>(xform + 0x0C) = 1.f;
        // Translation (0, 0, 85)  -- head-height offset above capsule centre
        *reinterpret_cast<float*>(xform + 0x10) = 0.f;
        *reinterpret_cast<float*>(xform + 0x14) = 0.f;
        *reinterpret_cast<float*>(xform + 0x18) = 85.f;
        // Scale (1,1,1)
        *reinterpret_cast<float*>(xform + 0x20) = 1.f;
        *reinterpret_cast<float*>(xform + 0x24) = 1.f;
        *reinterpret_cast<float*>(xform + 0x28) = 1.f;

        void* newLight = nullptr;
        {
            ue_wrap::ParamFrame f(sAddFn);
            f.Set<void*>(L"Class", sSpotCls);
            f.Set<bool>(L"bManualAttachment", false);  // auto-attach to actor root
            f.SetRaw(L"relativeTransform", xform, sizeof(xform));
            f.Set<bool>(L"bDeferredFinish", true);  // we'll set properties before Finish
            if (!ue_wrap::Call(puppetActor, f)) {
                UE_LOGE("flashlight: AddComponentByClass(SpotLight) Call failed");
                return nullptr;
            }
            newLight = f.Get<void*>(L"ReturnValue");
        }
        if (!newLight) {
            UE_LOGE("flashlight: AddComponentByClass returned null SpotLight");
            return nullptr;
        }

        // Set initial properties BEFORE FinishAddComponent (which
        // registers). Critical: IntensityUnits = 0 (Unitless) before
        // registration, otherwise the engine default lumens scale
        // will treat our intensity numbers as enormous values.
        // Intensity = 0 so the light is OFF until we toggle it. Cone
        // angles roughly mirror VOTV's flashlight (40deg outer is a
        // reasonable flashlight cone). bAffectsWorld bit 0 = 1.
        auto nU8 = reinterpret_cast<uint8_t*>(newLight);
        *reinterpret_cast<float*>  (nU8 + P::off::ULightComponentBase_Intensity)   = 0.f;
        *reinterpret_cast<uint8_t*>(nU8 + P::off::ULocalLightComponent_IntensityUnits) = 0;  // Unitless
        *reinterpret_cast<uint8_t*>(nU8 + P::off::ULightComponentBase_FlagsByte)   |= 0x01;  // bAffectsWorld
        // SpotLight cone angles (USpotLightComponent + 0x358/0x35C):
        *reinterpret_cast<float*>  (nU8 + 0x0358) = 12.f;  // InnerConeAngle  (deg)
        *reinterpret_cast<float*>  (nU8 + 0x035C) = 40.f;  // OuterConeAngle  (deg)
        // Attenuation radius (UPointLightComponent::AttenuationRadius @+0x0330)
        *reinterpret_cast<float*>  (nU8 + 0x0330) = 2000.f;

        // Direct bit write: bVisible (bit 4 of byte 0x14C). Some UE4
        // proxy-creation paths gate on bVisible -- set it true BEFORE
        // registration so CreateRenderState_Concurrent doesn't early-
        // out on visibility cascade. propagate-to-children is moot
        // for a freshly-spawned light with no children.
        *reinterpret_cast<uint8_t*>(nU8 + P::off::USceneComponent_VisFlagsByte) |= 0x10;

        // FinishAddComponent -> registers the component -> internally
        // calls UActorComponent::RegisterComponent -> CreateRenderState_
        // Concurrent -> CreateSceneProxy. This is the moment the
        // FSceneProxy is allocated and AddedToScene.
        {
            ue_wrap::ParamFrame f(sFinishFn);
            f.Set<void*>(L"Component", newLight);
            f.Set<bool>(L"bManualAttachment", false);
            f.SetRaw(L"relativeTransform", xform, sizeof(xform));
            ue_wrap::Call(puppetActor, f);
        }
        // Belt-and-braces: AFTER registration, ALSO call SetVisibility
        // via reflection. UE4.27's SetVisibility calls MarkRenderState
        // Dirty which schedules a RecreateRenderState_Concurrent next
        // frame if the proxy wasn't created during the synchronous
        // RegisterComponent path.
        if (void* lightCls = R::FindClass(L"LightComponent")) {
            if (void* setVisFn = R::FindFunction(R::FindClass(L"SceneComponent"), P::name::SetVisibilityFn)) {
                ue_wrap::ParamFrame fv(setVisFn);
                fv.Set<bool>(L"bNewVisibility", true);
                fv.Set<bool>(L"bPropagateToChildren", false);
                ue_wrap::Call(newLight, fv);
            }
            (void)lightCls;
        }
        sExtraLights[puppetActor] = newLight;
        UE_LOGI("flashlight: SPAWNED extra SpotLight on puppet=%p -> light=%p "
                "(via AddComponentByClass+Finish; registers + creates SceneProxy)",
                puppetActor, newLight);
        return newLight;
    };

    void* extraLight = getOrCreateExtraLight();

    // AUTOTEST-ONLY bright-puppet override: in normal play we keep
    // the extra light's intensity at a moderate "natural" value.
    // For the autonomous-test screenshots in daylight, drive it
    // brighter so surface illumination is visible against ambient.
    static const bool s_testBright = []() {
        wchar_t buf[8] = {};
        const DWORD n = ::GetEnvironmentVariableW(L"VOTVCOOP_RUN_FLASHLIGHT_TEST", buf, 8);
        return n > 0 && buf[0] == L'1';
    }();
    bool intOk = false;
    if (extraLight) {
        // Resolve SetIntensity on the parent ULightComponent class once.
        static void* sSetIntensityFn = nullptr;
        if (!sSetIntensityFn) {
            void* lightCls = R::FindClass(L"LightComponent");
            if (lightCls) sSetIntensityFn = R::FindFunction(lightCls, P::name::SetIntensityFn);
        }
        if (sSetIntensityFn) {
            // 100.0 Unitless is OBVIOUSLY bright -- if the light is
            // actually rendering, daylight ambient can't hide it. If
            // the test screenshots still show no change with this
            // intensity, the light isn't rendering at all (proxy never
            // created OR component isn't being added to the scene).
            const float target = newState ? (s_testBright ? 100.f : kIntensityOnFallback)
                                          : kIntensityOffDefault;
            ue_wrap::ParamFrame f(sSetIntensityFn);
            f.Set<float>(L"NewIntensity", target);
            intOk = ue_wrap::Call(extraLight, f);
        }
    }

    g_echoSuppress.store(false, std::memory_order_release);
    UE_LOGI("flashlight: applied to puppet=%p state=%d (extraLight=%p intOk=%d, testBright=%d)",
            puppetActor, newState ? 1 : 0, extraLight, intOk ? 1 : 0,
            s_testBright ? 1 : 0);

    // Diagnostic readback: confirm the puppet's light_R actually has the
    // fields we expect AFTER our writes. The 2026-05-26 deep-RE plan
    // identified ULightComponent::SceneProxy @+0x03F8 as the SMOKING-GUN
    // field: SetIntensity hard no-ops when SceneProxy is null. We dump
    // SceneProxy, bRegistered (UActorComponent @+0x88 bit 0), Mobility
    // (USceneComponent @+0x14F), bAffectsWorld (ULightComponentBase
    // @+0x214 bit 0), bVisible (USceneComponent @+0x14C bit 4), and
    // Intensity (ULightComponentBase @+0x20C) -- all on puppet AND on
    // local for differential diagnosis. If puppet.SceneProxy is null
    // and local.SceneProxy is non-null, we've localized the failure
    // to the orphan's component-registration path.
    auto dumpLight = [](const wchar_t* tag, void* light) {
        if (!light) { UE_LOGI("flashlight[diag %ls]: <null>", tag); return; }
        const float    intensity   = *reinterpret_cast<float*>   (reinterpret_cast<uint8_t*>(light) + P::off::ULightComponentBase_Intensity);
        const uint8_t  regByte     = *reinterpret_cast<uint8_t*> (reinterpret_cast<uint8_t*>(light) + P::off::UActorComponent_RegFlagsByte);
        const uint8_t  visByte     = *reinterpret_cast<uint8_t*> (reinterpret_cast<uint8_t*>(light) + P::off::USceneComponent_VisFlagsByte);
        const uint8_t  hidByte     = *reinterpret_cast<uint8_t*> (reinterpret_cast<uint8_t*>(light) + P::off::USceneComponent_HiddenFlagsByte);
        const uint8_t  mobility    = *reinterpret_cast<uint8_t*> (reinterpret_cast<uint8_t*>(light) + P::off::USceneComponent_Mobility);
        const uint8_t  worldByte   = *reinterpret_cast<uint8_t*> (reinterpret_cast<uint8_t*>(light) + P::off::ULightComponentBase_FlagsByte);
        void*          sceneProxy  = *reinterpret_cast<void**>   (reinterpret_cast<uint8_t*>(light) + P::off::ULightComponent_SceneProxy);
        void*          attachParent= *reinterpret_cast<void**>   (reinterpret_cast<uint8_t*>(light) + P::off::USceneComponent_AttachParent);
        UE_LOGI("flashlight[diag %ls]: light=%p SceneProxy=%p Intensity=%.2f "
                "bRegistered=%d bVisible=%d bHiddenInGame=%d Mobility=%u "
                "bAffectsWorld=%d (regByte=0x%02X visByte=0x%02X hidByte=0x%02X worldByte=0x%02X) "
                "AttachParent=%p",
                tag, light, sceneProxy, intensity,
                (regByte & 0x01) ? 1 : 0,
                (visByte & 0x10) ? 1 : 0,
                (hidByte & 0x04) ? 1 : 0,
                static_cast<unsigned>(mobility),
                (worldByte & 0x01) ? 1 : 0,
                regByte, visByte, hidByte, worldByte,
                attachParent);
    };
    dumpLight(L"puppet ", light_R);
    dumpLight(L"extra  ", extraLight);  // the AddComponentByClass-spawned one (SHOULD have SceneProxy non-null)
    // Local player's light_R for differential comparison. Use the
    // Registry (not FindObjectByClass) since FindObjectByClass may
    // return the puppet on its first hit.
    void* localPlayer = coop::players::Registry::Get().Local();
    if (localPlayer && localPlayer != puppetActor) {
        void* localLightR = *reinterpret_cast<void**>(
            reinterpret_cast<uint8_t*>(localPlayer) + P::off::AmainPlayer_light_R);
        if (localLightR && R::IsLive(localLightR)) {
            dumpLight(L"local ", localLightR);
        }
    }
}

}  // namespace coop::item_activate
