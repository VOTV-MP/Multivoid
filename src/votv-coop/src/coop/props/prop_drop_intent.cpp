// coop/props/prop_drop_intent.cpp -- see coop/props/prop_drop_intent.h for the design.

#include "coop/props/prop_drop_intent.h"

#include "coop/dev/prop_birth_key_probe.h"   // the seam's key-timing and drain-exit instrumentation
#include "coop/element/registry.h"          // EidForActor (drain: tracked/mirror exclusion)
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/hand_item.h"          // LocalHandActor (place detect: exclude the hand display)
#include "coop/props/prop_echo_suppress.h"  // PeekIncomingSpawn (exclude host-echo adopt spawns)
#include "coop/props/prop_save_data.h"
#include "coop/props/prop_element_tracker.h"// GetPropElementIdForActor, ResolveLiveActorByKey   
#include "coop/props/container_contents_sync.h"  // TakeObjInFlight -- mark a container-extraction birth
#include "coop/session/world_load_episode.h"  // InEpisode (quiet during the join loadObjects churn)
#include "ue_wrap/core/call.h"                   // ParamFrame + Call (setKey on the host re-spawn)
#include "ue_wrap/engine/engine.h"                 // BeginDeferredSpawn/FinishDeferredSpawn/SetActorScale3D
#include "ue_wrap/core/fname_utils.h"            // StringToFName
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/hot_path_guard.h"         // UE_ASSERT_GAME_THREAD
#include "ue_wrap/core/log.h"
#include "ue_wrap/actors/floppy_disc.h"
#include "ue_wrap/actors/prop.h"                   // the prop lineage, key, name and parity-identity accessors
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"            // profile::name::{GameplayStaticsClass,FinishSpawningActorFn,PropSetKeyFn}
#include "ue_wrap/desk/tape_caddy.h"            // IsReelClass whitelist + the Progress birth scalar
#include "ue_wrap/desk/phys_mods.h"             // IsModuleClass whitelist
#include "coop/interactables/physmods_sync.h"   // the denied-birth reap
#include "coop/interactables/drive_sync.h"      // the denied rack-take reap
#include "ue_wrap/desk/drive_chain.h"           // IsDriveClass whitelist
#include "ue_wrap/core/types.h"
#include "ue_wrap/core/ufunction_hook.h"         // InstallPostHook (chains after host_spawn_watcher's)

#include <atomic>
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_set>
#include <vector>

