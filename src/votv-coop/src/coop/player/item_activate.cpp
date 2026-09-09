// coop/player/item_activate.cpp -- flashlight sync (the item-activation lane). See
// coop/player/item_activate.h. Install resolves mainPlayer_C's flashlight UFunctions and registers
// POST observers (retried every pump tick); the observer is the sender, reading the flashlight
// state after the BP ran and sending an ItemActivate; ApplyToPuppet is the receiver, called from
// event_feed's drain with the puppet.

#include "coop/player/item_activate.h"

#include "coop/element/element.h"
#include "coop/player/flashlight_click_sound.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/config/config.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/types.h"

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

// The candidate UFunctions of the flashlight chain, resolved once. updateFlashlight is
// BP-inlined into Flashlight Update and never reaches ProcessEvent, so every candidate gets an
// observer and whichever the BP dispatches wins; the dedupe below absorbs a double fire.
// timerHoldFlashlight is the hold-F mode change (spread to focused), whose observer reads the
// post-mutation mode and cone shape so the receiver mirrors it.
void* g_updateFlashlightFn = nullptr;
void* g_flashlightUpdateFn = nullptr;
void* g_flashlightInput13Fn = nullptr;
void* g_flashlightInput14Fn = nullptr;
void* g_timerHoldFlashlightFn = nullptr;

// The last-sent signature (state, mode, intensity, cone angles): the hold-F mode change keeps
// state on but changes the mode and cones, so a state-only dedupe would drop it. kNoSendYet
// means never sent. One atomic, so the load cannot see a torn flag-plus-signature (a split
// pair let DebugForceToggle on a worker thread update the flag before the signature and an
// observer dedupe a fresh packet).
inline constexpr uint64_t kNoSendYet = 0xFFFFFFFFFFFFFFFFULL;
std::atomic<uint64_t> g_lastSentSig{kNoSendYet};

// The latched "on" intensity, sampled from the local light_R the first time it reads above
// the off-state value; the receiver drives the puppet's light_R with it. VOTV's light_R uses
// ELightUnits::Unitless, where Intensity is a small multiplier (about 0.2 off, roughly 5-10
// on), not the stock 5000 lumens; the fallback is a safe Unitless value used until the latch
// sees a real on value. 0 = not sampled. Both peers run the same BP defaults, so the latch
// reaches the same value on each once its player toggles once.
std::atomic<float> g_latchedOnIntensity{0.f};
inline constexpr float kIntensityOnFallback = 5.f;  // Unitless safe default
inline constexpr float kIntensityOffDefault = 0.f;  // turn light fully off

// The hash of prop_equipment_flashlight_C, the packet's itemClassHash; the _a and _b variants
// both toggle the puppet's light_R and share it.
uint32_t g_flashlightClassHash = 0;

// Atomic: the observer's read races the setter on another thread.
std::atomic<coop::net::Session*> g_session{nullptr};

// Pending receiver-side applies per peer slot: the puppet is created on the peer's first
// PoseSnapshot, and a tight connect edge can race the reliable ItemActivate ahead of that
// unreliable packet, so the latest payload waits here until TickConnect finds the puppet in the
// registry. Latest wins. Game thread.
bool g_pendingApplyValid[coop::players::kMaxPeers] = {};
coop::net::ItemActivatePayload g_pendingApplyPayload[coop::players::kMaxPeers] = {};

// The host's per-client cache of the last item state applied for that peer, not cleared on
// apply: ItemActivate is edge-triggered, so a joiner would otherwise see a flashlight that was
// already on stay dark until its owner toggles. Replayed to a new client on its connect edge.
// Slot 0 is not cached; the host's own state goes through QueueConnectBroadcastForSlot. Host
// only, game thread.
bool g_peerActivateValid[coop::players::kMaxPeers] = {};
coop::net::ItemActivatePayload g_peerActivateCache[coop::players::kMaxPeers] = {};

