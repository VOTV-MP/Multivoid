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

#include "coop/player/item_activate.h"

#include "coop/element/element.h"
#include "coop/player/flashlight_click_sound.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/config/config.h"
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
// a setter on another thread -- same pattern as coop::teleport_client).
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

// T2-4 (host-relay late-joiner): persistent per-peer-client cache of the
// LAST item-activate the host applied for that peer (NOT cleared on apply,
// unlike g_pendingApplyPayload). On a new client's connect edge the host
// replays each existing peer's cached ON state to the joiner so the joiner
// converges to the current world (ItemActivate is edge-triggered, so without
// this a flashlight already ON before the joiner arrived stays dark on its
// screen until the owner next toggles). Host-only, game thread only. Slot 0
// (host's own state) is NOT cached here -- QueueConnectBroadcastForSlot
// already replays the host's own flashlight via BuildPayloadFromLocal.
bool g_peerActivateValid[coop::players::kMaxPeers] = {};
coop::net::ItemActivatePayload g_peerActivateCache[coop::players::kMaxPeers] = {};

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
    static const bool s_enabled = ::coop::config::IsIniKeyTrue("flashlight_log");
    return s_enabled;
}

// v6 helper: snapshot the local mp's flashlight state + cone shape into a
// ItemActivatePayload. The reads happen on the calling thread (POST observer
// runs on the game thread; DebugForceToggle calls this inside its GT::Post
// lambda). All reads are direct memory, no UFunction dispatch. Returns
// false if light_R is dead (caller skips the send).
bool BuildPayloadFromLocal(void* mp, coop::net::ItemActivatePayload& out, coop::net::Session* session) {
    if (!mp || !session) return false;
    void* light_R = ue_wrap::engine::GetMainPlayerLightR(mp);
    if (!light_R) return false;

    ue_wrap::engine::MainPlayerFlashlightState fl{};
    if (!ue_wrap::engine::ReadMainPlayerFlashlightState(mp, fl)) return false;
    ue_wrap::engine::FlashlightSnapshot snap{};
    if (!ue_wrap::engine::ReadFlashlightSnapshot(light_R, snap)) return false;

    out = {};
    out.itemClassHash   = g_flashlightClassHash;
    // v13 (A4 2026-05-29): sender stamps its own local Player Element id.
    // Receivers resolve via coop::element::Registry::Get to a Player
    // Element + read PeerSlot() for routing. 0 == "Element not yet
    // allocated" (boot/seed race); receiver falls back to senderPeerSlot.
    // 3-peer-correct -- ids are unique across the host+peer ranges so
    // two clients can't collide.
    {
        const coop::element::ElementId selfEid =
            coop::players::Registry::Get().LocalPlayerElementId();
        out.senderElementId =
            (selfEid == coop::element::kInvalidId) ? 0u : selfEid;
        // (v14 stamped a paired senderContext byte here; v16
        // PR-FOUNDATION-1b moved stale-gen defense to the header
        // senderEpoch and removed the byte from the payload.)
    }
    out.state           = fl.flashlight ? 1 : 0;
    out.flags           = 0;  // Case (b) -- no actor key
    out.mode            = fl.mode;
    out.actorKeyHash    = 0;
    out.intensity       = snap.intensity;
    out.outerConeAngle  = snap.outerConeAngle;
    out.innerConeAngle  = snap.innerConeAngle;
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
    // the new on/off value. A-1 (2026-05-29): the 3 bools live at
    // adjacent fixed offsets on AmainPlayer_C; one wrapper call replaces
    // 3 raw struct-offset reads (Principle 7 -- gameplay never touches
    // engine memory directly).
    ue_wrap::engine::MainPlayerFlashlightState fl{};
    if (!ue_wrap::engine::ReadMainPlayerFlashlightState(self, fl)) return;
    const bool flashlight = fl.flashlight;
    const bool hasFlashlight = fl.hasFlashlight;
    const bool crankFlashlight = fl.crankFlashlight;

    if (ProbeLogEnabled()) {
        // [probe] flashlight_log=1 hands-on verification: see whether
        // the BP early-returns when !hasFlashlight (the bool should
        // not change in that case) and confirm light_R visibility is
        // in lockstep with the bool. The crank-flashlight (_c) path
        // is logged separately because we currently skip the wire
        // send for it (see below).
        void* light_R = ue_wrap::engine::GetMainPlayerLightR(self);
        ue_wrap::engine::FlashlightSnapshot snap{};
        const bool snapOk = ue_wrap::engine::ReadFlashlightSnapshot(light_R, snap);
        UE_LOGI("flashlight[POST %s] self=%p flashlight=%d hasFL=%d crankFL=%d "
                "light_R=%p bVisible=%d Intensity=%.1f", which, self,
                flashlight ? 1 : 0, hasFlashlight ? 1 : 0,
                crankFlashlight ? 1 : 0, light_R,
                snapOk && snap.visible ? 1 : 0,
                snapOk ? snap.intensity : -1.f);
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
        ue_wrap::engine::FlashlightSnapshot snap{};
        if (ue_wrap::engine::ReadFlashlightSnapshot(
                ue_wrap::engine::GetMainPlayerLightR(self), snap)) {
            if (snap.intensity > 1.f && g_latchedOnIntensity.load(std::memory_order_acquire) == 0.f) {
                g_latchedOnIntensity.store(snap.intensity, std::memory_order_release);
                UE_LOGI("flashlight: latched local 'on' intensity = %.2f (will drive puppet)", snap.intensity);
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
                "(senderElementId=0x%08x, via %s)",
                p.state, p.mode, p.intensity, p.outerConeAngle, p.innerConeAngle,
                p.senderElementId, which);
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
        // A-1 (2026-05-29): read current + flip via wrappers (Principle 7).
        ue_wrap::engine::MainPlayerFlashlightState cur{};
        if (!ue_wrap::engine::ReadMainPlayerFlashlightState(mp, cur)) {
            done->store(1, std::memory_order_release);
            return;
        }
        const bool newState = !cur.flashlight;
        ue_wrap::engine::WriteMainPlayerFlashlight(mp, newState);

        // Drive the local light_R's Intensity to match the new state.
        // Mirrors the BP's toggle effect so the SENDER also visually
        // toggles. Same SetIntensity path the puppet receiver uses
        // (MarkRenderStateDirty internally).
        void* light_R = ue_wrap::engine::GetMainPlayerLightR(mp);
        float localTarget = kIntensityOffDefault;
        if (newState) {
            localTarget = g_latchedOnIntensity.load(std::memory_order_acquire);
            if (localTarget == 0.f) localTarget = kIntensityOnFallback;
        }
        if (light_R) {
            ue_wrap::engine::SetLightIntensity(light_R, localTarget);
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

    // SendReliable queues internally via GNS; it returns true unconditionally
    // unless we hand it a malformed packet (pre-PR-2 a custom stop-and-wait
    // ARQ here required a 200-retry x 25ms loop, retired with the channel).
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) {
        UE_LOGW("flashlight: DebugForceToggle session not connected -- skipping wire send");
        return newState;
    }

    // Snapshot the local cone shape into the packet. DebugForceToggle
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

    if (s->SendReliable(coop::net::ReliableKind::ItemActivate, &p, sizeof(p))) {
        const uint64_t sig = SignaturePayload(p);
        const uint64_t storeSig = (sig == kNoSendYet) ? (kNoSendYet - 1) : sig;
        g_lastSentSig.store(storeSig, std::memory_order_release);
        UE_LOGI("flashlight: DebugForceToggle sent state=%d mode=%u Intensity=%.2f outerCone=%.1f "
                "(senderElementId=0x%08x)",
                p.state, p.mode, p.intensity, p.outerConeAngle, p.senderElementId);
    } else {
        UE_LOGW("flashlight: DebugForceToggle SendReliable failed (state=%d)", p.state);
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

void ApplyToPuppet(void* puppetActor, const coop::net::ItemActivatePayload& payload,
                   uint8_t senderPeerSlot) {
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

    void* light_R = ue_wrap::engine::GetMainPlayerLightR(puppetActor);
    if (!light_R) {
        UE_LOGW("flashlight: puppet light_R missing or dead -- light not toggled "
                "(puppet=%p)", puppetActor);
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

    // A-1 (2026-05-29) Principle-7: all 4 UFunction dispatches below go
    // through ue_wrap::engine wrappers (cached per-process). No more
    // raw R::FindClass / R::FindFunction / ParamFrame plumbing here.

    // 1) SetVisibility(newState, false) -- triggers MarkRenderStateDirty
    //    on the FIRST transition (false->true or true->false). Required
    //    for proxy creation on the orphan puppet (CreateRenderState_
    //    Concurrent runs on dirty mark; light_R's pre-existing
    //    bRegistered=1 + visByte bit-5 pass the cascade).
    if (!ue_wrap::engine::SetSceneComponentVisibility(light_R, newState, false)) {
        UE_LOGW("flashlight: SetSceneComponentVisibility failed (UFunction unresolved or light_R dead)");
    }

    // 2) SetIntensity(packet.intensity) -- mirror sender's exact brightness.
    //    For OFF state, sender's intensity is the BP's "off" sentinel
    //    (~0.2 Unitless) which renders as ~no light. No fallback latch
    //    needed -- the sender sends the truth.
    const float targetIntensity = newState ? payload.intensity : 0.f;
    if (!ue_wrap::engine::SetLightIntensity(light_R, targetIntensity)) {
        UE_LOGW("flashlight: SetLightIntensity failed (UFunction unresolved or light_R dead)");
    }

    // 3) SetOuterConeAngle + SetInnerConeAngle -- mirror sender's cone
    //    shape (default-spread vs focused mode from hold-F). Both
    //    UFunctions internally MarkRenderStateDirty so the proxy
    //    refreshes with the new angles next frame. Only apply when ON
    //    (cone shape on an off light is invisible; saves two UFunction
    //    dispatches per off toggle).
    if (newState) {
        if (payload.outerConeAngle > 0.f) {
            ue_wrap::engine::SetSpotLightOuterConeAngle(light_R, payload.outerConeAngle);
        }
        if (payload.innerConeAngle >= 0.f) {
            ue_wrap::engine::SetSpotLightInnerConeAngle(light_R, payload.innerConeAngle);
        }
    }

    g_echoSuppress.store(false, std::memory_order_release);
    UE_LOGI("flashlight: applied to puppet=%p state=%d Intensity=%.2f outerCone=%.1f innerCone=%.1f mode=%u "
            "(senderPeerSlot=%u senderElementId=0x%08x)",
            puppetActor, newState ? 1 : 0, targetIntensity,
            payload.outerConeAngle, payload.innerConeAngle, payload.mode,
            static_cast<unsigned>(senderPeerSlot), payload.senderElementId);

    // 3D positional click sound at the puppet -- extracted to its own
    // subsystem (see coop/flashlight_click_sound.h). Gated on state-CHANGE
    // (hold-F mode-change packets don't click). Runtime-constructs its
    // own USoundAttenuation, AddToRoot's it for GC stability. Keyed on
    // the resolved peer slot (per-peer state map).
    coop::flashlight_click_sound::PlayIfStateChanged(
        puppetActor, senderPeerSlot, newState);
}

void ApplyToPuppetOrDefer(uint8_t senderPeerSlot, void* puppetActor,
                          const coop::net::ItemActivatePayload& p) {
    if (senderPeerSlot >= coop::players::kMaxPeers) {
        UE_LOGW("flashlight: ApplyToPuppetOrDefer senderPeerSlot=%u out of range -- dropping",
                static_cast<unsigned>(senderPeerSlot));
        return;
    }
    // T2-4 late-joiner peer-state cache: on the HOST (the relay hub), remember
    // each PEER CLIENT's latest item state (whether applied now or deferred) so
    // a peer joining LATER can be caught up to it. Slot 0 (host's own state) is
    // covered by QueueConnectBroadcastForSlot (BuildPayloadFromLocal); this
    // cache is only for OTHER clients' states, which the late joiner can't
    // learn any other way (ItemActivate only fires on toggle). Host-only +
    // peer-slot-only; ReplayPeerStatesToSlot reads it on a new client's edge.
    if (senderPeerSlot >= 1) {
        if (auto* s = g_session.load(std::memory_order_acquire)) {
            if (s->role() == coop::net::Role::Host) {
                g_peerActivateCache[senderPeerSlot] = p;
                g_peerActivateValid[senderPeerSlot] = true;
            }
        }
    }
    if (puppetActor && R::IsLive(puppetActor)) {
        // Puppet is ready -- clear any stale pending entry (defensive:
        // a fresh apply supersedes a still-pending one) + apply now.
        g_pendingApplyValid[senderPeerSlot] = false;
        ApplyToPuppet(puppetActor, p, senderPeerSlot);
        return;
    }
    // Puppet not ready yet -- stash latest-wins. The TickConnect pump
    // will pick it up once the registry has a puppet for this peer.
    g_pendingApplyPayload[senderPeerSlot] = p;
    g_pendingApplyValid[senderPeerSlot] = true;
    UE_LOGI("flashlight: ApplyToPuppetOrDefer puppet not ready for peerSlot=%u "
            "(senderElementId=0x%08x) -- stashing (state=%d Intensity=%.2f)",
            static_cast<unsigned>(senderPeerSlot), p.senderElementId,
            p.state, p.intensity);
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) {
        UE_LOGW("flashlight: QueueConnectBroadcastForSlot called with no session");
        return;
    }
    if (peerSlot < 0 || peerSlot >= static_cast<int>(coop::players::kMaxPeers)) {
        UE_LOGW("flashlight: QueueConnectBroadcastForSlot peerSlot=%d out of range", peerSlot);
        return;
    }
    void* mp = coop::players::Registry::Get().Local();
    if (!mp) {
        // Local mp not yet alive (still in OMEGA / menu). Without a local
        // we have no state to broadcast. Edge will fire again on the next
        // reconnect; this connect-edge case (connected before mp loads)
        // is rare in practice (mp spawns in the loading splash before
        // session.Connected). Drop silently.
        UE_LOGI("flashlight: QueueConnectBroadcastForSlot(slot=%d) no local mp yet -- no broadcast",
                peerSlot);
        return;
    }
    coop::net::ItemActivatePayload p{};
    if (!BuildPayloadFromLocal(mp, p, s)) {
        UE_LOGW("flashlight: QueueConnectBroadcastForSlot(slot=%d) BuildPayloadFromLocal failed",
                peerSlot);
        return;
    }
    // Skip broadcast if LOCAL flashlight is OFF -- the receiver's puppet
    // defaults to OFF on spawn, so an OFF broadcast is a redundant packet.
    // (When the local user toggles to ON later, the normal POST observer
    // path ships it.)
    if (p.state == 0) {
        UE_LOGI("flashlight: QueueConnectBroadcastForSlot(slot=%d) local state is OFF -- "
                "skipping (puppet default is OFF; no replay needed)", peerSlot);
        return;
    }
    // Send to ONE slot only -- existing peers (already-connected)
    // received our state via the original POST-observer broadcast on
    // toggle; re-sending to them would be redundant.
    s->SendReliableToSlot(peerSlot, coop::net::ReliableKind::ItemActivate, &p, sizeof(p));
    const uint64_t sig = SignaturePayload(p);
    const uint64_t storeSig = (sig == kNoSendYet) ? (kNoSendYet - 1) : sig;
    g_lastSentSig.store(storeSig, std::memory_order_release);
    UE_LOGI("flashlight: connect-broadcast slot=%d sent state=%d Intensity=%.2f "
            "outerCone=%.1f mode=%u senderElementId=0x%08x",
            peerSlot, p.state, p.intensity, p.outerConeAngle, p.mode, p.senderElementId);
}

void ReplayPeerStatesToSlot(int newSlot) {
    // T2-4 (host-relay late-joiner): replay every EXISTING peer client's
    // current item state to a newly-joined client so it converges to the
    // live world. QueueConnectBroadcastForSlot covers the host's OWN state;
    // this covers the OTHER clients' states (the late joiner can't learn
    // them otherwise -- ItemActivate is edge-triggered). Host-only.
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    if (newSlot < 1 || newSlot >= static_cast<int>(coop::players::kMaxPeers)) return;
    for (int peer = 1; peer < static_cast<int>(coop::players::kMaxPeers); ++peer) {
        if (peer == newSlot) continue;
        if (!g_peerActivateValid[peer]) continue;
        const coop::net::ItemActivatePayload& p = g_peerActivateCache[peer];
        // Skip OFF -- the joiner's puppet for `peer` defaults to OFF on spawn,
        // so an OFF replay is a redundant packet (matches the host-own-state
        // skip in QueueConnectBroadcastForSlot).
        if (p.state == 0) continue;
        // Stamp senderSlot = the ORIGIN peer so the joiner routes the action
        // to that peer's puppet AND the receiver's eid-range trust check sees
        // the right role (a peer-range senderElementId from a peer slot). The
        // payload already carries the origin peer's senderElementId, which the
        // joiner resolves to that peer's mirror (installed via T2-1
        // PlayerJoined) for puppet routing.
        s->SendReliableToSlot(newSlot, coop::net::ReliableKind::ItemActivate,
                              &p, sizeof(p), static_cast<uint8_t>(peer));
        UE_LOGI("flashlight: T2-4 replayed peer %d state=%d (Intensity=%.2f "
                "senderElementId=0x%08x) to late joiner slot %d",
                peer, p.state, p.intensity, p.senderElementId, newSlot);
    }
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
        UE_LOGI("flashlight: draining deferred apply for peerSlot=%u "
                "(senderElementId=0x%08x state=%d Intensity=%.2f)",
                static_cast<unsigned>(peer), p.senderElementId,
                p.state, p.intensity);
        ApplyToPuppet(puppet, p, peer);
    }
}

void OnDisconnect() {
    int clearedApplies = 0;
    for (uint8_t i = 0; i < coop::players::kMaxPeers; ++i) {
        if (g_pendingApplyValid[i]) ++clearedApplies;
        g_pendingApplyValid[i] = false;
        g_pendingApplyPayload[i] = {};
        // T2-4: drop the late-joiner replay cache too -- it belongs to the
        // now-dead session; replaying it on a fresh session (possibly a
        // different peer reusing the slot) would carry wrong-peer state.
        g_peerActivateValid[i] = false;
        g_peerActivateCache[i] = {};
    }
    if (clearedApplies > 0) {
        UE_LOGI("flashlight: OnDisconnect cleared %d pending apply slot(s)",
                clearedApplies);
    }
}

void OnDisconnectForSlot(int peerSlot) {
    if (peerSlot < 0 || peerSlot >= static_cast<int>(coop::players::kMaxPeers)) return;
    // T2-4: a peer leaving invalidates its cached replay state so a different
    // peer reusing the slot doesn't inherit the departed peer's flashlight.
    g_peerActivateValid[peerSlot] = false;
    g_peerActivateCache[peerSlot] = {};
    if (!g_pendingApplyValid[peerSlot]) return;
    UE_LOGI("flashlight: peer slot %d disconnected -- clearing pending apply (state=%d intensity=%.2f)",
            peerSlot, g_pendingApplyPayload[peerSlot].state,
            g_pendingApplyPayload[peerSlot].intensity);
    g_pendingApplyValid[peerSlot] = false;
    g_pendingApplyPayload[peerSlot] = {};
}

}  // namespace coop::item_activate
