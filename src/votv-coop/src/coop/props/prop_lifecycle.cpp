// coop/props/prop_lifecycle.cpp -- the Aprop_C spawn observers: the Init POST observer that
// expresses a keyed prop's birth on the wire, the late-load catch for the trash and food classes,
// the install of the destroy seam, and the class predicates. The destroy seam body lives in
// prop_destroy_seam.cpp and the container extract in prop_container_extract.cpp.

#include "coop/props/prop_lifecycle.h"

#include "prop_lifecycle_detail.h"  // co-located private header (src tree, not include/)

#include "coop/element/element.h"
#include "coop/element/prop.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/creatures/kerfur_convert.h"  // TryAdoptFreshKerfurProp, the conversion successor's converge
#include "coop/creatures/kerfur_entity.h"   // IsKerfurPropClass
#include "coop/creatures/kerfur_form_assembler.h"  // IsCapturedForm, the conversion-successor peek
#include "coop/props/prop_echo_suppress.h"
#include "coop/props/prop_element_tracker.h"
#include "coop/props/prop_synth_key.h"
#include "coop/props/remote_prop.h"
#include "coop/props/remote_prop_spawn.h"
#include "coop/props/join_membership_sweep.h"  // the join claim
#include "coop/session/world_load_episode.h"     // the client world-load window
#include "coop/props/prop_drop_intent.h"       // a client keyed destroy parked for the host's re-place
#include "ue_wrap/core/call.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/core/ufunction_hook.h"  // the destroy seam's Func patch on K2_DestroyActor

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

// The session pointer observers read role() and Send*() through: an atomic, since observers fire
// from parallel-anim worker threads while the harness may SetSession(nullptr) at shutdown.
// Defined at namespace scope below, shared with prop_destroy_seam.cpp through
// prop_lifecycle_detail.h.

// Install idempotency.
bool g_propInitScanDone = false;
bool g_propDestroyObserverInstalled = false;
// The late-load catch: the one-shot Init scan latches on the first keyed Init found (Aprop_C
// loads with the world), and the garbage classes load a moment later, so their Init observers
// are registered by RegisterExtraKeyedInitObservers; this latches once all are hooked.
bool g_extraKeyedInitDone = false;
std::vector<void*> g_registeredPropInitFns;

// The takeObj-in-flight bracket is defined in prop_container_extract.cpp and shared through
// prop_lifecycle_detail.h; the Init POST body reads it, OnDisconnect clears it.

// Forward declarations for observer callbacks.
void GrabObserver_Aprop_Init_POST(void* self, void* function, void* params);

// Forward declaration so the game-thread-defer wrapper can refer to the body.
void GrabObserver_Aprop_Init_POST_Body(void* self);

void GrabObserver_Aprop_Init_POST(void* self, void* /*function*/, void* /*params*/) {
    auto* s = LoadSession();
    if (!self || !s) return;
    // The body calls UFunctions (GetActorLocation, GetKey), which are game-thread only, and the
    // observer can fire on a parallel-anim worker; off-thread it is posted to the game thread,
    // where the body re-validates the actor with IsLive.
    if (!GT::IsGameThread()) {
        GT::Post([self] { GrabObserver_Aprop_Init_POST_Body(self); });
        return;
    }
    GrabObserver_Aprop_Init_POST_Body(self);
}

