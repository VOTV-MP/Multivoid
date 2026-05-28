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

#include "coop/flashlight_click_sound.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/players_registry.h"
#include "coop/remote_player.h"
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
//
// 2026-05-26 v6: timerHoldFlashlightFn added for the hold-F mode-change
// trigger (default-spread <-> focused). The POST observer reads the
// post-mutation flashlightMode + light_R cone shape and sends a v6
// ItemActivate packet so the receiver mirrors the cone shape.
void* g_updateFlashlightFn = nullptr;
void* g_flashlightUpdateFn = nullptr;
void* g_flashlightInput13Fn = nullptr;
void* g_flashlightInput14Fn = nullptr;
void* g_timerHoldFlashlightFn = nullptr;

// v6 dedup: track last-sent packet signature (state + mode + intensity +
// cone angles, encoded as a u64 hash). The hold-F mode change keeps state=on
// but mutates mode + cone angles, so a state-only dedup would drop the
// mode-change packet. Sentinel `kNoSendYet` = no packet ever sent. Single
// atomic so the load is uninterruptible (audit-fix 2026-05-26: prior
// split-atomic [g_haveLastSent + g_lastSentSig] had a race window where
// DebugForceToggle on the worker thread could update the bool BEFORE the
// signature, letting an observer on the GT read haveLastSent=true with the
// OLD signature and erroneously dedup a fresh mode-change packet).
inline constexpr uint64_t kNoSendYet = 0xFFFFFFFFFFFFFFFFULL;
std::atomic<uint64_t> g_lastSentSig{kNoSendYet};

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

// Phase 5F Inc5 (connect-time replay) -- pending receiver-side applies.
// Per-peer slot keyed by peerSessionId. Stashes the latest ItemActivate
// payload that arrived BEFORE the corresponding puppet was spawned (the
// puppet is only created on first PoseSnapshot for that peer; a tightly-
// ordered connect-edge can race the reliable ItemActivate ahead of the
// unreliable PoseSnapshot). Drained per-tick by TickConnect() once the
// puppet becomes valid in the players::Registry. Latest-wins: a newer
// ApplyToPuppetOrDefer for the same peer overrides the prior pending.
// All access on the game thread (event_feed dispatches via GT::Post,
// TickConnect is called from the game-thread net pump tick).
bool g_pendingApplyValid[coop::players::kMaxPeers] = {};
coop::net::ItemActivatePayload g_pendingApplyPayload[coop::players::kMaxPeers] = {};

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

// v6 helper: snapshot the local mp's flashlight state + cone shape into a
// ItemActivatePayload. The reads happen on the calling thread (POST observer
// runs on the game thread; DebugForceToggle calls this inside its GT::Post
// lambda). All reads are direct memory, no UFunction dispatch. Returns
// false if light_R is dead (caller skips the send).
bool BuildPayloadFromLocal(void* mp, coop::net::ItemActivatePayload& out, coop::net::Session* session) {
    if (!mp || !session) return false;
    void* light_R = *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(mp) + P::off::AmainPlayer_light_R);
    if (!light_R || !R::IsLive(light_R)) return false;

    const bool flashlight = *reinterpret_cast<bool*>(
        reinterpret_cast<uint8_t*>(mp) + P::off::AmainPlayer_flashlight);
    const uint8_t mode = *reinterpret_cast<uint8_t*>(
        reinterpret_cast<uint8_t*>(mp) + P::off::AmainPlayer_flashlightMode);
    const float intensity = *reinterpret_cast<float*>(
        reinterpret_cast<uint8_t*>(light_R) + P::off::ULightComponentBase_Intensity);
    const float outerCone = *reinterpret_cast<float*>(
        reinterpret_cast<uint8_t*>(light_R) + P::off::USpotLightComponent_OuterConeAngle);
    const float innerCone = *reinterpret_cast<float*>(
        reinterpret_cast<uint8_t*>(light_R) + P::off::USpotLightComponent_InnerConeAngle);

    out = {};
    out.itemClassHash   = g_flashlightClassHash;
    // PR-4.2 (closes audit finding #9): peer session id comes from the
    // central Registry, which the wire layer populates via AssignPeerSlot
    // on the client and harness on the host. Pre-fix the value was
    // hardcoded `(role==Host) ? 0 : 1` -- broken in 3-peer because both
    // clients stamped 1 and silently self-echo-dropped each other.
    out.peerSessionId   = coop::players::Registry::Get().LocalPeerId();
    out.state           = flashlight ? 1 : 0;
    out.flags           = 0;  // Case (b) -- no actor key
    out.mode            = mode;
    out.actorKeyHash    = 0;
    out.intensity       = intensity;
    out.outerConeAngle  = outerCone;
    out.innerConeAngle  = innerCone;
    return true;
}

