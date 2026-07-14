// coop/prop_lifecycle.cpp -- Aprop_C spawn/destroy/extract wire observers.
//
// Extracted from harness/harness.cpp (2026-05-25 modular refactor).
// See coop/prop_lifecycle.h for the public interface.

#include "coop/props/prop_lifecycle.h"

#include "prop_lifecycle_detail.h"  // co-located private header (src tree, not include/)

#include "coop/element/element.h"
#include "coop/element/mirror_manager.h"  // SyncDestroyedTrackedProp reads the key off the element
#include "coop/element/prop.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/creatures/kerfur_convert.h"  // TryAdoptFreshKerfurProp (kerfur-conversion first refusal, take-8)
#include "coop/creatures/kerfur_entity.h"   // IsKerfurPropClass (the first-refusal class gate)
#include "coop/creatures/kerfur_form_assembler.h"  // IsCapturedForm -- deterministic conversion-successor peek (2026-07-14 sole-express)
#include "coop/props/prop_echo_suppress.h"
#include "coop/props/prop_element_tracker.h"
#include "coop/props/prop_synth_key.h"
#include "coop/props/remote_prop.h"
#include "coop/props/remote_prop_spawn.h"
#include "coop/props/join_membership_sweep.h"  // anti-smear 2026-06-30: claim+sweep extracted out of remote_prop_spawn
#include "coop/session/world_load_episode.h"     // v107 host-wipe fix: suppress keyed-destroy broadcast during the client world-load
#include "coop/props/prop_drop_intent.h"       // F2 Inc-1: park a client keyed-destroy key for a later host-auth re-place
#include "ue_wrap/call.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/fname_utils.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"
#include "ue_wrap/prop.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"
#include "ue_wrap/types.h"
#include "ue_wrap/ufunction_hook.h"  // v106 destroy seam: Func-patch on K2_DestroyActor

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace coop::prop_lifecycle {
namespace {

namespace P = ue_wrap::profile;
namespace R = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;
namespace PT = coop::prop_element_tracker;

// Cached session pointer (set on Install/InstallInventory). Observers read
// role()/connected()/SendProp*() through this. nullptr until first Install.
//
// Audit C2 (2026-05-27): atomic Session* (was plain pointer). Observers fire
// from parallel-anim worker threads per game_thread.cpp's header comment; a
// plain-pointer deref races with harness calling SetSession(nullptr) on
// shutdown. item_activate.cpp + weather_sync.cpp already use atomic; this
// file had diverged. Mirrors item_activate's pattern exactly: helper LoadSession
// for the read-many sites, .store(memory_order_release) for the set sites.
// (g_session_ptr definition moved to namespace scope below -- shared with
// prop_destroy_seam.cpp via prop_lifecycle_detail.h; LoadSession is inline there.)

// Install idempotency.
bool g_propInitScanDone = false;
bool g_propDestroyObserverInstalled = false;
// Late-load catch for the non-Aprop_C keyed garbage classes (chipPile/clump/
// trashBits). The one-shot Aprop-lineage Init scan below latches g_propInitScanDone
// on the FIRST keyed Init found (Aprop_C, which loads with the world), so garbage
// BP classes that load a moment later are missed -> they never broadcast PropSpawn
// -> trash-ball interaction never syncs (root-caused 2026-05-31). This latches
// once all three garbage Init UFunctions are hooked; see RegisterExtraKeyedInitObservers.
bool g_extraKeyedInitDone = false;
std::vector<void*> g_registeredPropInitFns;

// The takeObj-in-flight bracket (g_takeObjInFlight) is defined in
// prop_container_extract.cpp with the takeObj PRE/POST pair; shared via
// prop_lifecycle_detail.h (the Init POST body below reads it, OnDisconnect
// clears it).

// Per-actor lifecycle bookkeeping (ProcessedInit dedupe set + KnownKeyedProps
// maintained set + Prop Element shadow) extracted to
// coop/prop_element_tracker.{h,cpp} (M-1, 2026-05-29). All references below
// resolved via `coop::prop_element_tracker::*`.

// Per-feature PropSpawn/PropDestroy retry queues retired 2026-05-27:
// reliable_channel.cpp now buffers internally so Send() always succeeds.
// The Enqueue*/DrainPending* helpers, the harness per-tick drain calls,
// and the OnDisconnect "dropped" counters all went with them (RULE 2).

// Forward declarations for observer callbacks.
void GrabObserver_Aprop_Init_POST(void* self, void* function, void* params);

// Synth-key minting for non-Aprop_C keyed-interactables (chipPile/clump/
// trashBits) extracted to coop/prop_synth_key.{h,cpp} (M-1, 2026-05-29).

// DestroyLocalProp promoted to the public coop::prop_lifecycle API 2026-06-10
// (P2 claim sweep in remote_prop_spawn calls it cross-TU). Definition lives
// after the anon namespace closes; the anon-namespace callers below see the
// declaration via prop_lifecycle.h.

// Forward declaration so the GT-thread-defer wrapper can refer to the body.
void GrabObserver_Aprop_Init_POST_Body(void* self);

void GrabObserver_Aprop_Init_POST(void* self, void* /*function*/, void* /*params*/) {
    auto* s = LoadSession();
    if (!self || !s) return;
    // Audit Fix 3 (2026-05-27): the observer body invokes UFunctions
    // (GetActorLocation/Rotation, GetKey on chipPile/clump). ProcessEvent
    // is game-thread-only per project invariant. The observer can fire on
    // a parallel-anim task-graph worker per ue_wrap/game_thread.h:118-120.
    // Defer the body to GT when off-thread; the actor pointer is captured
    // and re-validated via R::IsLive inside the deferred body.
    if (!GT::IsGameThread()) {
        GT::Post([self] { GrabObserver_Aprop_Init_POST_Body(self); });
        return;
    }
    GrabObserver_Aprop_Init_POST_Body(self);
}

void GrabObserver_Aprop_Init_POST_Body(void* self) {
    if (!self) return;
    // H2-redux 2026-05-28: maintain the known-keyed-props set BEFORE the
    // session/echo-suppress gates so the set is warm by the time a peer
    // joins (e.g. populated during the pre-handshake save-load pass).
    // IsLive + IsKeyedInteractable promoted ahead of the session-connected
    // gate to filter non-keyed actors out of the set. The hot path
    // (post-connect spawn broadcast) pays two extra reflection probes per
    // Init event -- acceptable given Init firing rate is bursty (level
    // load) not steady-state.
    if (!R::IsLive(self)) return;
    if (!ue_wrap::prop::IsKeyedInteractable(self)) return;
    // CDO filter (re-audit 2026-05-28): IsLive doesn't filter CDOs (they're
    // persistent UObjects). A CDO whose Init fires (level streaming /
    // hot-reload edge cases) would enter g_knownKeyedProps permanently --
    // CDOs are never K2_DestroyActor'd, so UnmarkKnownKeyedProp never runs.
    // This matches the seed scan's Default__ guard so the set never holds
    // a CDO under any path. (Same filter justification cited in
    // prop_snapshot.cpp:StartEnumerationFor for removing the snapshot-side
    // CDO check.)
    {
        const std::wstring nm = R::ToString(R::NameOf(self));
        if (nm.rfind(L"Default__", 0) == 0) return;
    }
    // CHILD-ACTOR EXCLUSION (2026-07-12, take-7 floating-CCTV RCA; predicate + full rationale:
    // ue_wrap::engine::IsChildActor + prop_element_tracker::MarkPropElement). A parent-owned
    // sub-actor (kerfur eye cam) must neither enter the known-keyed set nor BROADCAST PropSpawn
    // -- the parent's SCS re-creates it on every peer; broadcasting minted a standalone floating
    // camera mirror on the joiner at every kerfur toggle (host log 13:59:44 waves).
    if (ue_wrap::engine::IsChildActor(self)) {
        UE_LOGI("grab_hook[Aprop.Init POST]: child actor %p cls='%ls' -- skip (parent-owned "
                "sub-actor; no independent identity, no broadcast)",
                self, R::ClassNameOf(self).c_str());
        return;
    }
    PT::MarkKnownKeyedProp(self);

    auto* s = LoadSession();
    if (!s) return;
    if (!s->connected()) return;                      // pre-handshake save-load -> skip
    if (coop::prop_echo_suppress::ConsumeIncomingSpawn(self)) {
        UE_LOGI("grab_hook[Aprop.Init POST]: actor %p was wire-received -- skip broadcast (echo suppression)",
                self);
        return;
    }
    if (PT::HasProcessedInit(self)) {
        UE_LOGI("grab_hook[Aprop.Init POST]: actor %p already processed -- skip (super-call dedupe)",
                self);
        return;
    }
    PT::MarkProcessedInit(self);

    // Storage-container spawn fix (2026-05-25): defer broadcast for actors
    // spawned from inside a takeObj call. takeObj POST is the canonical
    // broadcaster (sees the saved-Key-restored actor after loadData).
    if (g_takeObjInFlight.load(std::memory_order_relaxed)) {
        UE_LOGI("grab_hook[Aprop.Init POST]: actor %p spawned inside takeObj -- defer to takeObj POST (Key not yet restored by loadData)",
                self);
        return;
    }

    // v5 Phase 5N Stream B: host-authoritative intermediate-variant
    // suppression. Client destroys local intermediate; mature variant
    // will arrive via the wire.
    const std::wstring cls = R::ClassNameOf(self);
    if (s->role() == coop::net::Role::Client) {
        if (IsWireSuppressedPropClass(cls)) {
            UE_LOGI("spawner-suppress[client.Init]: scheduling deferred destroy for local intermediate variant '%ls' actor=%p (host-authoritative; mature variant will arrive via wire)",
                    cls.c_str(), self);
            DestroyLocalProp(self, /*deferred=*/true);
            return;
        }
        // Aprop_C lineage stays host-authoritative (save-persisted; client's
        // local save-load is its OWN copy, doesn't write to host). For non-
        // Aprop_C interactables (chipPile/clump/trashBitsPile -- "transient
        // world litter" per RE, no save lineage), client interactions
        // (toClump morph spawn) MUST propagate to host so the peer sees
        // what the player just made. Fall through to broadcast in that
        // case; otherwise return.
        if (ue_wrap::prop::IsDescendantOfProp(self)) {
            // [ROCK-DROP DIAG 2026-07-08, RULE-2-exempt] A CLIENT-originated Aprop_C world
            // spawn (R-drop/place = simulateDrop -> FinishSpawningActor -> a FRESH actor,
            // save Key restored by loadData) is skipped here by the host-authoritative
            // assumption -> the host never learns of a client-PLACED prop -> it is invisible
            // until an E-grab expresses it. This return was SILENT; log the skip so a repro
            // shows the smoking gun (key+eid to correlate with the host log's PropDestroy).
            const coop::element::ElementId dropEid = PT::GetPropElementIdForActor(self);
            const ue_wrap::FVector dloc = ue_wrap::engine::GetActorLocation(self);
            UE_LOGI("[ROCK-DROP] CLIENT Aprop spawn NOT authored (host-auth skip): cls='%ls' key='%ls' "
                    "eid=%u loc=(%.1f,%.1f,%.1f) -- host will NOT see this client-placed prop",
                    cls.c_str(), ue_wrap::prop::GetInteractableKeyString(self).c_str(),
                    (dropEid == coop::element::kInvalidId) ? 0u : static_cast<unsigned>(dropEid),
                    dloc.X, dloc.Y, dloc.Z);
            return;  // Aprop_C: host-authoritative world spawn, skip client broadcast
        }
        // Non-Aprop_C interactable: fall through to broadcast.
    }

    // Host (always) + Client (for non-Aprop_C only) reach here.
    if (IsWireSuppressedPropClass(cls)) {
        UE_LOGI("spawner-suppress[host.Init]: skipping broadcast for intermediate-variant '%ls' actor=%p (host-authoritative; will broadcast mature variant on transform)",
                cls.c_str(), self);
        return;
    }
    if (IsPerPlayerPropClass(cls)) {
        UE_LOGI("grab_hook[Aprop.Init POST]: skipping broadcast for per-player '%ls' actor=%p (each peer owns its own)",
                cls.c_str(), self);
        return;
    }
    // KERFUR-CONVERSION FIRST REFUSAL (2026-07-12, take-8 host-own toggle dupe RCA). A fresh
    // prop_kerfurOmega_C may be the turn_off verb's output: the kerfur layer owns kerfur-form
    // expression (redesign 10.3 -- ONE entity, KerfurConvert is the sole conversion wire signal,
    // never a generic PropSpawn). This body is the express funnel for the Init-POST observer AND
    // ExpressSpawnedProp (the FinishSpawningActor seam drain) -- the exact lane that out-raced the
    // 5 Hz death-watch poll and left the joiner with an NPC mirror + a generic prop mirror = the
    // dupe. An ordinary kerfur prop spawn (hand-place / purchase) returns false and keeps the
    // generic same-tick expression.
    // KERFUR-CONVERSION SOLE EXPRESS (2026-07-14 -- REPLACES the take-8 racy dead-NPC suppression).
    // The DETERMINISTIC test: is this prop the successor the form assembler captured IN-BRACKET
    // (IsCapturedForm -- a NON-consuming peek; the deferred converge still consumes the slot)? If
    // so the kerfur layer owns the wire expression (redesign 10.3: ONE entity, KerfurConvert is the
    // SOLE conversion signal, never a generic PropSpawn) -> TRACK below (so host_spawn_watcher / M2
    // defer via the tracked flag) but SKIP the SendPropSpawn broadcast (gate at line ~317).
    // The OLD suppressor gated on "a dead kerfur NPC within 500cm" -- a PROXY that RACED the verb's
    // spawn-vs-destroy order: when the source NPC was still alive at Init POST (SPAWN->Init POST->
    // DESTROY) it DECLINED, the keyed express leaked, and the 2a-capture converge double-expressed =
    // the 2026-07-14 host-own turn_off dupe. The capture is the fact itself (this prop IS B),
    // race-free, and covers both the destroy-first and spawn-first orderings.
    // [[lesson-new-generic-lane-must-inherit-owner-boundaries]]
    const bool kerfurConvSuccessor =
        coop::kerfur_entity::IsKerfurPropClass(R::ClassOf(self)) &&
        coop::kerfur_form_assembler::IsCapturedForm(self);
    if (kerfurConvSuccessor) {
        // Converge SYNCHRONOUSLY here if the source NPC already died (TryAdopt's dead-NPC match);
        // otherwise it no-ops and the POLL death-watch converges later via the same captured-B.
        // Gating TryAdopt behind the capture also kills its old false-positive (a hand-placed kerfur
        // prop near a coincidentally-dead kerfur NPC). NO early return -- fall through to MarkPropElement.
        coop::kerfur_convert::TryAdoptFreshKerfurProp(self);
    }

    coop::net::PropSpawnPayload p{};
    p.className.len = 0;
    for (size_t i = 0; i < cls.size() && i < 63; ++i) {
        p.className.data[p.className.len++] = static_cast<char>(cls[i]);
    }
    std::wstring keyStr = ue_wrap::prop::GetInteractableKeyString(self);
    // 2026-05-27: for non-Aprop_C interactables (chipPile/clump/trashBits)
    // whose BP doesn't auto-mint a NewGuid Key, synthesize one before the
    // None-skip guard. Probe confirmed clump.GetKey returns NAME_None on
    // fresh-spawn -- the None-skip would otherwise drop every chipPile
    // morph silently.
    keyStr = coop::prop_synth_key::EnsureKeyForBroadcast(self, keyStr);
    // UE4 FName(NAME_None) stringifies to "None" -- treat it as unkeyed.
    // For Aprop_C this still defers (BP UCS will mint on a subsequent
    // Init pass after loadData / setKey). For non-Aprop_C we just minted
    // above; if STILL None here something went wrong with setKey.
    if (keyStr.empty() || keyStr == L"None") {
        UE_LOGI("grab_hook[Aprop.Init POST]: actor %p (class '%ls') has unset key '%ls' -- skip (unkeyed = non-syncable)",
                self, cls.c_str(), keyStr.c_str());
        return;
    }
    // Tier 3 Props migration 2026-05-28: create the Prop Element shadow at
    // the Init POST broadcast site so it has the resolved Key (m_name) +
    // class (m_typeName). Idempotent w.r.t. the seed-scan creation path
    // (early-out if g_actorToPropElementId already has this actor).
    // KEY-UNIQUENESS (2026-07-11): Mark may RE-KEY a duplicate (host key
    // authority) -- the payload below must carry the enrolled key, not the
    // pre-rekey one.
    keyStr = PT::MarkPropElement(self, keyStr, cls);
    p.key.len = 0;
    for (size_t i = 0; i < keyStr.size() && i < 31; ++i) {
        p.key.data[p.key.len++] = static_cast<char>(keyStr[i]);
    }
    const auto loc = ue_wrap::engine::GetActorLocation(self);
    const auto rot = ue_wrap::engine::GetActorRotation(self);
    p.locX = loc.X; p.locY = loc.Y; p.locZ = loc.Z;
    p.rotPitch = ue_wrap::NormalizeAxis(rot.Pitch);
    p.rotYaw   = ue_wrap::NormalizeAxis(rot.Yaw);
    p.rotRoll  = ue_wrap::NormalizeAxis(rot.Roll);
    // v54: real scale + the list_props identity row + SP-parity bools (a
    // fresh gameplay spawn IS actively simulating -- kSimulatePhysics stays
    // unconditional per the P1 decision; the parity bits ride along so the
    // mirror's init() resolves the true mesh/mass/collision, not CDO 'cube').
    const auto scl = ue_wrap::engine::GetActorScale3D(self);
    p.scaleX = scl.X; p.scaleY = scl.Y; p.scaleZ = scl.Z;
    p.physFlags = coop::net::propspawn_flags::kSimulatePhysics;
    p.propName.len = 0;
    if (ue_wrap::prop::IsDescendantOfProp(self)) {
        if (ue_wrap::prop::IsHeavy(self))   p.physFlags |= coop::net::propspawn_flags::kIsHeavy;
        if (ue_wrap::prop::IsFrozen(self))  p.physFlags |= coop::net::propspawn_flags::kFrozen;
        if (ue_wrap::prop::IsStatic(self))  p.physFlags |= coop::net::propspawn_flags::kStatic;
        if (ue_wrap::prop::IsSleeping(self)) p.physFlags |= coop::net::propspawn_flags::kSleep;
        if (ue_wrap::prop::ReadRemoveWOrespawn(self)) {
            p.physFlags |= coop::net::propspawn_flags::kRemoveWOrespawn;
        }
        const std::wstring nm = ue_wrap::prop::GetPropNameString(self);
        for (size_t i = 0; i < nm.size() && i < 31; ++i) {
            p.propName.data[p.propName.len++] = static_cast<char>(nm[i]);
        }
    }
    p.initLinVelX = p.initLinVelY = p.initLinVelZ = 0.f;
    p.initAngVelX = p.initAngVelY = p.initAngVelZ = 0.f;
    UE_LOGI("grab_hook[Aprop.Init POST]: HOST broadcasting SPAWN cls='%ls' key='%ls' loc=(%.1f,%.1f,%.1f) heavy=%d frozen=%d",
            cls.c_str(), keyStr.c_str(), p.locX, p.locY, p.locZ,
            (p.physFlags & coop::net::propspawn_flags::kIsHeavy)  ? 1 : 0,
            (p.physFlags & coop::net::propspawn_flags::kFrozen)   ? 1 : 0);
    // v12 (2026-05-28): populate elementId from the Prop Element shadow,
    // translating kInvalidId -> 0 (wire sentinel per protocol.h contract).
    // (v15 also stamped a senderContext byte; v16 PR-FOUNDATION-1b
    // moved stale-gen defense to the header senderEpoch.)
    {
        const coop::element::ElementId eid = PT::GetPropElementIdForActor(self);
        p.elementId = (eid == coop::element::kInvalidId) ? 0u : eid;
    }
    // 2026-05-27 reliable-channel rewrite: Send always succeeds (FIFO queue
    // internal to the channel). The previous EnqueuePropSpawnForRetry fallback
    // path retired as RULE 2 baggage.
    // SOLE-EXPRESS gate (2026-07-14): a kerfur conversion successor is TRACKED above (so
    // host_spawn_watcher / M2 defer) but NOT broadcast here -- KerfurConvert is its only wire
    // signal. Skip ONLY the SendPropSpawn; the self-claim below is unconditional (it keys on
    // tracking, not on the broadcast, so snapshot-sweep safety is preserved).
    if (kerfurConvSuccessor) {
        UE_LOGI("grab_hook[Aprop.Init POST]: kerfur conversion successor %p key='%ls' -- generic "
                "PropSpawn SUPPRESSED (KerfurConvert is the sole express; tracked so the other "
                "broadcasters defer)", self, keyStr.c_str());
    } else {
        s->SendPropSpawn(p);
    }
    // Fork B 2c: self-claim -- this peer just wire-expressed the spawn; an
    // open snapshot bracket's sweep must not destroy it as "unclaimed".
    coop::join_membership_sweep::RecordClaimIfTracking(self);
}

// The DESTROY SEAM -- bidirectional destroy broadcast (host + client), echo-suppressed
// via the remote_prop incoming-destroy set. v106 (2026-07-07): fed by a UFunction::Func
// patch on Actor.K2_DestroyActor (context = the dying actor), which fires for EVERY
// dispatch route incl. the EX_CallMath destroys the old ProcessEvent PRE observer could
// never see (the R-pickup putObjectInventory2 destroy, the pile/clump morph destroys).
// Runs POST-native: K2_DestroyActor only marks PendingKill, so class/property reads on
// `self` are still valid here (the element's cached key never needs the actor anyway).
// (DestroySeamBody + OnK2DestroyFunc: EXTRACTED to prop_destroy_seam.cpp, 2026-07-10 soft-cap.)

// ---- propInventory takeObj PRE/POST pair + InstallInventory: EXTRACTED to
// prop_container_extract.cpp (2026-07-10 soft-cap extraction, second slice).

// Late-load catch for the non-Aprop_C keyed-interactable garbage/trash classes
// (actorChipPile_C / prop_garbageClump_C / trashBitsPile_C -- the "мусорные
// шарики"). These BP classes load LAZILY on first world encounter, frequently
// AFTER the one-shot Aprop_C-lineage Init scan in Install() has already latched
// g_propInitScanDone. That scan therefore never hooks their Init UFunction, so a
// freshly spawned clump never fires GrabObserver_Aprop_Init_POST -> never
// broadcasts PropSpawn -> the receiver has no entity to drive -> trash-ball
// pickup/carry/throw does not sync at all (root-caused 2026-05-31).
//
// Fix (RULE 1): resolve each class by name and hook its OWN Init UFunction
// directly, with a per-class sticky latch + an overall latch so the work stops
// (O(1)) once all are hooked. This is NOT a per-tick full-GUObjectArray rescan
// (which would re-arm the 19 GB wstring bomb the prop.cpp ResolveExtraBases
// sticky atomics exist to prevent); it does at most 3 FindClass/FindFunction per
// tick, and only between world-load and the moment all three classes are present
// (a few seconds), mirroring the existing FindClass-until-loaded pattern Install
// already uses for prop_C / Actor. Caller gates it on g_propInitScanDone so it
// never churns at the menu (before any world prop has loaded).
//
// FindFunction(cls, "Init") returns the Init UFunction OWNED by cls (OuterOf ==
// cls), exactly matching the Aprop-lineage scan's owning-class filter: a class
// that overrides Init is hooked here; one that only inherits a base Init is
// already covered by that base's registration (deduped against
// g_registeredPropInitFns). Game thread only (called from Install via net_pump).
bool RegisterExtraKeyedInitObservers() {
    if (g_extraKeyedInitDone) return true;
    struct Extra { const wchar_t* cls; bool* done; };
    static bool sTrash = false, sClump = false, sChip = false, sFood = false;
    static int  sAttempts = 0;
    constexpr int kMaxAttempts = 120;  // ~2 min at the ~1 Hz throttled call rate
    const Extra extras[] = {
        { L"trashBitsPile_C",     &sTrash },
        { L"prop_garbageClump_C", &sClump },
        { L"actorChipPile_C",     &sChip  },
        // prop_food_C (2026-06-11 pinecone-scare RE): the food base OWNS an Init
        // override and loads LATE (after the one-shot subclass scan latched), so
        // its derived leaves -- prop_food_pinecone_C (the RNG pinecone scare),
        // and every other food prop with no Init of its own -- dispatch THIS
        // unhooked Init and never live-broadcast their spawn. Empirically proven:
        // a force-spawned prop_food_pinecone_C produced no host Init-POST/SPAWN
        // line; the client only saw it 30 s late + at rest via the snapshot drain
        // (the scare drop/bounce lost). Registering prop_food_C::Init here closes
        // it for the whole food lineage (the same late-load gap as the trash
        // classes above).
        { L"prop_food_C",         &sFood  },
    };
    constexpr int kExtraCount = static_cast<int>(std::size(extras));
    const std::wstring kInitName(P::name::PropInitFn);
    int done = 0;
    for (const auto& e : extras) {
        if (*e.done) { ++done; continue; }
        void* cls = R::FindClass(e.cls);
        if (!cls) continue;  // BP class not loaded yet -- retry next Install() tick
        void* initFn = R::FindFunction(cls, kInitName.c_str());
        if (!initFn) {
            // Class loaded but owns no Init override -> it dispatches an inherited
            // base Init, already coverable via that base. Nothing of our own to
            // register; stop retrying this one.
            *e.done = true; ++done;
            UE_LOGI("grab_hook[extra]: %ls owns no Init UFunction (inherits base) -- nothing to hook", e.cls);
            continue;
        }
        bool already = false;
        for (void* fn : g_registeredPropInitFns) if (fn == initFn) { already = true; break; }
        if (already) { *e.done = true; ++done; continue; }
        if (GT::RegisterPostObserver(initFn, GrabObserver_Aprop_Init_POST)) {
            g_registeredPropInitFns.push_back(initFn);
            *e.done = true; ++done;
            UE_LOGI("grab_hook[extra]: registered POST observer for %ls::Init @ %p "
                    "(late-load catch -- trash-ball sync)", e.cls, initFn);
        } else {
            // The observer table won't shrink, so retrying is futile -- mark done
            // to keep Install() converging to O(1). kMaxObservers is 256, so this
            // is a loud, unexpected WARN if it ever fires.
            UE_LOGW("grab_hook[extra]: RegisterPostObserver failed for %ls::Init (observer table full) -- skipping", e.cls);
            *e.done = true; ++done;
        }
    }
    if (done == kExtraCount) {
        g_extraKeyedInitDone = true;
        UE_LOGI("grab_hook[extra]: all %d late-load keyed classes resolved/handled (trash-ball + "
                "food/pinecone Init catch) -- O(1) hereafter", kExtraCount);
        return true;
    }
    // O(1)-safety bound (audit 2026-05-31): cap the retry so Install() reaches its
    // O(1) steady state even if a garbage class never loads this session (a map/area
    // with no chipPiles). These classes are hard-referenced by actorChipPile_C and
    // load with the world, so all 3 normally resolve within ~1-2 s; the ~2 min budget
    // (at the ~1 Hz call rate) covers any lazy load while GUARANTEEING the per-tick
    // FindClass walk terminates rather than running for the whole session.
    if (++sAttempts >= kMaxAttempts) {
        g_extraKeyedInitDone = true;
        UE_LOGW("grab_hook[extra]: gave up after %d attempts -- unresolved: %s%s%s%s; Init catch "
                "latched to keep Install() O(1) (re-arms next session)",
                sAttempts,
                sTrash ? "" : "trashBitsPile_C ",
                sClump ? "" : "prop_garbageClump_C ",
                sChip  ? "" : "actorChipPile_C ",
                sFood  ? "" : "prop_food_C ");
        return true;
    }
    return false;
}

}  // namespace

// Shared session cache (declared in prop_lifecycle_detail.h; the destroy seam
// in prop_destroy_seam.cpp reads it through LoadSession too).
std::atomic<coop::net::Session*> g_session_ptr{nullptr};

// ---- public API --------------------------------------------------------

// See prop_lifecycle.h. The sandbox Q-menu / toolgun spawn's own init() is
// dispatched EX_LocalVirtualFunction from its UCS (BP-internal) so it never
// fires our Aprop_C::Init POST observer; coop/host_spawn_watcher catches the
// spawn at FinishSpawningActor POST (where init has already minted the Key) and
// calls this to run the IDENTICAL keyed broadcast. Direct call (no GT::Post):
// the caller guarantees the game thread (FinishSpawningActor POST is GT). The
// shared HasProcessedInit latch dedupes vs an Init-POST that did fire.
void ExpressSpawnedProp(void* actor) {
    GrabObserver_Aprop_Init_POST_Body(actor);
}

// (SyncDestroyedTrackedProp: EXTRACTED to prop_destroy_seam.cpp.)

coop::element::ElementId RegisterHostPropSilent(void* actor) {
    // See prop_lifecycle.h. The MarkPropElement shadow alloc (host range) for a BP-internally-spawned
    // prop, MINUS the wire PropSpawn the Init-POST / ExpressSpawnedProp path sends -- the kerfur
    // conversion's only wire signal is KerfurConvert. Game thread (ProcessEvent-adjacent key read).
    if (!actor) return coop::element::kInvalidId;
    const std::wstring cls = R::ClassNameOf(actor);
    const std::wstring keyStr = ue_wrap::prop::GetInteractableKeyString(actor);
    if (keyStr.empty() || keyStr == L"None") {
        UE_LOGW("prop_lifecycle[silent register]: prop %p class '%ls' has no key -- cannot register (kerfur converge)",
                actor, cls.c_str());
        return coop::element::kInvalidId;
    }
    PT::MarkPropElement(actor, keyStr, cls);
    // R1-regression fix (2026-06-18, host turn-on/off kerfur dupe). ALSO mark it KNOWN.
    // Without this the converged kerfur prop is absent from g_knownKeyedProps, so the R1
    // steady-world re-seed's newness test (`g_knownKeyedProps.insert(obj).second`) sees it
    // as NEW every ~4s and ExpressIncrementalSpawn re-broadcasts it with its REAL BP key --
    // a 2nd PropSpawn conflicting with the kerfur's ONLY intended wire signal (KerfurConvert).
    // On a client fuzzy-miss (skin variant / race) that 2nd PropSpawn fresh-spawns a duplicate
    // kerfurOmega. Marking it known closes the echo at the source (the release path's
    // UnmarkKnownKeyedProp is symmetric). The kerfur prop still needs NO PropSpawn here.
    PT::MarkKnownKeyedProp(actor);
    const coop::element::ElementId eid = PT::GetPropElementIdForActor(actor);
    UE_LOGI("prop_lifecycle[silent register]: host prop %p class '%ls' key '%ls' -> eid=%u (no PropSpawn broadcast; marked known so the re-seed won't re-express it)",
            actor, cls.c_str(), keyStr.c_str(), static_cast<uint32_t>(eid));
    return eid;
}

void SetSession(coop::net::Session* session) {
    g_session_ptr.store(session, std::memory_order_release);
    // Mirror to the element tracker so its in-lock role read sees the same
    // session pointer (M-1 2026-05-29 extraction). Note: this is two stores
    // to two atomic pointers; an observer reading both during the
    // sub-microsecond window between them could see a torn pair. Benign in
    // practice because (1) observer registration happens AFTER Install
    // finishes its setup, and (2) SeedKnownKeyedProps runs INSIDE Install
    // after both stores -- no concurrent reader exists during the window.
    // If a future expansion adds concurrent readers, fold the tracker's
    // pointer into prop_lifecycle's via a forwarding accessor instead.
    PT::SetSession(session);
}

bool IsWireSuppressedPropClass(const std::wstring& cls) {
    // P2 design note (2026-06-10): the litter classes (chipPile/trashBits
    // Pile) deliberately do NOT go here. This predicate is SYMMETRIC across
    // its 3 call sites (client Init-destroy, host Init skip-broadcast,
    // snapshot enumerate-skip) -- adding trashBitsPile would drop the 392
    // level-PLACED deterministic-key piles from the connect snapshot (they
    // would then be unclaimed -> the adoption sweep would destroy every one
    // of them on the client), and adding chipPile would destroy a client's
    // own v52 ball->pile convert-born piles at Init (same mechanism that
    // vetoed garbageClump: clump Init fires on the client's legit grab
    // morph). Connect-time divergence is handled by claim-tracking instead
    // (remote_prop_spawn's deferred divergence sweep).
    //
    // Fork B 2e (2026-06-10): the adoption SWEEP intentionally does NOT
    // consult this predicate -- mushroom7 is keyed (in-universe) but never
    // expressed (enumerate-skip here) AND client-forbidden (the Init-destroy
    // above + the wire-ingress drop): its authoritative client steady state
    // is ZERO instances, so the sweep removing stragglers is parity, not a
    // hole.
    return cls == P::name::PropMushroomGrowingClass;
}

bool IsPerPlayerPropClass(const std::wstring& cls) {
    // PER-PLAYER state actors, NOT shared world props (2026-06-10, the
    // sweep-kills-inventory crash): each peer owns its own instance whose
    // key differs per save BY DESIGN, so it can never claim-bind. The host
    // must not snapshot-express it (a mirror of the host's personal
    // inventory container is wrong on every peer), no peer live-broadcasts
    // it, and the adoption sweep must never destroy the local one -- the
    // 2026-06-10 smoke swept the client's prop_inventoryContainer_player_C
    // as "unclaimed" and the client fataled at the next GC purge (engine
    // references into the player's own inventory). Distinct from
    // IsWireSuppressedPropClass: that predicate's client call site DESTROYS
    // local instances (client-forbidden intermediates); a per-player class
    // is the opposite -- the local instance is the player's own state and
    // must live untouched.
    return cls == P::name::PropInventoryContainerPlayerClass;
}

// Destroy a local prop via K2_DestroyActor, echo-suppressed (MarkIncomingDestroy
// BEFORE the call so OUR destroy seam skips the re-broadcast).
// See prop_lifecycle.h for the deferred-vs-immediate contract.
// (DestroyLocalProp: EXTRACTED to prop_destroy_seam.cpp.)

// GetPropElementIdForActor moved to coop::prop_element_tracker (M-1, 2026-05-29).
// SnapshotKnownKeyedProps retired 2026-05-29 (M-1, prior commit) -- zero callers
// since prop_snapshot migrated to element::Registry::SnapshotByType<Prop>.

// Enqueue*/DrainPending* functions retired 2026-05-27 -- the reliable
// channel buffers internally now (see reliable_channel.cpp). Callers just
// call Send* and always get true (unless payload-too-large / queue full
// at 4096 backlog).

void Install(coop::net::Session* session) {
    g_session_ptr.store(session, std::memory_order_release);
    PT::SetSession(session);  // mirror; see SetSession comment above.
    // Audit Fix 1 (2026-05-27): composite atomic latch. InstallGrabObservers
    // runs at 125 Hz; until ALL inner flags resolve, every tick was calling
    // R::FindClass (a full GUObjectArray walk with std::wstring alloc per
    // entry -- the exact bomb that hit the retired non_prop_entity_sync
    // path. Same fix pattern.
    static std::atomic<bool> g_allInstalled{false};
    if (g_allInstalled.load(std::memory_order_acquire)) return;
    if (!g_propInitScanDone) {
        // Gate: wait for prop_C base class to load.
        void* propBase = R::FindClass(P::name::PropClass);
        if (propBase) {
            // One-shot GUObjectArray scan for Init UFunctions in prop_C lineage.
            const std::wstring kInitName(P::name::PropInitFn);
            const int32_t n = R::NumObjects();
            int registered = 0;
            for (int32_t i = 0; i < n; ++i) {
                void* obj = R::ObjectAt(i);
                if (!obj) continue;
                if (R::ClassNameOf(obj) != L"Function") continue;
                if (R::ToString(R::NameOf(obj)) != kInitName) continue;
                void* owningCls = R::OuterOf(obj);
                // Cover Aprop_C lineage AND the non-Aprop "prop-shaped"
                // garbage/trash bases (chipPile/clump/trashBitsPile) via the
                // 2026-05-27 IsKeyedInteractable extension. Same Init UFunction
                // protocol on all of them.
                if (!ue_wrap::prop::IsClassKeyedInteractable(owningCls)) continue;
                bool already = false;
                for (void* fn : g_registeredPropInitFns) {
                    if (fn == obj) { already = true; break; }
                }
                if (already) continue;
                if (GT::RegisterPostObserver(obj, GrabObserver_Aprop_Init_POST)) {
                    const std::wstring owner = R::ToString(R::NameOf(owningCls));
                    UE_LOGI("grab_hook: registered POST observer for %ls::Init @ %p (subclass-aware)",
                            owner.c_str(), obj);
                    g_registeredPropInitFns.push_back(obj);
                    ++registered;
                } else {
                    const std::wstring owner = R::ToString(R::NameOf(owningCls));
                    UE_LOGW("grab_hook: failed to register Init observer for %ls (observer table full)",
                            owner.c_str());
                }
            }
            UE_LOGI("grab_hook: subclass-aware Init scan: %d new registrations (total %zu Init UFunctions hooked across prop_C lineage)",
                    registered, g_registeredPropInitFns.size());
            if (!g_registeredPropInitFns.empty()) {
                g_propInitScanDone = true;
                // H2-redux 2026-05-28: seed g_knownKeyedProps with every
                // live keyed-interactable currently in the world. Done
                // AFTER observer registration so any spawns racing the
                // seed scan are also captured by the Init POST observer
                // (duplicate inserts are no-ops on the set). Internally
                // latched -- safe to call again on subsequent Install()
                // ticks.
                PT::SeedKnownKeyedProps();
            }
        }
    }
    // Catch the lazily-loaded garbage/trash keyed classes the one-shot Aprop scan
    // above missed (chipPile/clump/trashBits -- trash-ball sync, 2026-05-31).
    // Gated on g_propInitScanDone so it only runs once world props are loading
    // (never churns FindClass at the menu); self-latches via g_extraKeyedInitDone.
    // Throttled to ~1 Hz: each unresolved class costs one FindClass (a full
    // GUObjectArray name walk). prop_garbageClump_C may not load until the first
    // chipPile pickup (potentially minutes in), so at 125 Hz an unthrottled poll
    // would burn a sustained ~6M wstring allocs/sec walk until then -- the throttle
    // caps it at ~3 walks/sec, trivial, while keeping <=1 s catch latency.
    if (g_propInitScanDone && !g_extraKeyedInitDone) {
        static int sExtraThrottle = 0;
        if ((sExtraThrottle++ % 125) == 0) RegisterExtraKeyedInitObservers();
    }
    if (!g_propDestroyObserverInstalled) {
        // v106 (2026-07-07): the destroy seam is a UFunction::Func patch, NOT a ProcessEvent
        // observer. The R-pickup destroy (putObjectInventory2 @719 InputPin.K2_DestroyActor())
        // and the pile/clump morph destroys are EX_CallMath-dispatched -- INVISIBLE to a PE
        // observer (log-proven 2026-07-07 10:15: pickup vanish waited the 4s reap while this
        // "continuous" observer never fired). Func funnels EVERY dispatch route (PE + EX_*),
        // so the Func patch strictly supersedes the PE PRE registration (RULE 2: replaced).
        // The callback receives the dying actor as the dispatch CONTEXT.
        if (void* actorCls = R::FindClass(P::name::ActorClassName)) {
            if (void* fn = R::FindFunction(actorCls, P::name::DestroyActorFn)) {
                if (ue_wrap::ufunction_hook::InstallPostHook(fn, &OnK2DestroyFunc)) {
                    UE_LOGI("grab_hook: Func-patched %ls.%ls @ %p (destroy seam -- catches "
                            "EX_CallMath destroys the old PE observer missed)",
                            P::name::ActorClassName, P::name::DestroyActorFn, fn);
                    g_propDestroyObserverInstalled = true;
                }
            } else {
                UE_LOGW("grab_hook: %ls.%ls UFunction not found -- destroy broadcast disabled",
                        P::name::ActorClassName, P::name::DestroyActorFn);
                g_propDestroyObserverInstalled = true;  // stop retry
            }
        }
    }
    // (v52: the clump re-grab dupe fix moved to trash_collect_sync's mirror-pile death-watch --
    // identity-exact, fires for whoever grabs the shared pile, not just the grabber's aim edge.
    // The old InpActEvt_use lookAtActor PRE observer here was retired; see PropConvert.)
    // InstallInventory has its own atomic guard + early-out; not gated here.
    // g_extraKeyedInitDone is part of the latch so Install keeps re-entering until
    // the lazily-loaded garbage Init observers are hooked (else trash-ball sync
    // silently never arms); once all latches are set, Install is an O(1) no-op.
    if (g_propInitScanDone && g_extraKeyedInitDone && g_propDestroyObserverInstalled) {
        g_allInstalled.store(true, std::memory_order_release);
        UE_LOGI("prop_lifecycle: Install() complete -- subsequent calls are O(1) no-ops");
    }
}

// (InstallInventory: EXTRACTED to prop_container_extract.cpp.)

DisconnectStats OnDisconnect() {
    DisconnectStats s;
    s.initProcessedDropped = PT::ClearProcessedInit();
    g_takeObjInFlight.store(false, std::memory_order_relaxed);
    return s;
}

}  // namespace coop::prop_lifecycle