void GrabObserver_Aprop_Init_POST_Body(void* self) {
    if (!self) return;
    // The known-keyed set is maintained before the session and echo gates, so it is warm by the
    // time a peer joins (it fills during the pre-handshake save load); the two reflection probes
    // per Init are bursty (level load), not steady-state.
    if (!R::IsLive(self)) return;
    if (!ue_wrap::prop::IsKeyedInteractable(self)) return;
    // IsLive does not filter CDOs (persistent objects): a CDO whose Init fires would enter the
    // known set permanently, since a CDO is never destroyed; the seed scan carries the same
    // Default__ guard.
    {
        const std::wstring nm = R::ToString(R::NameOf(self));
        if (nm.rfind(L"Default__", 0) == 0) return;
    }
    // A child actor (a kerfur's eye camera) neither enters the known set nor broadcasts: the
    // parent's construction script re-creates it on every peer, and a broadcast minted a floating
    // camera mirror on the joiner at every kerfur toggle.
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

    // An actor spawned inside a takeObj call is broadcast by the takeObj POST, which sees the saved
    // Key restored by loadData.
    if (g_takeObjInFlight.load(std::memory_order_relaxed)) {
        UE_LOGI("grab_hook[Aprop.Init POST]: actor %p spawned inside takeObj -- defer to takeObj POST (Key not yet restored by loadData)",
                self);
        return;
    }

    // Host-authoritative intermediate variants: the client destroys its local one, and the mature
    // variant arrives on the wire.
    const std::wstring cls = R::ClassNameOf(self);
    if (s->role() == coop::net::Role::Client) {
        if (IsWireSuppressedPropClass(cls)) {
            UE_LOGI("spawner-suppress[client.Init]: scheduling deferred destroy for local intermediate variant '%ls' actor=%p (host-authoritative; mature variant will arrive via wire)",
                    cls.c_str(), self);
            DestroyLocalProp(self, /*deferred=*/true);
            return;
        }
        // The Aprop_C lineage stays host-authoritative (save-persisted; a client's save load is its
        // own copy). The non-Aprop_C interactables (chipPile, clump, trashBitsPile: transient
        // litter with no save lineage) are the client's own creations (the toClump morph) and must
        // reach the host, so they fall through to the broadcast.
        if (ue_wrap::prop::IsDescendantOfProp(self)) {
            // A client-originated Aprop_C world spawn (a drop or a place: simulateDrop,
            // FinishSpawningActor, a fresh actor with its save Key restored by loadData) is skipped
            // by the host-authoritative rule, so the host does not learn of a client-placed prop
            // until an E-grab expresses it; logged with key and eid so a repro correlates with the
            // host log.
            const coop::element::ElementId dropEid = PT::GetPropElementIdForActor(self);
            const ue_wrap::FVector dloc = ue_wrap::engine::GetActorLocation(self);
            UE_LOGI("[ROCK-DROP] CLIENT Aprop spawn NOT authored (host-auth skip): cls='%ls' key='%ls' "
                    "eid=%u loc=(%.1f,%.1f,%.1f) -- host will NOT see this client-placed prop",
                    cls.c_str(), ue_wrap::prop::GetInteractableKeyString(self).c_str(),
                    (dropEid == coop::element::kInvalidId) ? 0u : static_cast<unsigned>(dropEid),
                    dloc.X, dloc.Y, dloc.Z);
            return;  // Aprop_C: host-authoritative world spawn, skip client broadcast
        }
        // Non-Aprop_C interactable: fall through to the broadcast.
    }

    // The host (always) and a client (non-Aprop_C only) reach here.
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
    // A kerfur conversion successor: the kerfur layer owns kerfur-form expression (one entity;
    // KerfurConvert is the sole conversion wire signal, never a generic PropSpawn), and this body
    // is the express funnel for both the Init POST observer and ExpressSpawnedProp. The
    // deterministic test is whether the form assembler captured this prop in its bracket (a
    // non-consuming peek), which covers both the destroy-first and the spawn-first orderings; a
    // proximity test against a dead NPC raced the verb's order. An ordinary kerfur prop spawn keeps
    // the generic expression.
    const bool kerfurConvSuccessor =
        coop::kerfur_entity::IsKerfurPropClass(R::ClassOf(self)) &&
        coop::kerfur_form_assembler::IsCapturedForm(self);
    if (kerfurConvSuccessor) {
        // Converge synchronously if the source NPC already died; otherwise a no-op, and the
        // death-watch poll converges later through the same captured form. Gating it on the capture
        // removes the false positive of a hand-placed kerfur near a dead one. No early return:
        // MarkPropElement follows.
        coop::kerfur_convert::TryAdoptFreshKerfurProp(self);
    }

    coop::net::PropSpawnPayload p{};
    p.className.len = 0;
    for (size_t i = 0; i < cls.size() && i < 63; ++i) {
        p.className.data[p.className.len++] = static_cast<char>(cls[i]);
    }
    std::wstring keyStr = ue_wrap::prop::GetInteractableKeyString(self);
    // A non-Aprop_C interactable whose BP mints no Key gets a synthetic one before the None guard
    // (clump.GetKey returns NAME_None on a fresh spawn, and the guard would drop every chipPile
    // morph).
    keyStr = coop::prop_synth_key::EnsureKeyForBroadcast(self, keyStr);
    // FName(NAME_None) stringifies to "None": unkeyed. An Aprop_C mints on a later Init pass after
    // loadData or setKey; a non-Aprop_C was just minted, so None here means setKey failed.
    if (keyStr.empty() || keyStr == L"None") {
        UE_LOGI("grab_hook[Aprop.Init POST]: actor %p (class '%ls') has unset key '%ls' -- skip (unkeyed = non-syncable)",
                self, cls.c_str(), keyStr.c_str());
        return;
    }
    // Create the Prop Element at the broadcast site, where the Key and class are resolved
    // (idempotent against the seed-scan creation). Mark may re-key a duplicate (the host is the key
    // authority), so the payload carries the enrolled key.
    keyStr = PT::MarkPropElement(self, keyStr, cls, PT::EnrollSource::kExpressSeam);
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
    // The real scale, the identity row and the parity bools: a fresh gameplay spawn is simulating,
    // and the parity bits let the mirror's init() resolve the true mesh, mass and collision, not
    // the CDO cube.
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
    // elementId from the Prop Element; kInvalidId travels as 0 (the wire sentinel).
    {
        const coop::element::ElementId eid = PT::GetPropElementIdForActor(self);
        p.elementId = (eid == coop::element::kInvalidId) ? 0u : eid;
    }
    // A kerfur conversion successor is tracked above (so the other broadcasters defer) but not
    // broadcast: KerfurConvert is its only wire signal. Only the SendPropSpawn is skipped; the
    // self-claim below is unconditional, since it keys on tracking.
    if (kerfurConvSuccessor) {
        UE_LOGI("grab_hook[Aprop.Init POST]: kerfur conversion successor %p key='%ls' -- generic "
                "PropSpawn SUPPRESSED (KerfurConvert is the sole express; tracked so the other "
                "broadcasters defer)", self, keyStr.c_str());
    } else {
        // Logged only where the broadcast happens, so a suppressed conversion never prints
        // "broadcasting".
        UE_LOGI("grab_hook[Aprop.Init POST]: HOST broadcasting SPAWN cls='%ls' key='%ls' loc=(%.1f,%.1f,%.1f) heavy=%d frozen=%d",
                cls.c_str(), keyStr.c_str(), p.locX, p.locY, p.locZ,
                (p.physFlags & coop::net::propspawn_flags::kIsHeavy)  ? 1 : 0,
                (p.physFlags & coop::net::propspawn_flags::kFrozen)   ? 1 : 0);
        s->SendPropSpawn(p);
        // NO save record here. This seam is the prop's own init(), which runs BEFORE the leaf
        // class's loadData has filled anything on a save load, and on a fresh spawn there is
        // nothing to carry yet -- the record would be the class default, and broadcasting a
        // default over an author's real copy is how a client's ejected disc came back blank. The
        // record's publishers are the seams where the state exists: the container extract POST,
        // the snapshot drain, the runtime-adoption express and the client's own drop intent.
    }
    // Self-claim: this peer just expressed the spawn, so an open snapshot bracket's sweep must not
    // destroy it as unclaimed.
    coop::join_membership_sweep::RecordClaimIfTracking(self);
}