// Echo suppression around the receiver's apply, so a UFunction invoked on the puppet cannot
// bounce back as a wire packet. Atomic, as the session pointer.
std::atomic<bool> g_echoSuppress{false};

bool ProbeLogEnabled() {
    // Read once per process: the observer is hot.
    static const bool s_enabled = ::coop::config::ResolveFlag(::coop::config_registry::rows::flashlight_log);
    return s_enabled;
}

// The local player's flashlight state and cone shape as a payload; direct memory reads on the
// calling thread (the observer on the game thread, DebugForceToggle inside its posted lambda).
// False if light_R is dead.
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
    // The sender's own Player element id; 0 before it is allocated (the boot window), and the
    // receiver then falls back to the sender slot. Ids are unique across the host and peer ranges.
    {
        const coop::element::ElementId selfEid =
            coop::players::Registry::Get().LocalPlayerElementId();
        out.senderElementId =
            (selfEid == coop::element::kInvalidId) ? 0u : selfEid;
    }
    out.state           = fl.flashlight ? 1 : 0;
    out.flags           = 0;  // no actor key
    out.mode            = fl.mode;
    out.actorKeyHash    = 0;
    out.intensity       = snap.intensity;
    out.outerConeAngle  = snap.outerConeAngle;
    out.innerConeAngle  = snap.innerConeAngle;
    return true;
}