// FNV1a-ish 64-bit hash of the payload's mutable fields (state, mode,
// intensity, cones). Used to dedup the observer fires that all happen
// for the same logical change (press fires InpActEvt_13 + Flashlight Update
// + updateFlashlight in a chain; hold-F fires timerHoldFlashlight + maybe
// some of the others).
uint64_t SignaturePayload(const coop::net::ItemActivatePayload& p) {
    uint64_t h = 0xcbf29ce484222325ULL;
    auto mix = [&](uint64_t v) { h ^= v; h *= 0x100000001b3ULL; };
    mix(p.state);
    mix(p.mode);
    mix(reinterpret_cast<const uint32_t&>(p.intensity));
    mix(reinterpret_cast<const uint32_t&>(p.outerConeAngle));
    mix(reinterpret_cast<const uint32_t&>(p.innerConeAngle));
    return h;
}

void OnUpdateFlashlightPost(void* self, void* function, void* /*params*/) {
    if (!self) return;
    // Identify which hook fired -- diagnostic in probe mode.
    const char* which =
        (function == g_flashlightUpdateFn)     ? "Flashlight_Update" :
        (function == g_updateFlashlightFn)     ? "updateFlashlight"  :
        (function == g_flashlightInput13Fn)    ? "InpActEvt_13"      :
        (function == g_flashlightInput14Fn)    ? "InpActEvt_14"      :
        (function == g_timerHoldFlashlightFn)  ? "timerHoldFlashlight" :
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

    coop::net::ItemActivatePayload p{};
    if (!BuildPayloadFromLocal(self, p, s)) return;

    // v6 dedup: signature includes state + mode + intensity + cone angles.
    // Press fires InpActEvt_13 + Flashlight Update + updateFlashlight in a
    // chain (all 3 see the same final state); hold-F fires
    // timerHoldFlashlight + possibly some of the others. We send ONLY the
    // first observer of a logical change. SINGLE atomic with sentinel so
    // the load can never see a torn "have-flag + stale-sig" state (audit
    // 2026-05-26 fix).
    const uint64_t sig = SignaturePayload(p);
    // FNV hash CAN by extraordinary coincidence equal kNoSendYet (1 in 2^64);
    // avoid the sentinel value to keep "no send yet" disambiguated.
    const uint64_t storeSig = (sig == kNoSendYet) ? (kNoSendYet - 1) : sig;
    if (g_lastSentSig.load(std::memory_order_acquire) == storeSig) {
        if (ProbeLogEnabled()) {
            UE_LOGI("flashlight: payload signature unchanged (sig=%llX) -- no wire send",
                    static_cast<unsigned long long>(storeSig));
        }
        return;
    }

    const bool sent = s->SendReliable(coop::net::ReliableKind::ItemActivate, &p, sizeof(p));
    if (!sent) {
        UE_LOGW("flashlight: SendReliable failed (channel busy or not connected) [%s]", which);
    } else {
        g_lastSentSig.store(storeSig, std::memory_order_release);
        UE_LOGI("flashlight: sent state=%d mode=%u Intensity=%.2f outerCone=%.1f innerCone=%.1f "
                "(peer=%u, via %s)",
                p.state, p.mode, p.intensity, p.outerConeAngle, p.innerConeAngle,
                p.peerSessionId, which);
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

    // v6: snapshot the local cone shape into the packet. DebugForceToggle
    // bypasses the BP (directly writes flashlight bool + SetIntensity), so
    // outer/inner cone won't reflect a real mode change -- they'll carry
    // whatever the BP-default values are on the local light_R. That's fine
    // for the autotest (focused-vs-spread is a hands-on feature; autotest
    // doesn't exercise hold-F).
    coop::net::ItemActivatePayload p{};
    if (!BuildPayloadFromLocal(mp, p, s)) {
        UE_LOGW("flashlight: DebugForceToggle BuildPayloadFromLocal failed (light_R dead?)");
        return newState;
    }

    bool sent = false;
    int attempts = 0;
    for (; attempts < 200; ++attempts) {
        sent = s->SendReliable(coop::net::ReliableKind::ItemActivate, &p, sizeof(p));
        if (sent) break;
        ::Sleep(25);
    }
    if (sent) {
        const uint64_t sig = SignaturePayload(p);
        const uint64_t storeSig = (sig == kNoSendYet) ? (kNoSendYet - 1) : sig;
        g_lastSentSig.store(storeSig, std::memory_order_release);
        UE_LOGI("flashlight: DebugForceToggle sent state=%d mode=%u Intensity=%.2f outerCone=%.1f "
                "(peer=%u, after %d retr%s)",
                p.state, p.mode, p.intensity, p.outerConeAngle, p.peerSessionId, attempts,
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
        { P::name::MainPlayerUpdateFlashlightFn,    &g_updateFlashlightFn    },
        { P::name::MainPlayerFlashlightUpdateFn,    &g_flashlightUpdateFn    },
        { P::name::MainPlayerFlashlightInput13Fn,   &g_flashlightInput13Fn   },
        { P::name::MainPlayerFlashlightInput14Fn,   &g_flashlightInput14Fn   },
        { P::name::MainPlayerTimerHoldFlashlightFn, &g_timerHoldFlashlightFn },
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
            "timerHoldFlashlight=%p, classHash=0x%08X, probe_log=%d)",
            registered, g_updateFlashlightFn, g_flashlightUpdateFn,
            g_flashlightInput13Fn, g_flashlightInput14Fn,
            g_timerHoldFlashlightFn, g_flashlightClassHash,
            ProbeLogEnabled() ? 1 : 0);
}

void ApplyToPuppet(void* puppetActor, const coop::net::ItemActivatePayload& payload) {
    if (!puppetActor || !R::IsLive(puppetActor)) {
        UE_LOGW("flashlight: ApplyToPuppet called with invalid puppet=%p", puppetActor);
        return;
    }
    if (payload.itemClassHash != g_flashlightClassHash) {
        UE_LOGW("flashlight: ApplyToPuppet classHash mismatch (got 0x%08X, "
                "expected 0x%08X = flashlight) -- dropping", payload.itemClassHash,
                g_flashlightClassHash);
        return;
    }

    const bool newState = (payload.state != 0);

    void* light_R = *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(puppetActor) + P::off::AmainPlayer_light_R);
    if (!light_R || !R::IsLive(light_R)) {
        UE_LOGW("flashlight: puppet light_R missing or dead -- light not toggled "
                "(puppet=%p light=%p)", puppetActor, light_R);
        return;
    }

    // 2026-05-26 Option α (deep-RE pass-5 convergence after 3 agents):
    // Drive the EXISTING puppet light_R via SetVisibility + SetIntensity
    // UFunctions ONLY. Do NOT write the puppet.flashlight bool (that
    // triggers the BP `updateFlashlight` ubergraph which is A1's
    // highest-confidence "Invalid prop" emitter). Do NOT
    // AddComponentByClass (component-attached BP listeners on
    // mainPlayer_C). Do NOT broadcast flashlightStateChanged
    // (verified dead delegate by A2 -- zero subscribers).
    //
    // Why SetVisibility is the key: it's a USceneComponent UFunction
    // (ProcessEvent-dispatched native). Its body sets bVisible bit +
    // calls the OnVisibilityChanged virtual. For ULightComponent the
    // virtual calls MarkRenderStateDirty (pure native, NOT a UFunction
    // -- so it CANNOT fire BP observers). MarkRenderStateDirty
    // schedules RecreateRenderState_Concurrent at end-of-frame which
    // runs CreateSceneProxy. The puppet light_R's bRegistered=1 +
    // visByte bit-5 already pass the cascade per diagnostic readout
    // (regByte=0x2B visByte=0x63 bAffectsWorld=1 Mobility=Movable).
    // Once the proxy is created, our existing SetIntensity wire path
    // drives brightness.
    //
    // Direct bit writes to bVisible do NOT trigger MarkRenderStateDirty
    // (just a memory store, no virtual dispatch). That's why the
    // earlier 0d5bce5/722031e attempts which used direct bit writes
    // produced no visible change.
    g_echoSuppress.store(true, std::memory_order_release);

    static void* sSetVisibilityFn      = nullptr;
    static void* sSetIntensityFn       = nullptr;
    static void* sSetOuterConeAngleFn  = nullptr;
    static void* sSetInnerConeAngleFn  = nullptr;
    if (!sSetVisibilityFn) {
        if (void* sceneCls = R::FindClass(P::name::SceneComponentClass)) {
            sSetVisibilityFn = R::FindFunction(sceneCls, P::name::SetVisibilityFn);
        }
    }
    if (!sSetIntensityFn) {
        if (void* lightCls = R::FindClass(L"LightComponent")) {
            sSetIntensityFn = R::FindFunction(lightCls, P::name::SetIntensityFn);
        }
    }
    if (!sSetOuterConeAngleFn) {
        if (void* spotCls = R::FindClass(P::name::SpotLightComponentClass)) {
            sSetOuterConeAngleFn = R::FindFunction(spotCls, P::name::SetOuterConeAngleFn);
            sSetInnerConeAngleFn = R::FindFunction(spotCls, P::name::SetInnerConeAngleFn);
        }
    }

    // 1) SetVisibility(newState, false) -- triggers MarkRenderStateDirty
    //    on the FIRST transition (false->true or true->false). Required
    //    for proxy creation on the orphan puppet (CreateRenderState_
    //    Concurrent runs on dirty mark; light_R's pre-existing
    //    bRegistered=1 + visByte bit-5 pass the cascade).
    if (sSetVisibilityFn) {
        ue_wrap::ParamFrame f(sSetVisibilityFn);
        f.Set<bool>(L"bNewVisibility", newState);
        f.Set<bool>(L"bPropagateToChildren", false);
        ue_wrap::Call(light_R, f);
    } else {
        UE_LOGW("flashlight: SceneComponent::SetVisibility UFunction not resolved");
    }

    // 2) SetIntensity(packet.intensity) -- mirror sender's exact brightness.
    //    For OFF state, sender's intensity is the BP's "off" sentinel
    //    (~0.2 Unitless) which renders as ~no light. No fallback latch
    //    needed -- the sender sends the truth.
    const float targetIntensity = newState ? payload.intensity : 0.f;
    if (sSetIntensityFn) {
        ue_wrap::ParamFrame f(sSetIntensityFn);
        f.Set<float>(L"NewIntensity", targetIntensity);
        ue_wrap::Call(light_R, f);
    } else {
        UE_LOGW("flashlight: LightComponent::SetIntensity UFunction not resolved");
    }

    // 3) SetOuterConeAngle + SetInnerConeAngle -- mirror sender's cone
    //    shape (default-spread vs focused mode from hold-F). Both
    //    UFunctions internally MarkRenderStateDirty so the proxy
    //    refreshes with the new angles next frame. Only apply when ON
    //    (cone shape on an off light is invisible; saves two UFunction
    //    dispatches per off toggle).
    if (newState) {
        if (sSetOuterConeAngleFn && payload.outerConeAngle > 0.f) {
            ue_wrap::ParamFrame f(sSetOuterConeAngleFn);
            f.Set<float>(L"NewOuterConeAngle", payload.outerConeAngle);
            ue_wrap::Call(light_R, f);
        }
        if (sSetInnerConeAngleFn && payload.innerConeAngle >= 0.f) {
            ue_wrap::ParamFrame f(sSetInnerConeAngleFn);
            f.Set<float>(L"NewInnerConeAngle", payload.innerConeAngle);
            ue_wrap::Call(light_R, f);
        }
    }

    g_echoSuppress.store(false, std::memory_order_release);
    UE_LOGI("flashlight: applied to puppet=%p state=%d Intensity=%.2f outerCone=%.1f innerCone=%.1f mode=%u",
            puppetActor, newState ? 1 : 0, targetIntensity,
            payload.outerConeAngle, payload.innerConeAngle, payload.mode);

    // 3D positional click sound at the puppet -- extracted to its own
    // subsystem (see coop/flashlight_click_sound.h). Gated on state-CHANGE
    // (hold-F mode-change packets don't click). Runtime-constructs its
    // own USoundAttenuation, AddToRoot's it for GC stability.
    coop::flashlight_click_sound::PlayIfStateChanged(
        puppetActor, payload.peerSessionId, newState);
}

void ApplyToPuppetOrDefer(uint8_t peerSessionId, void* puppetActor,
                          const coop::net::ItemActivatePayload& p) {
    if (peerSessionId >= coop::players::kMaxPeers) {
        UE_LOGW("flashlight: ApplyToPuppetOrDefer peerSessionId=%u out of range -- dropping",
                static_cast<unsigned>(peerSessionId));
        return;
    }
    if (puppetActor && R::IsLive(puppetActor)) {
        // Puppet is ready -- clear any stale pending entry (defensive:
        // a fresh apply supersedes a still-pending one) + apply now.
        g_pendingApplyValid[peerSessionId] = false;
        ApplyToPuppet(puppetActor, p);
        return;
    }
    // Puppet not ready yet -- stash latest-wins. The TickConnect pump
    // will pick it up once the registry has a puppet for this peer.
    g_pendingApplyPayload[peerSessionId] = p;
    g_pendingApplyValid[peerSessionId] = true;
    UE_LOGI("flashlight: ApplyToPuppetOrDefer puppet not ready for peer=%u -- "
            "stashing (state=%d Intensity=%.2f)",
            static_cast<unsigned>(peerSessionId), p.state, p.intensity);
}

void QueueConnectBroadcast() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) {
        UE_LOGW("flashlight: QueueConnectBroadcast called with no session");
        return;
    }
    void* mp = coop::players::Registry::Get().Local();
    if (!mp) {
        // Local mp not yet alive (still in OMEGA / menu). Without a local
        // we have no state to broadcast. Edge will fire again on the next
        // reconnect; this connect-edge case (connected before mp loads)
        // is rare in practice (mp spawns in the loading splash before
        // session.Connected). Drop silently.
        UE_LOGI("flashlight: QueueConnectBroadcast no local mp yet -- no broadcast");
        return;
    }
    coop::net::ItemActivatePayload p{};
    if (!BuildPayloadFromLocal(mp, p, s)) {
        UE_LOGW("flashlight: QueueConnectBroadcast BuildPayloadFromLocal failed");
        return;
    }
    // Skip broadcast if LOCAL flashlight is OFF -- the receiver's puppet
    // defaults to OFF on spawn, so an OFF broadcast is a redundant packet.
    // (When the local user toggles to ON later, the normal POST observer
    // path ships it.)
    if (p.state == 0) {
        UE_LOGI("flashlight: QueueConnectBroadcast local state is OFF -- "
                "skipping (puppet default is OFF; no replay needed)");
        return;
    }
    // 2026-05-27: reliable channel buffers internally; Send always accepted.
    s->SendReliable(coop::net::ReliableKind::ItemActivate, &p, sizeof(p));
    const uint64_t sig = SignaturePayload(p);
    const uint64_t storeSig = (sig == kNoSendYet) ? (kNoSendYet - 1) : sig;
    g_lastSentSig.store(storeSig, std::memory_order_release);
    UE_LOGI("flashlight: connect-broadcast sent state=%d Intensity=%.2f "
            "outerCone=%.1f mode=%u peer=%u",
            p.state, p.intensity, p.outerConeAngle, p.mode, p.peerSessionId);
}