// The destroy seam (prop_destroy_seam.cpp): a UFunction::Func patch on Actor.K2_DestroyActor,
// which fires for every dispatch route including the EX_CallMath destroys a ProcessEvent
// observer never sees (the R-pickup destroy, the pile and clump morphs). Post-native:
// K2_DestroyActor only marks PendingKill, so reads on the actor are still valid.

// The late-load catch for the keyed litter classes (actorChipPile_C, prop_garbageClump_C,
// trashBitsPile_C), which load lazily on first encounter, after the one-shot Init scan has
// latched, so a fresh clump would never broadcast and trash-ball carry would not sync. Each class
// is resolved by name and its own Init hooked, with a per-class latch and an overall latch, so
// the work stops once all are hooked: at most one FindClass and FindFunction per class per call,
// never a per-tick array rescan. FindFunction returns the Init the class owns, matching the
// scan's owning-class filter; a class that only inherits Init is covered by its base. Game thread.
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
        // prop_food_C owns an Init override and loads late, so every food leaf with no Init of its
        // own (the pinecone scare among them) dispatched an unhooked Init and reached the client
        // only via the snapshot, 30 s late and at rest.
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
            // Loaded but without an Init of its own: it dispatches a base Init that is already
            // covered.
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
            // The observer table will not shrink, so retrying is futile; a loud, unexpected WARN.
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
    // The retry is capped so Install() reaches its O(1) steady state even if a class never loads
    // this session (an area with no chipPiles); the classes normally resolve within seconds, and
    // the ~2 min budget covers a lazy load.
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

