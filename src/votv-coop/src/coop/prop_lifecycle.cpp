// coop/prop_lifecycle.cpp -- Aprop_C spawn/destroy/extract wire observers.
//
// Extracted from harness/harness.cpp (2026-05-25 modular refactor).
// See coop/prop_lifecycle.h for the public interface.

#include "coop/prop_lifecycle.h"

#include "coop/element/element.h"
#include "coop/element/mirror_manager.h"  // SyncDestroyedTrackedProp reads the key off the element
#include "coop/element/prop.h"
#include "coop/net/session.h"
#include "coop/players_registry.h"
#include "coop/prop_echo_suppress.h"
#include "coop/prop_element_tracker.h"
#include "coop/prop_synth_key.h"
#include "coop/remote_prop.h"
#include "coop/remote_prop_spawn.h"
#include "ue_wrap/call.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/fname_utils.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"
#include "ue_wrap/prop.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"
#include "ue_wrap/types.h"

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
std::atomic<coop::net::Session*> g_session_ptr{nullptr};

inline coop::net::Session* LoadSession() {
    return g_session_ptr.load(std::memory_order_acquire);
}

// Install idempotency.
bool g_propInitScanDone = false;
bool g_propDestroyObserverInstalled = false;
bool g_inventoryObserverInstalled = false;
// Late-load catch for the non-Aprop_C keyed garbage classes (chipPile/clump/
// trashBits). The one-shot Aprop-lineage Init scan below latches g_propInitScanDone
// on the FIRST keyed Init found (Aprop_C, which loads with the world), so garbage
// BP classes that load a moment later are missed -> they never broadcast PropSpawn
// -> trash-ball interaction never syncs (root-caused 2026-05-31). This latches
// once all three garbage Init UFunctions are hooked; see RegisterExtraKeyedInitObservers.
bool g_extraKeyedInitDone = false;
std::vector<void*> g_registeredPropInitFns;

// Storage-container spawn fix (2026-05-25 RE): bracketed by takeObj PRE
// (set) and takeObj POST (clear) so the nested Aprop_C::Init POST observer
// defers its broadcast until loadData has restored the saved Key.
//
// Audit H12 (2026-05-27): std::atomic<bool> (was plain bool). The PRE/POST
// observers and the nested Init POST observer all run from parallel-anim
// worker threads per game_thread.cpp's header comment; plain-bool writes
// in the PRE + reads in the Init POST race per the C++ memory model. The
// PRE->POST sequencing within a single ProcessEvent dispatch is preserved
// by the same-thread execution order; relaxed memory order is sufficient
// (no other state depends on the bool's visibility ordering).
std::atomic<bool> g_takeObjInFlight{false};

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
void GrabObserver_Actor_K2DestroyActor_PRE(void* self, void* function, void* params);
void GrabObserver_PropInventory_TakeObj_PRE(void* self, void* function, void* params);
void GrabObserver_PropInventory_TakeObj_POST(void* self, void* function, void* params);

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
    PT::MarkPropElement(self, keyStr, cls);
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
    s->SendPropSpawn(p);
    // Fork B 2c: self-claim -- this peer just wire-expressed the spawn; an
    // open snapshot bracket's sweep must not destroy it as "unclaimed".
    coop::remote_prop_spawn::RecordClaimIfTracking(self);
}