void TickConnect() {
    // 2026-05-27: host-side pending-broadcast retry retired (channel queues
    // internally). Kept the receiver-side pending-apply drain -- that's a
    // state-defer (waiting for the peer's puppet to spawn), NOT channel-busy
    // retry.

    // Drain pending applies. For each peer slot with a pending payload,
    // look up the puppet via the registry; if it's now valid, apply + clear.
    for (uint8_t peer = 0; peer < coop::players::kMaxPeers; ++peer) {
        if (!g_pendingApplyValid[peer]) continue;
        auto* rp = coop::players::Registry::Get().Puppet(peer);
        if (!rp || !rp->valid()) continue;
        void* puppet = rp->GetActor();
        if (!puppet || !R::IsLive(puppet)) continue;
        coop::net::ItemActivatePayload p = g_pendingApplyPayload[peer];
        g_pendingApplyValid[peer] = false;
        UE_LOGI("flashlight: draining deferred apply for peer=%u (state=%d Intensity=%.2f)",
                static_cast<unsigned>(peer), p.state, p.intensity);
        ApplyToPuppet(puppet, p);
    }
}

void OnDisconnect() {
    int clearedApplies = 0;
    for (uint8_t i = 0; i < coop::players::kMaxPeers; ++i) {
        if (g_pendingApplyValid[i]) ++clearedApplies;
        g_pendingApplyValid[i] = false;
        g_pendingApplyPayload[i] = {};
    }
    if (clearedApplies > 0) {
        UE_LOGI("flashlight: OnDisconnect cleared %d pending apply slot(s)",
                clearedApplies);
    }
}

}  // namespace coop::item_activate