// The shared session cache (prop_lifecycle_detail.h); the destroy seam reads it too.
std::atomic<coop::net::Session*> g_session_ptr{nullptr};

// ---- public API ----

// The sandbox spawn menu's and the toolgun's own init() is dispatched EX_LocalVirtualFunction
// from the construction script, invisible to the Init POST observer, so host_spawn_watcher
// catches the spawn at FinishSpawningActor POST (the Key already minted) and calls this for the
// same keyed broadcast. Game thread (the caller guarantees it); the HasProcessedInit latch
// dedupes against an Init POST that did fire.
void ExpressSpawnedProp(void* actor) {
    GrabObserver_Aprop_Init_POST_Body(actor);
}

coop::element::ElementId RegisterHostPropSilent(void* actor) {
    // The Prop Element for a BP-internally spawned prop, minus the wire PropSpawn: the kerfur
    // conversion's only wire signal is KerfurConvert. Game thread.
    if (!actor) return coop::element::kInvalidId;
    const std::wstring cls = R::ClassNameOf(actor);
    const std::wstring keyStr = ue_wrap::prop::GetInteractableKeyString(actor);
    if (keyStr.empty() || keyStr == L"None") {
        UE_LOGW("prop_lifecycle[silent register]: prop %p class '%ls' has no key -- cannot register (kerfur converge)",
                actor, cls.c_str());
        return coop::element::kInvalidId;
    }
    PT::MarkPropElement(actor, keyStr, cls, PT::EnrollSource::kExpressSeam);
    // Also mark it known: absent from the known set, the steady-world re-seed's newness test would
    // re-express the converged kerfur prop every few seconds with its real BP key, a second
    // PropSpawn beside KerfurConvert, and a client fuzzy miss would spawn a duplicate. The release
    // path's Unmark is symmetric.
    PT::MarkKnownKeyedProp(actor);
    const coop::element::ElementId eid = PT::GetPropElementIdForActor(actor);
    UE_LOGI("prop_lifecycle[silent register]: host prop %p class '%ls' key '%ls' -> eid=%u (no PropSpawn broadcast; marked known so the re-seed won't re-express it)",
            actor, cls.c_str(), keyStr.c_str(), static_cast<uint32_t>(eid));
    return eid;
}

void SetSession(coop::net::Session* session) {
    g_session_ptr.store(session, std::memory_order_release);
    // Mirrored to the element tracker for its in-lock role read. Two stores to two atomics; no
    // reader exists in the window (observers register after Install's setup, and the seed runs
    // inside Install after both stores).
    PT::SetSession(session);
}