// FNV1a over the mutable fields, to dedupe the observer fires of one logical change (a press
// fires the input event, Flashlight Update and updateFlashlight in a chain; hold-F fires
// timerHoldFlashlight and maybe some of the others).
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
    // Which hook fired, for the probe log.
    const char* which =
        (function == g_flashlightUpdateFn)     ? "Flashlight_Update" :
        (function == g_updateFlashlightFn)     ? "updateFlashlight"  :
        (function == g_flashlightInput13Fn)    ? "InpActEvt_13"      :
        (function == g_flashlightInput14Fn)    ? "InpActEvt_14"      :
        (function == g_timerHoldFlashlightFn)  ? "timerHoldFlashlight" :
        "<unknown>";

    // A receiver-applied change must not bounce back as a packet.
    if (g_echoSuppress.load(std::memory_order_acquire)) return;

    // Puppets are skipped: a fire on a puppet would send its state back to the peer that authored
    // it. A controller is the local-versus-puppet discriminator.
    if (!E::GetController(self)) return;

    // The state after the BP ran: the bools carry the new value. One wrapper read.
    ue_wrap::engine::MainPlayerFlashlightState fl{};
    if (!ue_wrap::engine::ReadMainPlayerFlashlightState(self, fl)) return;
    const bool flashlight = fl.flashlight;
    const bool hasFlashlight = fl.hasFlashlight;
    const bool crankFlashlight = fl.crankFlashlight;

    if (ProbeLogEnabled()) {
        // The probe line: whether the BP early-returned without a flashlight (the bool unchanged)
        // and whether light_R's visibility tracks the bool; the crank variant is logged separately
        // since it is not sent.
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

    // The crank lantern (_c) toggles its own light components, not the player's light_R; it is not
    // sent.
    if (crankFlashlight) {
        if (ProbeLogEnabled()) {
            UE_LOGI("flashlight: crank lantern (_c) -- nothing sent");
        }
        return;
    }

    // No send when the BP guard early-returned for no equipped flashlight, or a recook that always
    // sets the bool on F would ship spurious activations.
    if (!hasFlashlight) {
        if (ProbeLogEnabled()) {
            UE_LOGI("flashlight: hasFlashlight=false -- BP early-returned, no wire send");
        }
        return;
    }

    // The "on" intensity latches the first time it reads clearly above the off sentinel (about 0.2
    // Unitless); above 1.0 filters the sentinel.
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

    // Only the first observer of a logical change sends: the signature covers state, mode,
    // intensity and cones.
    const uint64_t sig = SignaturePayload(p);
    // A hash equal to kNoSendYet is shifted by one so the sentinel stays unambiguous.
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

    // The field flip runs on the game thread (UObject memory), posted and awaited from this worker
    // thread; the autotest calls this from its worker, never inside a posted lambda. The local
    // light_R's intensity is driven too, so the sender's own light toggles: the BP path is gated
    // by input state reflection cannot reach, so calling Flashlight Update by reflection produces
    // no visible toggle, and writing the bool and the intensity is the pair the BP would write.
    auto done = std::make_shared<std::atomic<int>>(0);
    auto newStateOut = std::make_shared<std::atomic<bool>>(false);
    GT::Post([mp, done, newStateOut] {
        // Read and flip through the wrappers.
        ue_wrap::engine::MainPlayerFlashlightState cur{};
        if (!ue_wrap::engine::ReadMainPlayerFlashlightState(mp, cur)) {
            done->store(1, std::memory_order_release);
            return;
        }
        const bool newState = !cur.flashlight;
        ue_wrap::engine::WriteMainPlayerFlashlight(mp, newState);

        // The local light_R's intensity follows the new state, through the same SetIntensity the
        // puppet receiver uses.
        void* light_R = ue_wrap::engine::GetMainPlayerLightR(mp);
        float localTarget = kIntensityOffDefault;
        if (newState) {
            localTarget = g_latchedOnIntensity.load(std::memory_order_acquire);
            if (localTarget == 0.f) localTarget = kIntensityOnFallback;
        }
        if (light_R) {
            ue_wrap::engine::SetLightIntensity(light_R, localTarget);
        }
        // The on value latches too, so the receiver gets the intensity in use rather than the
        // fallback.
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

    // SendReliable queues inside the channel and fails only on a malformed packet.
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) {
        UE_LOGW("flashlight: DebugForceToggle session not connected -- skipping wire send");
        return newState;
    }

    // The cone shape is whatever the local light_R carries: this bypasses the BP, so the cones do
    // not reflect a real mode change, which the autotest does not exercise.
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
    // FNV-1a 32-bit over the UTF-16 bytes; the same encoding on every peer.
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
        // mainPlayer_C is not loaded yet (the menu); retry next pump tick.
        return;
    }

    // POST observers on every candidate of the chain (the two input events, Flashlight Update,
    // updateFlashlight, timerHoldFlashlight): the inner functions are BP-inlined into the input
    // events, which the engine's input system dispatches through ProcessEvent, and registering all
    // of them survives a recook that inlines differently. The signature dedupes a double fire.
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

    // The puppet's existing light_R is driven through SetVisibility and SetIntensity only. The
    // puppet's flashlight bool is not written (it triggers the updateFlashlight ubergraph, an
    // "Invalid prop" emitter), no component is added (component-attached BP listeners), and
    // flashlightStateChanged is not broadcast (no subscribers). SetVisibility is the key: a
    // USceneComponent UFunction whose body sets the bit and calls the OnVisibilityChanged virtual,
    // which for a light calls MarkRenderStateDirty (native, so no BP observer fires) and schedules
    // the render-state recreation that builds the scene proxy; a direct bit write does none of
    // that and produced no visible change.
    g_echoSuppress.store(true, std::memory_order_release);

    // The four dispatches go through the cached engine wrappers.

    // 1) SetVisibility: marks the render state dirty on the first transition, which creates the
    // proxy on the orphan puppet.
    if (!ue_wrap::engine::SetSceneComponentVisibility(light_R, newState, false)) {
        UE_LOGW("flashlight: SetSceneComponentVisibility failed (UFunction unresolved or light_R dead)");
    }

    // 2) SetIntensity: the sender's exact brightness; off is 0.
    const float targetIntensity = newState ? payload.intensity : 0.f;
    if (!ue_wrap::engine::SetLightIntensity(light_R, targetIntensity)) {
        UE_LOGW("flashlight: SetLightIntensity failed (UFunction unresolved or light_R dead)");
    }

    // 3) The cone angles (spread versus focused); both mark the render state dirty. Only when on: a
    // cone on an off light is invisible.
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

    // The positional click at the puppet (coop/flashlight_click_sound), on a state change only; a
    // mode-change packet does not click.
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
    // The host remembers each client's latest item state, applied or deferred, for a later joiner;
    // the host's own state goes through QueueConnectBroadcastForSlot.
    if (senderPeerSlot >= 1) {
        if (auto* s = g_session.load(std::memory_order_acquire)) {
            if (s->role() == coop::net::Role::Host) {
                g_peerActivateCache[senderPeerSlot] = p;
                g_peerActivateValid[senderPeerSlot] = true;
            }
        }
    }
    if (puppetActor && R::IsLive(puppetActor)) {
        // The puppet is ready: a stale pending entry clears and the apply runs now.
        g_pendingApplyValid[senderPeerSlot] = false;
        ApplyToPuppet(puppetActor, p, senderPeerSlot);
        return;
    }
    // No puppet yet: stashed, latest wins, until TickConnect finds it.
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
        // No local player yet (the menu): nothing to broadcast; the player spawns in the loading
        // splash before the session connects, so this is rare.
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
    // Off is not sent: the puppet spawns off, and the observer ships a later toggle.
    if (p.state == 0) {
        UE_LOGI("flashlight: QueueConnectBroadcastForSlot(slot=%d) local state is OFF -- "
                "skipping (puppet default is OFF; no replay needed)", peerSlot);
        return;
    }
    // To the one slot: the connected peers got the state on the toggle.
    s->SendReliableToSlot(peerSlot, coop::net::ReliableKind::ItemActivate, &p, sizeof(p));
    const uint64_t sig = SignaturePayload(p);
    const uint64_t storeSig = (sig == kNoSendYet) ? (kNoSendYet - 1) : sig;
    g_lastSentSig.store(storeSig, std::memory_order_release);
    UE_LOGI("flashlight: connect-broadcast slot=%d sent state=%d Intensity=%.2f "
            "outerCone=%.1f mode=%u senderElementId=0x%08x",
            peerSlot, p.state, p.intensity, p.outerConeAngle, p.mode, p.senderElementId);
}

