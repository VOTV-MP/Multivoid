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
#include "dev/common.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"
#include "ue_wrap/types.h"

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
void* g_updateFlashlightFn = nullptr;
void* g_flashlightUpdateFn = nullptr;
void* g_flashlightInput13Fn = nullptr;
void* g_flashlightInput14Fn = nullptr;

// Last sent state so the input-event hook doesn't send a packet for
// the F-RELEASE event (which fires the InpActEvt_14 UFunction but
// doesn't change `mp.flashlight`). One value per local process is
// enough; the sender is always the local player. -1 = no send yet.
std::atomic<int> g_lastSentState{-1};

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
        if (light_R && R::IsLive(light_R)) {
            // bVisible lives in USceneComponent VisFlags byte at +0x14C bit 4
            // (per sdk_profile USceneComponent_VisFlagsByte). Earlier draft
            // used a guessed 0x80 offset that landed in vtable territory; the
            // audit-pinned correct offset is the project's existing
            // VisFlagsByte+bit4. Read-only diagnostic; misreads only affect
            // log accuracy, but using the wrong offset risks fault-on-read
            // if the component header isn't fully populated yet.
            const uint8_t flagsByte = *reinterpret_cast<uint8_t*>(
                reinterpret_cast<uint8_t*>(light_R) + P::off::USceneComponent_VisFlagsByte);
            lightVisible = (flagsByte & 0x10) != 0;
        }
        UE_LOGI("flashlight[POST %s] self=%p flashlight=%d hasFL=%d crankFL=%d "
                "light_R=%p light.bVisible=%d", which, self,
                flashlight ? 1 : 0, hasFlashlight ? 1 : 0,
                crankFlashlight ? 1 : 0, light_R, lightVisible ? 1 : 0);
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
    const bool ok = E::SetComponentVisible(light_R, newState, /*propagate=*/false);
    g_echoSuppress.store(false, std::memory_order_release);
    if (!ok) {
        UE_LOGW("flashlight: SetComponentVisible(light_R=%p, %d) failed",
                light_R, newState ? 1 : 0);
    } else {
        UE_LOGI("flashlight: applied to puppet=%p state=%d", puppetActor, newState ? 1 : 0);
    }
}

}  // namespace coop::item_activate