bool IsWireSuppressedPropClass(const std::wstring& cls) {
    // The litter classes are deliberately not here. The predicate is symmetric across its three
    // call sites (the client Init destroy, the host skip, the snapshot enumerate skip):
    // trashBitsPile would drop the 392 level-placed piles from the connect snapshot and the
    // adoption sweep would destroy every one on the client, and chipPile would destroy a client's
    // own convert-born piles at Init. Connect-time divergence is handled by claim tracking instead.
    // The adoption sweep does not consult this predicate: the growing mushroom is keyed but never
    // expressed and client-forbidden, so zero client instances is parity.
    return cls == P::name::PropMushroomGrowingClass;
}

bool IsPerPlayerPropClass(const std::wstring& cls) {
    // Per-player state actors, not shared world props: each peer owns its own instance, keyed per
    // save by design, so it can never claim-bind. The host must not snapshot-express it, no peer
    // broadcasts it, and the adoption sweep must never destroy the local one (a sweep of the
    // client's own inventory container fataled the client at the next GC purge). The opposite of
    // IsWireSuppressedPropClass, whose client site destroys the local instance.
    return cls == P::name::PropInventoryContainerPlayerClass;
}

void Install(coop::net::Session* session) {
    g_session_ptr.store(session, std::memory_order_release);
    PT::SetSession(session);  // mirror; see SetSession comment above.
    // A composite latch: this runs at 125 Hz, and until every inner flag resolves each tick called
    // FindClass, a full GUObjectArray walk with a wstring per entry.
    static std::atomic<bool> g_allInstalled{false};
    if (g_allInstalled.load(std::memory_order_acquire)) return;
    if (!g_propInitScanDone) {
        // Gate: wait for the prop_C base class to load.
        void* propBase = R::FindClass(P::name::PropClass);
        if (propBase) {
            // The one-shot GUObjectArray scan for Init UFunctions in the prop_C lineage.
            const std::wstring kInitName(P::name::PropInitFn);
            const int32_t n = R::NumObjects();
            int registered = 0;
            for (int32_t i = 0; i < n; ++i) {
                void* obj = R::ObjectAt(i);
                if (!obj) continue;
                if (R::ClassNameOf(obj) != L"Function") continue;
                if (R::ToString(R::NameOf(obj)) != kInitName) continue;
                void* owningCls = R::OuterOf(obj);
                // The Aprop_C lineage and the prop-shaped litter bases (chipPile, clump,
                // trashBitsPile), the same Init protocol on all.
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
                // Seed the known set with every live keyed interactable, after the observers are
                // registered so a spawn racing the seed is caught by the Init POST (a duplicate
                // insert is a no-op); latched internally.
                PT::SeedKnownKeyedProps();
            }
        }
    }
    // The late-load catch, gated on the first scan (never at the menu) and throttled to ~1 Hz: each
    // unresolved class costs one FindClass, a full name walk, and prop_garbageClump_C may not load
    // until the first chipPile pickup, minutes in.
    if (g_propInitScanDone && !g_extraKeyedInitDone) {
        static int sExtraThrottle = 0;
        if ((sExtraThrottle++ % 125) == 0) RegisterExtraKeyedInitObservers();
    }
    if (!g_propDestroyObserverInstalled) {
        // The destroy seam is a UFunction::Func patch, not a ProcessEvent observer: the R-pickup
        // destroy and the pile and clump morph destroys are EX_CallMath-dispatched, invisible to a
        // PE observer, and Func funnels every route. The callback receives the dying actor as the
        // context.
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
    // g_extraKeyedInitDone is part of the latch, so Install keeps re-entering until the late-load
    // observers are hooked; then it is an O(1) no-op.
    if (g_propInitScanDone && g_extraKeyedInitDone && g_propDestroyObserverInstalled) {
        g_allInstalled.store(true, std::memory_order_release);
        UE_LOGI("prop_lifecycle: Install() complete -- subsequent calls are O(1) no-ops");
    }
}

DisconnectStats OnDisconnect() {
    DisconnectStats s;
    s.initProcessedDropped = PT::ClearProcessedInit();
    g_takeObjInFlight.store(false, std::memory_order_relaxed);
    return s;
}

}  // namespace coop::prop_lifecycle