void ReplayPeerStatesToSlot(int newSlot) {
    // Every other client's cached state, replayed to a new client so it converges; ItemActivate is
    // edge-triggered, and the joiner cannot learn these otherwise. Host only.
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    if (newSlot < 1 || newSlot >= static_cast<int>(coop::players::kMaxPeers)) return;
    for (int peer = 1; peer < static_cast<int>(coop::players::kMaxPeers); ++peer) {
        if (peer == newSlot) continue;
        if (!g_peerActivateValid[peer]) continue;
        const coop::net::ItemActivatePayload& p = g_peerActivateCache[peer];
        // Off is skipped: the joiner's puppet for that peer spawns off.
        if (p.state == 0) continue;
        // The sender slot is the origin peer, so the joiner routes the action to that peer's puppet
        // and the eid-range trust check sees a peer-range id from a peer slot.
        s->SendReliableToSlot(newSlot, coop::net::ReliableKind::ItemActivate,
                              &p, sizeof(p), static_cast<uint8_t>(peer));
        UE_LOGI("flashlight: T2-4 replayed peer %d state=%d (Intensity=%.2f "
                "senderElementId=0x%08x) to late joiner slot %d",
                peer, p.state, p.intensity, p.senderElementId, newSlot);
    }
}

void TickConnect() {

    // The pending applies drain once the registry has a live puppet for the slot.
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
        // The replay cache goes too: on a fresh session a different peer may reuse the slot.
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
    // A leaving peer's cached state goes, so a peer reusing the slot does not inherit its
    // flashlight.
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