namespace coop::prop_drop_intent {
namespace {

namespace P  = ue_wrap::profile;
namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace GT = ue_wrap::game_thread;
namespace PT = coop::prop_element_tracker;

std::atomic<coop::net::Session*> g_session{nullptr};
inline coop::net::Session* LoadSession() { return g_session.load(std::memory_order_acquire); }

// The client place-detection state, game thread only.
void* g_finishSpawnFn = nullptr;  // GameplayStatics.FinishSpawningActor (resolved once)
bool  g_installed     = false;    // InstallPostHook done (chain once, not per tick)

struct PendingPlace {
    void*   actor = nullptr;
    int32_t idx   = -1;
    int     tries = 0;   // net-pump ticks waited for the loadData Key restore
    // True when this entry is the actor a container extraction just materialised (the
    // extraction-in-flight latch was live at enqueue). Admitted at drain as a host-authoritative
    // drop intent, since the client-extracted item's world actor is invisible to the host
    // otherwise (the fresh-birth whitelist covers only reel, module and drive births).
    bool    containerExtract = false;
};
std::vector<PendingPlace> g_pending;         // GT-only
constexpr size_t kMaxPending  = 32;          // runaway backstop (a settled client rarely has >1 in flight)
constexpr int    kMaxKeyTries = 8;           // ~8 net-pump ticks (~64 ms) for the Key to restore, then give up

// The park set: keys the client locally destroyed (a pickup) and may re-place. A bounded FIFO
// set: the keyed-destroy note inserts, a matching place consumes, overflow evicts the oldest.
// The invariant it guards: only author a drop intent for a key whose pickup destroy already
// crossed to the host (the host destroyed its copy), so the host re-spawn creates exactly one
// prop. The set and the FIFO hold exactly the same keys, and every insert, consume and evict
// touches both: a consume that dropped only the set desynced them, a re-parked same-key prop
// then had two FIFO copies, and after enough cycles the evict popped a stale copy and erased
// the live entry, so the place stopped syncing.
std::unordered_set<std::wstring> g_parkedKeys;   // GT-only
std::deque<std::wstring>         g_parkFifo;      // GT-only (eviction order); mirrors g_parkedKeys 1:1
constexpr size_t kMaxParked = 64;

// Remove a key from both containers (a consume, or any targeted un-park), preserving the
// mirror invariant.
void UnparkKey(const std::wstring& key) {
    if (g_parkedKeys.erase(key) == 0) return;   // not parked -> FIFO can't hold it either (invariant)
    for (auto it = g_parkFifo.begin(); it != g_parkFifo.end(); ++it) {
        if (*it == key) { g_parkFifo.erase(it); break; }   // at most ONE copy by the invariant
    }
}

// Fill a fixed-size wire short string (the key and class-name fields share the shape).
template <size_t N>
void FillWireStr(uint8_t& len, char (&data)[N], const std::wstring& s) {
    len = 0;
    for (size_t i = 0; i < s.size() && i < (N - 1); ++i) data[len++] = static_cast<char>(s[i]);
}

std::wstring WireToWide(const uint8_t len, const char* data, size_t cap) {
    std::wstring w;
    const size_t n = (len < cap) ? len : cap;
    w.reserve(n);
    for (size_t i = 0; i < n; ++i) w.push_back(static_cast<wchar_t>(static_cast<unsigned char>(data[i])));
    return w;
}

// The client finish-spawn post-hook: enqueue a fresh, untracked, non-echo keyed prop spawn in
// a settled session for place detection. Chains after the host spawn watcher's callback on
// the same UFunction, role-disjoint (that one is host-only, this client-only). Runs on the
// dispatching thread; the finish spawn is game-thread only in UE4 (actor construction).
void OnClientFinishSpawn(void* /*context*/, void* /*srcObj*/, void* result) {
    if (!GT::IsGameThread()) return;
    auto* s = LoadSession();
    if (!s || !s->connected()) return;
    if (s->role() != coop::net::Role::Client) return;      // host places broadcast via host_spawn_watcher
    // The end condition: quiet during the load episode and the reconcile window's load-kind
    // segments (the join and reload bracket, whose spawn churn flooded this path; the echo peek
    // below already scopes our own applies out). A mid-session bracket does not suppress: a
    // client's genuine place has no other delivery channel (the census express is host-only)
    // and the re-bracket sweep would doom it, so intent, host spawn, express, claim must flow.
    // The warning latch mirrors the destroy seam's.
    if (coop::world_load_episode::InEpisode()) return;     // quiet during the join loadObjects churn
    if (coop::world_load_episode::InReconcileWindow() &&
        coop::world_load_episode::ReconcileWindowIsLoadKind()) {
        static uint32_t sWarned = 0;
        ++sWarned;
        if (sWarned <= 5 || (sWarned <= 100 && sWarned % 10 == 0) || sWarned % 100 == 0) {
            UE_LOGW("[PROP-DROP] client place-detection suppressed #%u (reconcile window, "
                    "kind=load -- bracket spawn churn; a genuine blind place would re-arrive "
                    "via rejoin)", sWarned);
        }
        return;
    }
    void* actor = result;
    if (!actor || !R::IsLive(actor)) return;
    if (coop::prop_echo_suppress::PeekIncomingSpawn(actor)) return;  // a host-authored mirror we adopt, not a place
    if (!ue_wrap::prop::IsDescendantOfProp(actor)) return;          // keyed Aprop_C lineage only
    if (PT::GetPropElementIdForActor(actor) != coop::element::kInvalidId) return;  // already tracked = not a fresh place
    // The hand-axis exclusion, the enqueue half (a fast path only, not load-bearing here: at
    // finish-spawn return the hold update has not yet written the holding actor, so the freshly
    // spawned hand view actor can pass this check; the drain-time re-check in Tick is the
    // authoritative one, the host spawn watcher's proven shape). The hand-axis test also covers
    // remote display mirrors.
    if (coop::hand_item::IsHandAxisActor(actor)) return;
    if (g_pending.size() >= kMaxPending) {
        UE_LOGW("[PROP-DROP] client pending-place cap %zu hit -- dropping %p", kMaxPending, actor);
        coop::dev::prop_birth_key_probe::NotePendingCapHit(actor);
        return;
    }
    // Was a container extraction in flight when this actor spawned? The extracted item's actor
    // materialises inside the take call, so the latch is live exactly here. Marks the entry as a
    // container-extraction birth, admitted at drain.
    const bool fromContainerExtract = coop::props::container_contents_sync::TakeObjInFlight();
    if (coop::dev::prop_birth_key_probe::IsEnabled()) {
        // The seam reading the probe exists for: is the Key there before any drain tick waits?
        coop::dev::prop_birth_key_probe::NoteEnqueue(
            actor, R::ClassNameOf(actor), ue_wrap::prop::GetInteractableKeyString(actor),
            fromContainerExtract);
    }
    g_pending.push_back(PendingPlace{actor, R::InternalIndexOf(actor), 0, fromContainerExtract});
    if (fromContainerExtract)
        UE_LOGI("[PROP-DROP] CLIENT enqueued container-EXTRACT birth actor=%p (admitted at drain)", actor);
}

// Host: spawn the authoritative prop by key at the transform. Mirrors the spawn receiver's
// spawn-by-key (deferred begin, set the key, write the parity identity, finish) but does not
// mark an incoming spawn, so the host's own finish-spawn watcher catches it and broadcasts
// the authoritative spawn to every peer. Returns the spawned actor, or null.
void* HostSpawnPlacedProp(const coop::net::PropDropIntentPayload& p, const std::wstring& cls,
                          const std::wstring& key, uint8_t authorSlot) {
    void* clsObj = R::FindClass(cls.c_str());
    if (!clsObj) {
        UE_LOGW("[PROP-DROP] HOST FindClass('%ls') failed -- cannot spawn placed prop key='%ls'",
                cls.c_str(), key.c_str());
        return nullptr;
    }
    const ue_wrap::FVector  loc{p.locX, p.locY, p.locZ};
    const ue_wrap::FRotator rot{p.rotPitch, p.rotYaw, p.rotRoll};
    void* actor = E::BeginDeferredSpawn(clsObj, loc, rot);
    if (!actor) {
        UE_LOGW("[PROP-DROP] HOST BeginDeferredSpawn('%ls') failed", cls.c_str());
        return nullptr;
    }
    // Set the key before finishing: the init inside the finish spawn's construction script mints
    // a fresh key unless one is already set, and writing our wire key first keeps the cross-peer
    // identity. The setter is resolved on the prop base class (its declaring class) and cached,
    // as the spawn receiver does: the function lookup is exact-owner with no superclass climb,
    // so a leaf-class resolve missed every subclass that does not redeclare the setter, and the
    // host spawn then auto-minted a key unequal to the client's, an identity split and a
    // host-side duplicate. Every wire class reaching here is prop lineage (the intent author
    // gates on it), so the base's setter is a valid member call on the spawned actor.
    static void* s_setKeyFn = nullptr;
    if (!s_setKeyFn) {
        if (void* propBase = R::FindClass(P::name::PropClass))
            s_setKeyFn = R::FindFunction(propBase, P::name::PropSetKeyFn);
    }
    void* setKeyFn = s_setKeyFn;
    if (setKeyFn) {
        const R::FName kf = ue_wrap::fname_utils::StringToFName(key);
        if (kf.ComparisonIndex != 0) {
            ue_wrap::ParamFrame sk(setKeyFn);
            if (!sk.SetRaw(L"Key", &kf, sizeof(kf)) || !ue_wrap::Call(actor, sk)) {
                UE_LOGW("[PROP-DROP] HOST setKey('%ls') failed on '%ls'", key.c_str(), cls.c_str());
            }
        }
    } else {
        UE_LOGW("[PROP-DROP] HOST setKey UFunction not found on '%ls' -- prop will auto-mint a Key", cls.c_str());
    }
    // The single-player parity identity (the props-table name and the static,
    // remove-without-respawn, frozen and sleep flags) before finishing: the init resolves the
    // true mesh, mass and collision from the name (empty gives the default white cube).
    if (ue_wrap::prop::IsDescendantOfProp(actor)) {
        const std::wstring nameW = WireToWide(p.propName.len, p.propName.data, sizeof(p.propName.data));
        R::FName nameRow{0, 0};
        if (!nameW.empty() && nameW != L"None") nameRow = ue_wrap::fname_utils::StringToFName(nameW);
        namespace pf = coop::net::propspawn_flags;
        ue_wrap::prop::WriteSpParityIdentity(
            actor, nameRow,
            (p.physFlags & pf::kStatic) != 0,
            (p.physFlags & pf::kRemoveWOrespawn) != 0,
            (p.physFlags & pf::kFrozen) != 0,
            (p.physFlags & pf::kSleep) != 0);
    }
    // No incoming-spawn mark: the host finish-spawn watcher must see this and broadcast it.
    if (!E::FinishDeferredSpawn(actor, loc, rot)) {
        UE_LOGW("[PROP-DROP] HOST FinishDeferredSpawn('%ls') failed", cls.c_str());
        return nullptr;
    }
    // Scale is a runtime transform (the deferred begin takes location and rotation only); apply
    // it after finishing and before the next-tick drain re-reads the scale for the broadcast.
    if (p.scaleX > 0.001f || p.scaleY > 0.001f || p.scaleZ > 0.001f) {
        E::SetActorScale3D(actor, ue_wrap::FVector{p.scaleX, p.scaleY, p.scaleZ});
    }
    // The prop's own save record, if the author's copy is already here. It usually is not -- it
    // rides behind this intent in the same FIFO -- and then it lands on this actor by Key the
    // moment it arrives, which is what makes the store keyed by identity rather than by actor.
    // Until then this key is AWAITED: the host has just spawned a class-default copy, and
    // publishing that as canonical would overwrite the author's real state on the author's own
    // machine.
    if (!coop::prop_save_data::ApplyParked(actor, key))
        coop::prop_save_data::ExpectRecordFor(key, authorSlot);
    return actor;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_installed) return;

