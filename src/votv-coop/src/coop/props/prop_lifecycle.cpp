// coop/props/prop_lifecycle.cpp -- the Aprop_C spawn seams: the Init watch that expresses a keyed
// prop's birth on the wire, the install of the destroy seam, and the class predicates. The destroy
// seam body lives in prop_destroy_seam.cpp and the container extract in prop_container_extract.cpp.

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
#include "coop/props/prop_wire_parity.h"  // PhysFlagsOf, the one flag builder
#include "ue_wrap/core/call.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/core/ufunction_hook.h"  // the destroy seam's Func patch on K2_DestroyActor

#include <atomic>
#include <cstdint>
#include <string>

namespace coop::prop_lifecycle {
namespace {

namespace P = ue_wrap::profile;
namespace R = ue_wrap::reflection;
namespace PT = coop::prop_element_tracker;
namespace SG = ue_wrap::script_gate;

// The session pointer the seams read role() and Send*() through: an atomic, written at a session's
// start on the timeline thread and by the lanes' Install on the game thread, read by the seams.
// Defined at namespace scope below, shared with prop_destroy_seam.cpp through
// prop_lifecycle_detail.h.

// Install idempotency: the Init watch is asked for once (the gate refuses one only with a full table or
// when it never installed), the known set is seeded once, the destroy seam settles once. Game thread.
bool g_initWatchAsked = false;
bool g_initWatchRefused = false;
bool g_seeded = false;
bool g_destroySeamSettled = false;
constexpr int kTagInit = 0x50494E54;  // 'PINT'

// The takeObj-in-flight bracket is defined in prop_container_extract.cpp and shared through
// prop_lifecycle_detail.h; the Init POST body reads it, OnDisconnect clears it.

void GrabObserver_Aprop_Init_POST_Body(void* self) {
    if (!self) return;
    // The known-keyed set is marked before the session and echo gates. Its bulk comes from the seed
    // walk, not from here: a save load runs each prop's Init from its own Blueprint (its construction
    // script, its ubergraph, loadData), which this seam does not take -- 0 of 10,685 keyed Init bodies
    // on a host came through ProcessEvent, 0 of 11,989 on a joining client, its boot world's included.
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
    if (coop::prop_echo_suppress::IsMirrorSpawn(self)) {
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
            // A client-originated Aprop_C world spawn is skipped HERE by the host-authoritative
            // rule. That is not the same as "the host never learns of it": the drop-intent lane
            // (coop/props/prop_drop_intent) authors it a tick later when it is a place with a
            // parked key, one of the four whitelisted device births, a container extract, or a
            // birth a player's own spawn verb asked for. This line says only that the EXPRESS
            // declined; read the lane's own `[PROP-DROP] CLIENT authored ...` line, or the
            // `prop_birth_key_probe` drain exit, for what actually happened to it. Logged with key
            // and eid so a repro correlates with the host log.
            const coop::element::ElementId dropEid = PT::GetPropElementIdForActor(self);
            ue_wrap::FVector dloc{};
            const bool dlocRead = ue_wrap::engine::TryGetActorLocation(self, dloc);
            UE_LOGI("[ROCK-DROP] CLIENT Aprop spawn not expressed here (host-auth skip): cls='%ls' key='%ls' "
                    "eid=%u loc=(%.1f,%.1f,%.1f)%s -- the drop-intent lane decides whether it crosses",
                    cls.c_str(), ue_wrap::prop::GetInteractableKeyString(self).c_str(),
                    (dropEid == coop::element::kInvalidId) ? 0u : static_cast<unsigned>(dropEid),
                    dloc.X, dloc.Y, dloc.Z, dlocRead ? "" : " (unread)");
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
    // A spawn with no readable location or rotation has nothing to place its mirror by: left
    // unexpressed, like an unkeyed one, both reads before the Prop Element is created below.
    ue_wrap::FVector loc{};
    ue_wrap::FRotator rot{};
    if (!ue_wrap::engine::TryGetActorLocation(self, loc) || !ue_wrap::engine::TryGetActorRotation(self, rot)) {
        UE_LOGW("grab_hook[Aprop.Init POST]: actor %p (class '%ls') has no readable location or rotation -- skip",
                self, cls.c_str());
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
    p.locX = loc.X; p.locY = loc.Y; p.locZ = loc.Z;
    p.rotPitch = ue_wrap::NormalizeAxis(rot.Pitch);
    p.rotYaw   = ue_wrap::NormalizeAxis(rot.Yaw);
    p.rotRoll  = ue_wrap::NormalizeAxis(rot.Roll);
    // The real scale, the identity row and the parity bools: a fresh gameplay spawn is simulating,
    // and the parity bits let the mirror's init() resolve the true mesh, mass and collision, not
    // the CDO cube.
    const auto scl = ue_wrap::engine::GetActorScale3D(self);
    p.scaleX = scl.X; p.scaleY = scl.Y; p.scaleZ = scl.Z;
    // The trash variant, as the join snapshot's builder already sends it: without it a
    // trash-family prop born at runtime -- a bagged pile's filled bag, a dropped clump -- mirrors
    // at variant 0 and wears the wrong mesh. GetChipType answers 0 on a class without the
    // property, so this is one line for the whole family rather than a case for one class.
    p.chipType = ue_wrap::prop::GetChipType(self);
    // The live prop's own flags (0 for anything but an Aprop_C), with simulation stated on: a
    // birth moves.
    p.physFlags = coop::prop_wire_parity::PhysFlagsOf(self) |
                  coop::net::propspawn_flags::kSimulatePhysics;
    p.propName.len = 0;
    if (ue_wrap::prop::IsDescendantOfProp(self)) {
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

// The Init watch: the post of every Init body the game thread runs, of which this seam keeps those with
// no calling Blueprint frame -- the ones ProcessEvent dispatched (an engine event, a timer, a delegate,
// a reflected call of ours such as a mirror's skin), the set the per-class ProcessEvent observers saw.
// A Blueprint's own call (its construction script, loadData, a super call) runs where the key may not
// be final yet, and a deferred spawn's is expressed at FinishSpawningActor (host_spawn_watcher).
// Keyed on the name, the watch holds for every class of the lineage, one that loads late or comes back
// as a new object with a world included. The gate fires on the game thread, where the body's engine
// reads belong, and while anything holds it: a dev probe can hold it before any session has set the
// pointer.
void OnInitPost(const SG::Call& call) {
    if (call.callerFunction || !call.object || !LoadSession()) return;
    GrabObserver_Aprop_Init_POST_Body(call.object);
}

// The destroy seam (prop_destroy_seam.cpp): a UFunction::Func patch on Actor.K2_DestroyActor,
// which fires for every dispatch route including the EX_CallMath destroys a ProcessEvent
// observer never sees (the R-pickup destroy, the pile and clump morphs). Post-native:
// K2_DestroyActor only marks PendingKill, so reads on the actor are still valid.

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
    // A composite latch over the three settlements below, each a few ticks at most: this runs on every
    // pump tick of the world.
    static std::atomic<bool> g_allInstalled{false};
    if (g_allInstalled.load(std::memory_order_acquire)) return;
    if (!g_initWatchAsked) {
        g_initWatchAsked = true;
        g_initWatchRefused = !SG::WatchName(P::name::PropInitFn, kTagInit, nullptr, &OnInitPost);
        if (g_initWatchRefused)
            UE_LOGE("grab_hook: the script gate refused the Init watch -- a keyed prop born through a "
                    "ProcessEvent-dispatched Init is not expressed");
    }
    if (!g_seeded) {
        // The seed walk waits for the watch to settle: live, a birth racing the walk while a session holds
        // the gate is caught by one or the other (a duplicate insert is a no-op); dead or refused, the
        // walk goes on without it. And for prop_C, the base of the Aprop_C lineage, to be loaded.
        const bool watchSettled = g_initWatchRefused || SG::NameWatchSettled(P::name::PropInitFn, kTagInit);
        if (watchSettled && ue_wrap::object_index::ClassByName(P::name::PropClass)) {
            g_seeded = true;
            if (!g_initWatchRefused && SG::NameWatchLive(P::name::PropInitFn, kTagInit)) {
                UE_LOGI("grab_hook: the Init watch is live -- seeding the known keyed props");
            } else {
                UE_LOGE("grab_hook: the Init watch is DEAD (%s) -- seeding the known keyed props without it",
                        g_initWatchRefused ? "refused at registration" : "its name resolved into a full table");
            }
            PT::SeedKnownKeyedProps();
        }
    }
    if (!g_destroySeamSettled) {
        // The destroy seam is a UFunction::Func patch, not a ProcessEvent observer: the R-pickup
        // destroy and the pile and clump morph destroys are EX_CallMath-dispatched, invisible to a
        // PE observer, and Func funnels every route. The callback receives the dying actor as the
        // context. Settled on the first attempt: Actor is native, in the index from boot with its
        // functions, and a patch that failed is not mended by trying it again.
        if (void* actorCls = ue_wrap::object_index::ClassByName(P::name::ActorClassName)) {
            g_destroySeamSettled = true;
            void* fn = R::FindFunction(actorCls, P::name::DestroyActorFn);
            if (!fn) {
                UE_LOGW("grab_hook: %ls.%ls UFunction not found -- destroy broadcast disabled",
                        P::name::ActorClassName, P::name::DestroyActorFn);
            } else if (ue_wrap::ufunction_hook::InstallPostHook(fn, &OnK2DestroyFunc)) {
                UE_LOGI("grab_hook: Func-patched %ls.%ls @ %p (destroy seam -- catches "
                        "EX_CallMath destroys the old PE observer missed)",
                        P::name::ActorClassName, P::name::DestroyActorFn, fn);
            } else {
                UE_LOGE("grab_hook: the Func patch on %ls.%ls failed -- destroy broadcast disabled",
                        P::name::ActorClassName, P::name::DestroyActorFn);
            }
        }
    }
    if (g_seeded && g_destroySeamSettled) {
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