// K2_DestroyActor PRE -- bidirectional destroy broadcast (host + client),
// echo-suppressed via the remote_prop incoming-destroy set.
void GrabObserver_Actor_K2DestroyActor_PRE(void* self, void* /*function*/, void* /*params*/) {
    auto* s = LoadSession();
    if (!self || !s) return;
    // K2_DestroyActor PRE fires for EVERY actor destroy in the world.
    // We CANNOT promote IsKeyedInteractable to a fast-path gate here:
    // ue_wrap::prop::IsKeyedInteractable internally calls ResolveExtraBases
    // which does R::FindClass walks for trashBitsPile_C / prop_garbageClump_C /
    // actorChipPile_C until all three resolve. During the pre-resolution
    // window (early boot, widget/UI teardown phase), every non-prop_C
    // actor destroy would burn multiple GUObjectArray walks with wstring
    // allocations -- the documented install-loop bomb pattern (see
    // [[feedback-install-idempotent-o1-steady-state]]). The session-null
    // and not-connected gates here are what historically prevented the
    // bomb from firing during the unresolved-classes window. Keep them
    // first. (Audited + smoke-FAILED + reverted 2026-05-28.)
    // Capture the Prop Element id BEFORE UnmarkKnownKeyedProp drains the
    // shadow (audit fix 2026-05-28 -- the prior order returned kInvalidId
    // on every destroy broadcast).
    const coop::element::ElementId destroyEid = PT::GetPropElementIdForActor(self);
    PT::UnmarkProcessedInit(self);
    PT::UnmarkKnownKeyedProp(self);
    if (!s->connected()) return;
    if (coop::prop_echo_suppress::ConsumeIncomingDestroy(self)) {
        UE_LOGI("grab_hook[K2_DestroyActor PRE]: actor %p was wire-received destroy -- skip rebroadcast",
                self);
        return;
    }
    if (!ue_wrap::prop::IsKeyedInteractable(self)) return;
    const std::wstring keyStr = ue_wrap::prop::GetInteractableKeyString(self);
    // FName(NAME_None) stringifies to "None" -- a KEYED prop broadcasts by Key (the
    // common path). The NON-KEYABLE trash clump (prop_garbageClump_C: setKey doesn't
    // stick, key always reads None) instead rides OUR eid: broadcast key=None + eid so
    // the receiver's eid-routable OnDestroy despawns its mirror (v26 spawn-by-eid
    // symmetry). WITHOUT this the clump's morph-destroy (toClump/turnToPile call
    // K2_DestroyActor -- IDA-confirmed, votv-chippile-clump-morph-RE-2026-05-27.md) was
    // dropped here -> the mirror leaked -> the infinite grab/throw dupe. Only drop when
    // there is NEITHER a Key NOR an eid (a genuinely unsyncable actor).
    // [[project-bug-trash-chippile-uaf-crash]]
    const bool keyless = (keyStr.empty() || keyStr == L"None");
    const bool hasEid  = (destroyEid != coop::element::kInvalidId);
    if (keyless && !hasEid) return;
    coop::net::WireKey wk{};
    wk.len = 0;
    if (!keyless) {
        for (size_t i = 0; i < keyStr.size() && i < 31; ++i) {
            wk.data[wk.len++] = static_cast<char>(keyStr[i]);
        }
    }
    const char* roleStr =
        s->role() == coop::net::Role::Host ? "HOST" : "CLIENT";
    // v12 (2026-05-28): construct PropDestroyPayload with both wire key
    // (existing receiver lookup path) and elementId (forward-compat for
    // event_feed routing-by-elementId). Lookup is best-effort: actor may
    // have been Unmark'd already by the time we get here (parallel-anim
    // race), in which case elementId is kInvalidId (0xFFFFFFFF on the
    // wire -- distinct from 0 = no Element ever assigned).
    coop::net::PropDestroyPayload dp{};
    dp.key = wk;
    // Translate kInvalidId (C++ sentinel) → 0 (wire sentinel) per the
    // protocol.h contract that "elementId == 0 → sender had no Element".
    dp.elementId = (destroyEid == coop::element::kInvalidId) ? 0u : destroyEid;
    // (v15 stamped a senderContext byte here; v16 PR-FOUNDATION-1b
    // moved stale-gen defense to the header senderEpoch.)
    UE_LOGI("grab_hook[K2_DestroyActor PRE]: %s broadcasting DESTROY actor=%p key='%ls' eid=%u%s",
            roleStr, self, keyless ? L"None" : keyStr.c_str(), dp.elementId,
            keyless ? " (eid-only: trash clump)" : "");
    s->SendPropDestroy(dp);  // channel queues internally; always accepted
}

// PRE observer for propInventory_C::takeObj -- sets g_takeObjInFlight so
// the nested Aprop_C::Init POST observer defers its broadcast (Key is
// NewGuid pre-loadData).
void GrabObserver_PropInventory_TakeObj_PRE(void* self, void* /*function*/, void* /*params*/) {
    auto* s = LoadSession();
    if (!self || !s) return;
    if (!s->connected()) return;
    g_takeObjInFlight.store(true, std::memory_order_relaxed);
}