    // Throttle the object-array walks while the UFunction is unresolved (it loads on gameplay
    // entry).
    static int s_retry = 0;
    if (s_retry > 0) { --s_retry; return; }

    if (!g_finishSpawnFn) {
        void* gsCls = R::FindClass(P::name::GameplayStaticsClass);
        if (!gsCls) { s_retry = 60; return; }
        g_finishSpawnFn = R::FindFunction(gsCls, P::name::FinishSpawningActorFn);
        if (!g_finishSpawnFn) {
            UE_LOGW("prop_drop_intent: FinishSpawningActor UFunction not found -- client place-detect disabled");
            g_installed = true;  // permanent give-up (don't re-walk every tick)
            return;
        }
    }
    if (ue_wrap::ufunction_hook::InstallPostHook(g_finishSpawnFn, &OnClientFinishSpawn)) {
        UE_LOGI("prop_drop_intent: FinishSpawningActor post-hook installed (client place -> host DROP INTENT)");
    } else {
        UE_LOGW("prop_drop_intent: FinishSpawningActor post-hook FAILED (hook table full?) -- client place-detect off");
    }
    g_installed = true;
}

void Tick(coop::net::Session* session) {
    UE_ASSERT_GAME_THREAD("prop_drop_intent::Tick");
    if (g_pending.empty()) return;
    if (!session || !session->connected() || session->role() != coop::net::Role::Client) {
        // Tally these before dropping them: an entry discarded here is one the seam accepted and
        // never resolved, and a count of enqueues that does not balance against the exits cannot
        // say which bound lost a prop.
        for (const PendingPlace& e : g_pending)
            coop::dev::prop_birth_key_probe::NoteDrainExit(e.actor, "session-gone", e.tries,
                                                           std::wstring());
        g_pending.clear();
        return;
    }
    std::vector<PendingPlace> keep;
    namespace probe = coop::dev::prop_birth_key_probe;
    for (PendingPlace& e : g_pending) {
        if (!e.actor || !R::IsLiveByIndex(e.actor, e.idx)) {                          // died before drain
            probe::NoteDrainExit(e.actor, "died-before-drain", e.tries, std::wstring());
            continue;
        }
        if (coop::element::Registry::Get().EidForActor(e.actor) != coop::element::kInvalidId) { // got tracked/bound
            probe::NoteDrainExit(e.actor, "already-tracked", e.tries, std::wstring());
            continue;
        }
        if (coop::prop_echo_suppress::PeekIncomingSpawn(e.actor)) {                   // a late echo mark -> not a place
            probe::NoteDrainExit(e.actor, "late-echo", e.tries, std::wstring());
            continue;
        }
        // The drain-time hand-axis re-check: the enqueue-time check runs before the hold update
        // writes the holding actor (the host spawn watcher documents this window and excludes at
        // drain for the same reason). Without it a hold-to-pick-up's hand view husk, carrying the
        // item's parked key, authors a false drop intent: the host spawns a duplicate world prop
        // while the item is still in the player's hand, and the park is consumed. Drop the entry
        // permanently.
        if (coop::hand_item::IsHandAxisActor(e.actor)) {
            probe::NoteDrainExit(e.actor, "hand-axis-drop", e.tries, std::wstring());
            continue;
        }
        std::wstring key = ue_wrap::prop::GetInteractableKeyString(e.actor);
        if (key.empty() || key == L"None") {
            // The key is not restored yet (the load runs after the finish). Re-defer a few ticks.
            if (++e.tries <= kMaxKeyTries) keep.push_back(e);
            else probe::NoteDrainExit(e.actor, "key-wait-expired", e.tries, std::wstring());
            continue;
        }
        probe::NoteKeyReadable(e.actor, e.tries);
        const bool parked = (g_parkedKeys.find(key) != g_parkedKeys.end());
        // The fresh births. A client's fresh prop spawn never broadcasts (the lifecycle's client
        // skip), so a caddy or reel-box eject on a client is a local-only ghost; an unparked
        // reel-class pending entry here is that birth (mirrors are excluded by the echo peek above,
        // tracked actors by the eid check, and the actor already carries the key the init minted
        // inside the finish spawn). Author it host-side via the eject intent, the same spawn
        // author, class-whitelisted at the host. The whitelist widens to desk modules (the unplug
        // births a module into the hand, the same local-only-ghost class), to drives (a rack
        // take on a client births a payload-bearing drive into the hand; the payload rides the
        // drive payload broadcast at adoption, so no birth scalar is needed) and to floppy discs.
        //
        // The disc is the one that is NOT born into a hand: a device's eject drops it in the world
        // at the slot's mouth. Before it was admitted, a disc a client ejected from a device it had
        // not itself filled reached no other peer at all -- it lived on one machine until a rejoin
        // loaded the host's world without it. The park covers only a disc that same client put in.
        const bool isDiscBirth = ue_wrap::floppy_disc::EnsureResolved() &&
                                 ue_wrap::floppy_disc::IsDiscClass(R::ClassOf(e.actor));
        const bool freshBirth = !parked &&
            (isDiscBirth ||
             (ue_wrap::tape_caddy::EnsureResolved() &&
              ue_wrap::tape_caddy::IsReelClass(R::ClassOf(e.actor))) ||
             (ue_wrap::phys_mods::EnsureResolved() &&
              ue_wrap::phys_mods::IsModuleClass(R::ClassOf(e.actor))) ||
             (ue_wrap::drive_chain::EnsureResolved() &&
              ue_wrap::drive_chain::IsDriveClass(R::ClassOf(e.actor))));
        // A container-extraction birth is admitted too: the client's take materialises the
        // extracted item as a world actor, and without this the fresh-birth whitelist (reel, module
        // and drive only) drops it at drain and the item never reaches the host's world. The host's
        // duplicate guard keeps the intent safe.
        if (!parked && !freshBirth && !e.containerExtract) {   // not a place / not a whitelisted birth / not a container extract
            probe::NoteDrainExit(e.actor, "not-a-place-nor-whitelisted-birth", e.tries, key);
            continue;
        }
        // Author the host-authoritative spawn intent (a place, or a fresh birth).
        coop::net::PropDropIntentPayload p{};
        const std::wstring cls = R::ClassNameOf(e.actor);
        FillWireStr(p.className.len, p.className.data, cls);
        FillWireStr(p.key.len, p.key.data, key);
        namespace pf = coop::net::propspawn_flags;
        if (ue_wrap::prop::IsDescendantOfProp(e.actor)) {
            const std::wstring nm = ue_wrap::prop::GetPropNameString(e.actor);
            FillWireStr(p.propName.len, p.propName.data, nm);
            p.physFlags = 0;
            if (ue_wrap::prop::IsStatic(e.actor))           p.physFlags |= pf::kStatic;
            if (ue_wrap::prop::IsFrozen(e.actor))           p.physFlags |= pf::kFrozen;
            if (ue_wrap::prop::IsSleeping(e.actor))         p.physFlags |= pf::kSleep;
            if (ue_wrap::prop::ReadRemoveWOrespawn(e.actor)) p.physFlags |= pf::kRemoveWOrespawn;
        }
        if (freshBirth) {
            // Born asleep on the host (no free fall; the held-prop pose stream takes over) -- for
            // the three lineages that are born INTO A HAND. A disc is not: its device drops it at
            // the slot's mouth with nobody holding it, and a sleeping host copy would park in mid
            // air while the client's own copy fell, and then drag the client's back up the moment
            // the pose stream took over. It falls on the host, which is the peer that owns it.
            if (!isDiscBirth) p.physFlags |= pf::kSleep;
            // A locally born drive carries its payload in its data slot: note the authorship, so
            // the drive sync broadcasts it at adoption (the first eid sight); un-noted first sights
            // stay prime-only.
            if (ue_wrap::drive_chain::IsDriveClass(R::ClassOf(e.actor)))
                coop::drive_sync::NoteLocalDriveBirth(e.actor);
        }
        const auto loc = ue_wrap::engine::GetActorLocation(e.actor);
        const auto rot = ue_wrap::engine::GetActorRotation(e.actor);
        const auto scl = ue_wrap::engine::GetActorScale3D(e.actor);
        p.locX = loc.X; p.locY = loc.Y; p.locZ = loc.Z;
        p.rotPitch = ue_wrap::NormalizeAxis(rot.Pitch);
        p.rotYaw   = ue_wrap::NormalizeAxis(rot.Yaw);
        p.rotRoll  = ue_wrap::NormalizeAxis(rot.Roll);
        p.scaleX = scl.X; p.scaleY = scl.Y; p.scaleZ = scl.Z;
        session->SendReliable(freshBirth ? coop::net::ReliableKind::ReelEjectIntent
                                         : coop::net::ReliableKind::PropDropIntent,
                              &p, sizeof(p));
        // The prop's own save record behind the intent, same lane, same FIFO: the host respawns
        // this prop from the intent and would otherwise author a class-default copy -- a blank
        // tape, an empty disc -- and broadcast that as truth.
        coop::prop_save_data::Publish(session, e.actor, key);
        if (parked) UnparkKey(key);   // consume from BOTH the set AND the FIFO (mirror invariant)
        probe::NoteDrainExit(e.actor, freshBirth ? "authored-fresh-birth" : "authored-drop-intent",
                             e.tries, key);
        UE_LOGI("[PROP-DROP] CLIENT authored %s key='%ls' cls='%ls' name='%ls' loc=(%.1f,%.1f,%.1f)%s",
                freshBirth ? "FRESH-BIRTH intent" : "drop intent",
                key.c_str(), cls.c_str(),
                WireToWide(p.propName.len, p.propName.data, sizeof(p.propName.data)).c_str(),
                p.locX, p.locY, p.locZ,
                coop::prop_save_data::Covers(e.actor) ? " +record" : "");
    }
    g_pending.swap(keep);
}

void NoteClientKeyedDestroy(const std::wstring& key) {
    UE_ASSERT_GAME_THREAD("prop_drop_intent::NoteClientKeyedDestroy");
    if (key.empty() || key == L"None") return;
    if (g_parkedKeys.insert(key).second) {
        g_parkFifo.push_back(key);
        while (g_parkFifo.size() > kMaxParked) {
            coop::dev::prop_birth_key_probe::NoteParkEvict(g_parkFifo.front());
            g_parkedKeys.erase(g_parkFifo.front());
            g_parkFifo.pop_front();
        }
    }
}

void OnPropDropIntent(coop::net::Session& session, const coop::net::PropDropIntentPayload& p,
                      uint8_t senderSlot) {
    UE_ASSERT_GAME_THREAD("prop_drop_intent::OnPropDropIntent");
    if (session.role() != coop::net::Role::Host) return;   // host-authoritative (router also gates)
    const std::wstring cls = WireToWide(p.className.len, p.className.data, sizeof(p.className.data));
    const std::wstring key = WireToWide(p.key.len, p.key.data, sizeof(p.key.data));
    if (key.empty() || key == L"None" || cls.empty()) {
        UE_LOGW("[PROP-DROP] HOST drop intent from slot=%u missing key/class -- dropping", senderSlot);
        return;
    }
    // The duplicate guard: if the host somehow still has this key live (the grab destroy did not
    // cross), do not spawn a second one. The park-set invariant normally guarantees the host has
    // no copy here.
    if (coop::prop_element_tracker::ResolveLiveActorByKey(key, nullptr)) {
        UE_LOGW("[PROP-DROP] HOST already has key='%ls' live -- skip drop-intent re-spawn (no dup)", key.c_str());
        return;
    }
    void* actor = HostSpawnPlacedProp(p, cls, key, senderSlot);
    if (actor) {
        UE_LOGI("[PROP-DROP] HOST spawned client-placed prop key='%ls' cls='%ls' slot=%u at (%.1f,%.1f,%.1f) "
                "-- FinishSpawn watcher broadcasts it this tick",
                key.c_str(), cls.c_str(), senderSlot, p.locX, p.locY, p.locZ);
    }
}

void OnReelEjectIntent(coop::net::Session& session, const coop::net::PropDropIntentPayload& p,
                       uint8_t senderSlot) {
    UE_ASSERT_GAME_THREAD("prop_drop_intent::OnReelEjectIntent");
    if (session.role() != coop::net::Role::Host) return;
    // The client fresh-birth author, class-whitelisted: reels (the caddy eject), desk modules
    // (the socket unplug), drives and floppy discs. Not a general client-spawn door; any other
    // class here is a protocol violation, dropped.
    const std::wstring cls = WireToWide(p.className.len, p.className.data, sizeof(p.className.data));
    void* clsObj = R::FindClass(cls.c_str());
    const bool isReel = clsObj && ue_wrap::tape_caddy::EnsureResolved() &&
                        ue_wrap::tape_caddy::IsReelClass(clsObj);
    const bool isModule = clsObj && ue_wrap::phys_mods::EnsureResolved() &&
                          ue_wrap::phys_mods::IsModuleClass(clsObj);
    const bool isDrive = clsObj && ue_wrap::drive_chain::EnsureResolved() &&
                         ue_wrap::drive_chain::IsDriveClass(clsObj);
    const bool isDisc = clsObj && ue_wrap::floppy_disc::EnsureResolved() &&
                        ue_wrap::floppy_disc::IsDiscClass(clsObj);
    if (!isReel && !isModule && !isDrive && !isDisc) {
        UE_LOGW("[PROP-DROP] HOST birth intent from slot=%u rejected: class '%ls' not whitelisted",
                senderSlot, cls.c_str());
        return;
    }
    // A module birth matching a fresh unplug deny for this sender is the raced ghost that got
    // dropped before the deny landed; reap it (the physics-mods sync logs).
    if (isModule && coop::physmods_sync::HostShouldReapModuleBirth(senderSlot, clsObj)) return;
    // Drive births are authored normally; a denied rack-take ghost is reaped later by its
    // adoption payload's content hash (the drive sync).
    OnPropDropIntent(session, p, senderSlot);  // same author: dup-guard + HostSpawnPlacedProp
}

void Reset() {
    UE_ASSERT_GAME_THREAD("prop_drop_intent::Reset");
    // The drain's session gate normally empties this first; anything still here reached teardown
    // unresolved, and the tally says so rather than losing it.
    for (const PendingPlace& e : g_pending)
        coop::dev::prop_birth_key_probe::NoteDrainExit(e.actor, "reset-dropped", e.tries,
                                                       std::wstring());
    g_pending.clear();
    g_parkedKeys.clear();
    g_parkFifo.clear();
}

}  // namespace coop::prop_drop_intent