// POST observer for propInventory_C::takeObj -- the canonical broadcaster
// for container extracts (after loadData restored the saved Key).
void GrabObserver_PropInventory_TakeObj_POST(void* self, void* function, void* params) {
    // Clear the in-flight flag FIRST regardless of any early returns.
    g_takeObjInFlight.store(false, std::memory_order_relaxed);

    auto* s = LoadSession();
    if (!self || !params || !function || !s) return;
    // Cache the Object out-param offset. Atomic because the observer can
    // dispatch on either the game thread (typical) or a task-graph worker.
    static std::atomic<int32_t> sObjectOff{-2};
    int32_t off = sObjectOff.load(std::memory_order_acquire);
    if (off == -2) {
        const int32_t resolved = R::FindParamOffset(function, L"Object");
        sObjectOff.store(resolved >= 0 ? resolved : -1, std::memory_order_release);
        off = resolved >= 0 ? resolved : -1;
        UE_LOGI("grab_hook[takeObj POST]: resolved Object out-param offset = %d", resolved);
    }
    if (off < 0) return;
    void* spawnedActor = *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(params) + off);
    if (!spawnedActor || !R::IsLive(spawnedActor)) return;
    if (!s->connected()) {
        UE_LOGI("grab_hook[takeObj POST]: spawned %p but session not connected -- skipping broadcast",
                spawnedActor);
        return;
    }
    if (!ue_wrap::prop::IsKeyedInteractable(spawnedActor)) {
        UE_LOGW("grab_hook[takeObj POST]: spawned actor %p is NOT a keyed-interactable -- skipping",
                spawnedActor);
        return;
    }

    coop::net::PropSpawnPayload p{};
    const std::wstring cls = R::ClassNameOf(spawnedActor);
    p.className.len = 0;
    for (size_t i = 0; i < cls.size() && i < 63; ++i) {
        p.className.data[p.className.len++] = static_cast<char>(cls[i]);
    }
    // Audit defensive close 2026-05-29 (M-1 extraction post-ship): mirror
    // the Init POST EnsureKeyForBroadcast call here so a non-Aprop_C
    // keyed-interactable extracted from an inventory container also gets
    // a synthetic Key (no-op for Aprop_C lineage whose Key is already set
    // via loadData; just returns currentKey unchanged). Symmetric with
    // the Init POST path; zero cost in the typical Aprop_C inventory drop.
    std::wstring keyStr = ue_wrap::prop::GetInteractableKeyString(spawnedActor);
    keyStr = coop::prop_synth_key::EnsureKeyForBroadcast(spawnedActor, keyStr);
    p.key.len = 0;
    for (size_t i = 0; i < keyStr.size() && i < 31; ++i) {
        p.key.data[p.key.len++] = static_cast<char>(keyStr[i]);
    }
    const auto loc = ue_wrap::engine::GetActorLocation(spawnedActor);
    const auto rot = ue_wrap::engine::GetActorRotation(spawnedActor);
    p.locX = loc.X; p.locY = loc.Y; p.locZ = loc.Z;
    p.rotPitch = ue_wrap::NormalizeAxis(rot.Pitch);
    p.rotYaw   = ue_wrap::NormalizeAxis(rot.Yaw);
    p.rotRoll  = ue_wrap::NormalizeAxis(rot.Roll);
    // v54: real scale + identity row + SP-parity bools (same stamp as the
    // Init POST site; a container extraction is a fresh simulating spawn).
    // The flag reads are Aprop_C-gated -- pre-v54 this read the heavy/frozen
    // offsets on non-Aprop_C lineages too, i.e. stray bytes.
    const auto scl = ue_wrap::engine::GetActorScale3D(spawnedActor);
    p.scaleX = scl.X; p.scaleY = scl.Y; p.scaleZ = scl.Z;
    p.physFlags = coop::net::propspawn_flags::kSimulatePhysics;
    p.propName.len = 0;
    if (ue_wrap::prop::IsDescendantOfProp(spawnedActor)) {
        if (ue_wrap::prop::IsHeavy(spawnedActor))   p.physFlags |= coop::net::propspawn_flags::kIsHeavy;
        if (ue_wrap::prop::IsFrozen(spawnedActor))  p.physFlags |= coop::net::propspawn_flags::kFrozen;
        if (ue_wrap::prop::IsStatic(spawnedActor))  p.physFlags |= coop::net::propspawn_flags::kStatic;
        if (ue_wrap::prop::IsSleeping(spawnedActor)) p.physFlags |= coop::net::propspawn_flags::kSleep;
        if (ue_wrap::prop::ReadRemoveWOrespawn(spawnedActor)) {
            p.physFlags |= coop::net::propspawn_flags::kRemoveWOrespawn;
        }
        const std::wstring nm = ue_wrap::prop::GetPropNameString(spawnedActor);
        for (size_t i = 0; i < nm.size() && i < 31; ++i) {
            p.propName.data[p.propName.len++] = static_cast<char>(nm[i]);
        }
    }
    p.initLinVelX = p.initLinVelY = p.initLinVelZ = 0.f;
    p.initAngVelX = p.initAngVelY = p.initAngVelZ = 0.f;

    // Init POST returned early (g_takeObjInFlight) so MarkPropElement wasn't
    // called from that path. Mint the Prop Element here so this container-
    // extracted actor has a Registry shadow (audit fix 2026-05-28).
    PT::MarkPropElement(spawnedActor, keyStr, cls);
    {
        const coop::element::ElementId eid = PT::GetPropElementIdForActor(spawnedActor);
        p.elementId = (eid == coop::element::kInvalidId) ? 0u : eid;
        // (v15 stamped a senderContext byte here; v16 PR-FOUNDATION-1b
        // moved stale-gen defense to the header senderEpoch.)
    }
    UE_LOGI("grab_hook[takeObj POST]: SPAWN broadcast cls='%ls' key='%ls' loc=(%.1f, %.1f, %.1f) heavy=%d frozen=%d eid=%u",
            cls.c_str(), keyStr.c_str(), p.locX, p.locY, p.locZ,
            (p.physFlags & coop::net::propspawn_flags::kIsHeavy)  ? 1 : 0,
            (p.physFlags & coop::net::propspawn_flags::kFrozen)   ? 1 : 0,
            p.elementId);
    s->SendPropSpawn(p);  // channel queues internally
    // Fork B 2c: self-claim (see the Init POST site).
    coop::remote_prop_spawn::RecordClaimIfTracking(spawnedActor);
}

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

void SyncDestroyedTrackedProp(void* actorKey, coop::element::ElementId eid) {
    // See prop_lifecycle.h. Contract: NEVER dereference `actorKey` (the caller
    // may hold a PendingKill or GC-purged pointer; v67 kerfur_convert calls
    // this a tick after the BP-internal destroy). The wire key comes from the
    // ELEMENT; the pointer is only the tracker maps' key.
    if (!actorKey || eid == coop::element::kInvalidId) return;
    std::string key8;
    {
        auto* el =
            coop::element::MirrorManager<coop::element::Prop>::Instance().Get(eid);
        if (!el) return;  // already drained -- double call / raced the real PRE
        key8 = el->GetName();
    }
    auto* s = LoadSession();
    if (s && s->connected()) {
        coop::net::PropDestroyPayload dp{};
        dp.key.len = 0;
        for (size_t i = 0; i < key8.size() && i < 31; ++i) {
            dp.key.data[dp.key.len++] = key8[i];
        }
        dp.elementId = eid;  // eid rides along (keyless elements route by it)
        UE_LOGI("prop_lifecycle[explicit destroy]: broadcasting DESTROY key='%s' eid=%u (BP-internal K2_DestroyActor -- v67 converge)",
                key8.c_str(), static_cast<uint32_t>(eid));
        s->SendPropDestroy(dp);
    }
    // Same teardown the organic K2_DestroyActor PRE runs: processed-Init latch
    // out, then UnmarkKnownKeyedProp (key index + reverse map + Element drain
    // via ElementDeleter). Both are pointer-as-map-key only.
    PT::UnmarkProcessedInit(actorKey);
    PT::UnmarkKnownKeyedProp(actorKey);
}

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
// BEFORE the call so OUR K2_DestroyActor PRE observer skips the re-broadcast).
// See prop_lifecycle.h for the deferred-vs-immediate contract.
void DestroyLocalProp(void* actor, bool deferred) {
    if (!actor) return;
    auto doDestroy = [actor]() {
        static void* sActorCls = nullptr;
        static void* sDestroyFn = nullptr;
        if (!sActorCls || !R::IsLive(sActorCls)) {
            sActorCls = R::FindClass(P::name::ActorClassName);
            sDestroyFn = nullptr;
        }
        if (sActorCls && !sDestroyFn) {
            sDestroyFn = R::FindFunction(sActorCls, P::name::DestroyActorFn);
        }
        if (!sDestroyFn) {
            UE_LOGW("spawner-suppress: K2_DestroyActor UFunction unresolved -- cannot destroy local %p", actor);
            return;
        }
        if (!R::IsLive(actor)) {
            UE_LOGI("spawner-suppress: deferred destroy target %p no longer live (already destroyed elsewhere) -- skip",
                    actor);
            return;
        }
        // Mark BEFORE calling destroy so OUR PRE-observer skips broadcast.
        coop::prop_echo_suppress::MarkIncomingDestroy(actor);
        R::CallFunction(actor, sDestroyFn, nullptr);
    };
    if (deferred) {
        GT::Post(doDestroy);
    } else {
        doDestroy();
    }
}

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
        if (void* actorCls = R::FindClass(P::name::ActorClassName)) {
            if (void* fn = R::FindFunction(actorCls, P::name::DestroyActorFn)) {
                if (GT::RegisterPreObserver(fn, GrabObserver_Actor_K2DestroyActor_PRE)) {
                    UE_LOGI("grab_hook: registered PRE observer for %ls.%ls @ %p (continuous destroy broadcast)",
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

void InstallInventory(coop::net::Session* session) {
    g_session_ptr.store(session, std::memory_order_release);
    PT::SetSession(session);  // mirror; see SetSession comment above.
    // Audit Fix 2 (2026-05-27): atomic latch matches Install()'s. Without
    // this, the 125 Hz pump's InstallGrabObservers call runs R::FindClass
    // (a full GUObjectArray walk with std::wstring alloc per entry) on
    // every tick until propInventory_C loads. The plain-bool guard below
    // is read/written only from the GT, but it doesn't short-circuit the
    // GUObjectArray walk if propInventory_C is slow to load.
    static std::atomic<bool> s_done{false};
    if (s_done.load(std::memory_order_acquire)) return;
    if (g_inventoryObserverInstalled) {
        s_done.store(true, std::memory_order_release);
        return;
    }
    void* invCls = R::FindClass(P::name::PropInventoryClass);
    if (!invCls) return;  // not loaded yet -- retry next tick
    void* fn = R::FindFunction(invCls, P::name::PropInventoryTakeObjFn);
    if (!fn) {
        UE_LOGW("grab_hook: %ls.%ls UFunction not found -- Bug C disabled permanently this session",
                P::name::PropInventoryClass, P::name::PropInventoryTakeObjFn);
        g_inventoryObserverInstalled = true;  // stop the retry loop
        s_done.store(true, std::memory_order_release);
        return;
    }
    if (!GT::RegisterPostObserver(fn, GrabObserver_PropInventory_TakeObj_POST)) {
        UE_LOGW("grab_hook: failed to register takeObj POST observer (table full?)");
        return;
    }
    if (!GT::RegisterPreObserver(fn, GrabObserver_PropInventory_TakeObj_PRE)) {
        UE_LOGW("grab_hook: failed to register takeObj PRE observer (table full?) -- container extracts may double-broadcast with mismatched keys");
    } else {
        UE_LOGI("grab_hook: registered PRE observer for %ls.%ls (takeObj-in-flight bracket)",
                P::name::PropInventoryClass, P::name::PropInventoryTakeObjFn);
    }
    UE_LOGI("grab_hook: registered POST observer for %ls.%ls @ %p (Bug C inventory drop ready)",
            P::name::PropInventoryClass, P::name::PropInventoryTakeObjFn, fn);
    g_inventoryObserverInstalled = true;
    s_done.store(true, std::memory_order_release);
}

DisconnectStats OnDisconnect() {
    DisconnectStats s;
    s.initProcessedDropped = PT::ClearProcessedInit();
    g_takeObjInFlight.store(false, std::memory_order_relaxed);
    return s;
}

}  // namespace coop::prop_lifecycle
