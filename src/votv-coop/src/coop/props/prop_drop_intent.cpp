// coop/props/prop_drop_intent.cpp -- see coop/props/prop_drop_intent.h for the design.

#include "coop/props/prop_drop_intent.h"

#include "coop/element/registry.h"          // EidForActor (drain: tracked/mirror exclusion)
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/hand_item.h"          // LocalHandActor (place detect: exclude the hand display)
#include "coop/props/prop_echo_suppress.h"  // PeekIncomingSpawn (exclude host-echo adopt spawns)
#include "coop/props/prop_element_tracker.h"// GetPropElementIdForActor, ResolveLiveActorByKey
#include "coop/session/world_load_episode.h"  // InEpisode (quiet during the join loadObjects churn)
#include "ue_wrap/core/call.h"                   // ParamFrame + Call (setKey on the host re-spawn)
#include "ue_wrap/engine/engine.h"                 // BeginDeferredSpawn/FinishDeferredSpawn/SetActorScale3D
#include "ue_wrap/core/fname_utils.h"            // StringToFName
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/hot_path_guard.h"         // UE_ASSERT_GAME_THREAD
#include "ue_wrap/core/log.h"
#include "ue_wrap/actors/prop.h"                   // IsDescendantOfProp, GetInteractableKeyString,
                                            // GetPropNameString, WriteSpParityIdentity
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"            // profile::name::{GameplayStaticsClass,FinishSpawningActorFn,PropSetKeyFn}
#include "ue_wrap/desk/tape_caddy.h"             // v114 (L7): IsReelClass whitelist + the Progress birth scalar
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

// ---- CLIENT place-detection state (game-thread only) ----------------------
void* g_finishSpawnFn = nullptr;  // GameplayStatics.FinishSpawningActor (resolved once)
bool  g_installed     = false;    // InstallPostHook done (chain once, not per tick)

struct PendingPlace {
    void*   actor = nullptr;
    int32_t idx   = -1;
    int     tries = 0;   // net-pump ticks waited for the loadData Key restore
};
std::vector<PendingPlace> g_pending;         // GT-only
constexpr size_t kMaxPending  = 32;          // runaway backstop (a settled client rarely has >1 in flight)
constexpr int    kMaxKeyTries = 8;           // ~8 net-pump ticks (~64 ms) for the Key to restore, then give up

// ---- park set: keys the CLIENT locally destroyed (pickup) and may re-place --
// A bounded FIFO SET. NoteClientKeyedDestroy inserts; a matching place consumes; overflow evicts the
// oldest. The invariant it guards: only author a drop intent for a key whose pickup-DESTROY already
// crossed to the host (host destroyed its copy) => the host re-spawn creates exactly ONE prop.
// INVARIANT: g_parkedKeys and g_parkFifo hold EXACTLY the same keys -- every insert/consume/evict
// touches BOTH (a consume that dropped only the set desynced them: a re-parked same-key prop then had
// TWO FIFO copies, and after kMaxParked cycles the evict popped a stale copy and erased the LIVE
// entry -> the place stopped syncing; audit 2026-07-09 MEDIUM).
std::unordered_set<std::wstring> g_parkedKeys;   // GT-only
std::deque<std::wstring>         g_parkFifo;      // GT-only (eviction order); mirrors g_parkedKeys 1:1
constexpr size_t kMaxParked = 64;

// Remove a key from BOTH containers (consume / any targeted un-park), preserving the mirror invariant.
void UnparkKey(const std::wstring& key) {
    if (g_parkedKeys.erase(key) == 0) return;   // not parked -> FIFO can't hold it either (invariant)
    for (auto it = g_parkFifo.begin(); it != g_parkFifo.end(); ++it) {
        if (*it == key) { g_parkFifo.erase(it); break; }   // at most ONE copy by the invariant
    }
}

// Fill a fixed-size wire short-string (WireKey/WireClassName share the {len,data[]} shape).
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

// The CLIENT FinishSpawn post-hook: enqueue a fresh, untracked, non-echo keyed Aprop_C spawn in a
// SETTLED session for place-detection. Chains after host_spawn_watcher's callback on the same
// UFunction; role-disjoint (host_spawn_watcher's is host-only, this is client-only). Runs on the
// dispatching thread -- FinishSpawningActor is game-thread only in UE4 (actor construction).
void OnClientFinishSpawn(void* /*context*/, void* /*srcObj*/, void* result) {
    if (!GT::IsGameThread()) return;
    auto* s = LoadSession();
    if (!s || !s->connected()) return;
    if (s->role() != coop::net::Role::Client) return;      // host places broadcast via host_spawn_watcher
    if (coop::world_load_episode::InEpisode()) return;     // quiet during the join loadObjects churn
    void* actor = result;
    if (!actor || !R::IsLive(actor)) return;
    if (coop::prop_echo_suppress::PeekIncomingSpawn(actor)) return;  // a host-authored mirror we adopt, not a place
    if (!ue_wrap::prop::IsDescendantOfProp(actor)) return;          // keyed Aprop_C lineage only
    if (PT::GetPropElementIdForActor(actor) != coop::element::kInvalidId) return;  // already tracked = not a fresh place
    // Hand-axis exclusion, ENQUEUE half (fast path only -- NOT load-bearing here: at FinishSpawn-return
    // updateHold has NOT yet written holding_actor, so the freshly-spawned hand view actor can pass this
    // check; the DRAIN-time re-check in Tick() is the authoritative one, the proven host_spawn_watcher
    // shape. Audit 2026-07-10 CRITICAL.). IsHandAxisActor also covers remote display mirrors.
    if (coop::hand_item::IsHandAxisActor(actor)) return;
    if (g_pending.size() >= kMaxPending) {
        UE_LOGW("[PROP-DROP] client pending-place cap %zu hit -- dropping %p", kMaxPending, actor);
        return;
    }
    g_pending.push_back(PendingPlace{actor, R::InternalIndexOf(actor), 0});
}

// HOST: spawn the authoritative Aprop by Key at the transform. Mirrors remote_prop_spawn's
// spawn-by-key (BeginDeferred -> setKey -> WriteSpParityIdentity -> FinishSpawningActor) but does NOT
// MarkIncomingSpawn -- so the host's own FinishSpawn watcher (host_spawn_watcher) catches it and
// broadcasts the authoritative PropSpawn to every peer. Returns the spawned actor (or null).
void* HostSpawnPlacedProp(const coop::net::PropDropIntentPayload& p, const std::wstring& cls,
                          const std::wstring& key) {
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
    // setKey BEFORE Finish -- Init() (inside FinishSpawningActor's UCS) mints a NewGuid Key unless one
    // is already set; writing our wire Key first keeps the cross-peer identity. Resolve setKey on the
    // Aprop_C BASE (its declaring class), cached -- exactly like remote_prop_spawn::ResolveSpawnFns:
    // FindFunction is EXACT-OWNER, no SuperStruct climb ([[lesson-findfunction-exact-owner-no-
    // superstruct-climb]]), so the previous leaf-class resolve missed every subclass that does not
    // redeclare setKey (live 2026-07-11: prop_crowbar_C -> "setKey UFunction not found" -> the host
    // spawn auto-minted a key != the client's local copy -> identity split -> host-side crowbar dupe).
    // Every wire class reaching here is Aprop_C lineage (the intent author gates IsDescendantOfProp),
    // so the base's setKey is a valid member call on the spawned actor.
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
    // SP-parity identity (list_props Name + Static/removeWOrespawn/frozen/sleep) BEFORE Finish -- init()
    // resolves the true mesh/mass/collision from Name (empty -> CDO 'cube' white-box, the v54 root).
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
    // NO MarkIncomingSpawn -- the host FinishSpawn watcher MUST see this and broadcast it.
    if (!E::FinishDeferredSpawn(actor, loc, rot)) {
        UE_LOGW("[PROP-DROP] HOST FinishDeferredSpawn('%ls') failed", cls.c_str());
        return nullptr;
    }
    // Scale is a runtime transform (BeginDeferredSpawn takes loc+rot only); apply after Finish, BEFORE
    // the next-tick DrainPendingSpawns re-reads GetActorScale3D for the broadcast.
    if (p.scaleX > 0.001f || p.scaleY > 0.001f || p.scaleZ > 0.001f) {
        E::SetActorScale3D(actor, ue_wrap::FVector{p.scaleX, p.scaleY, p.scaleZ});
    }
    // v114 (L7): the save-scalar birth channel -- write it NOW so the next-tick express drain
    // re-reads the live actor and the broadcast PropSpawn carries it (reel Progress; post-Finish
    // is measured safe -- the reel's consumers are lookAt + loadData only).
    if (p.physFlags & coop::net::propspawn_flags::kHasSavedScalar) {
        if (!ue_wrap::prop::ApplySavedScalarForClass(actor, p.savedScalar)) {
            UE_LOGW("[PROP-DROP] HOST savedScalar=%.2f apply failed on '%ls'", p.savedScalar, cls.c_str());
        }
    }
    return actor;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_installed) return;

    // Throttle the GUObjectArray walks while the UFunction is unresolved (loads on gameplay entry).
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
        g_pending.clear();
        return;
    }
    std::vector<PendingPlace> keep;
    for (PendingPlace& e : g_pending) {
        if (!e.actor || !R::IsLiveByIndex(e.actor, e.idx)) continue;                 // died before drain
        if (coop::element::Registry::Get().EidForActor(e.actor) != coop::element::kInvalidId) continue; // got tracked/bound
        if (coop::prop_echo_suppress::PeekIncomingSpawn(e.actor)) continue;          // a late echo mark -> not a place
        // DRAIN-time hand-axis re-check (audit 2026-07-10 CRITICAL): the enqueue-time check runs before
        // updateHold writes holding_actor (host_spawn_watcher.cpp documents this exact window and excludes
        // at drain for the same reason). Without this, a hold-R pickup's hand view husk -- carrying the
        // item's PARKED key -- authors a FALSE drop intent: the host spawns a duplicate world prop while
        // the item is still in the player's hand, and the park is consumed. Drop the entry permanently.
        if (coop::hand_item::IsHandAxisActor(e.actor)) continue;
        std::wstring key = ue_wrap::prop::GetInteractableKeyString(e.actor);
        if (key.empty() || key == L"None") {
            // Key not restored yet (loadData runs AFTER Finish). Re-defer a few ticks.
            if (++e.tries <= kMaxKeyTries) keep.push_back(e);
            continue;
        }
        const bool parked = (g_parkedKeys.find(key) != g_parkedKeys.end());
        // v114 (L7): the reel EJECT birth. A client's fresh Aprop_C spawn never broadcasts
        // (prop_lifecycle client-skip), so a caddy/reelbox eject on a CLIENT is a local-only
        // ghost. An UNPARKED reel-class pending entry here IS that birth (mirrors are excluded
        // by PeekIncomingSpawn above; tracked actors by the EidForActor check; the actor already
        // carries the NewGuid key Init minted inside FinishSpawn). Author it host-side via
        // ReelEjectIntent -- the same HostSpawnPlacedProp author, class-whitelisted at the host.
        const bool reelEject = !parked &&
                               ue_wrap::tape_caddy::EnsureResolved() &&
                               ue_wrap::tape_caddy::IsReelClass(R::ClassOf(e.actor));
        if (!parked && !reelEject) continue;  // not a place of a picked-up prop, not a reel birth
        // Author the host-authoritative spawn intent (place OR reel-eject birth).
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
            // The save-scalar birth channel rides BOTH intent kinds (correctness-audit
            // CRITICAL 1): the parked place (pocket->place of a reel) must carry Progress
            // exactly like the eject birth, or the host respawn resets it to the CDO
            // default (a blank tape) and broadcasts that as truth. No-op for classes
            // without a save scalar.
            float sc = 0.f;
            if (ue_wrap::prop::ReadSavedScalarForClass(e.actor, sc)) {
                p.savedScalar = sc;
                p.physFlags |= pf::kHasSavedScalar;
            }
        }
        if (reelEject) {
            // Born ASLEEP on the host (no free-fall; the held-prop pose stream takes over).
            p.physFlags |= pf::kSleep;
        }
        const auto loc = ue_wrap::engine::GetActorLocation(e.actor);
        const auto rot = ue_wrap::engine::GetActorRotation(e.actor);
        const auto scl = ue_wrap::engine::GetActorScale3D(e.actor);
        p.locX = loc.X; p.locY = loc.Y; p.locZ = loc.Z;
        p.rotPitch = ue_wrap::NormalizeAxis(rot.Pitch);
        p.rotYaw   = ue_wrap::NormalizeAxis(rot.Yaw);
        p.rotRoll  = ue_wrap::NormalizeAxis(rot.Roll);
        p.scaleX = scl.X; p.scaleY = scl.Y; p.scaleZ = scl.Z;
        session->SendReliable(reelEject ? coop::net::ReliableKind::ReelEjectIntent
                                        : coop::net::ReliableKind::PropDropIntent,
                              &p, sizeof(p));
        if (parked) UnparkKey(key);   // consume from BOTH the set AND the FIFO (mirror invariant)
        UE_LOGI("[PROP-DROP] CLIENT authored %s key='%ls' cls='%ls' name='%ls' loc=(%.1f,%.1f,%.1f)%s",
                reelEject ? "REEL-EJECT intent" : "drop intent",
                key.c_str(), cls.c_str(),
                WireToWide(p.propName.len, p.propName.data, sizeof(p.propName.data)).c_str(),
                p.locX, p.locY, p.locZ,
                (p.physFlags & pf::kHasSavedScalar) ? " +savedScalar" : "");
    }
    g_pending.swap(keep);
}

void NoteClientKeyedDestroy(const std::wstring& key) {
    UE_ASSERT_GAME_THREAD("prop_drop_intent::NoteClientKeyedDestroy");
    if (key.empty() || key == L"None") return;
    if (g_parkedKeys.insert(key).second) {
        g_parkFifo.push_back(key);
        while (g_parkFifo.size() > kMaxParked) {
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
    // Dup guard: if the host somehow still has this Key live (the grab-destroy didn't cross), do NOT
    // spawn a second one. The park-set invariant normally guarantees the host has no copy here.
    if (coop::prop_element_tracker::ResolveLiveActorByKey(key, nullptr)) {
        UE_LOGW("[PROP-DROP] HOST already has key='%ls' live -- skip drop-intent re-spawn (no dup)", key.c_str());
        return;
    }
    void* actor = HostSpawnPlacedProp(p, cls, key);
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
    // v114 (L7): the client-eject birth author. CLASS-WHITELISTED to the Aprop_reel_C lineage --
    // this is NOT a general client-spawn door (the design's explicit gate; a non-reel class here
    // is a protocol violation, logged and dropped).
    const std::wstring cls = WireToWide(p.className.len, p.className.data, sizeof(p.className.data));
    void* clsObj = R::FindClass(cls.c_str());
    if (!clsObj || !ue_wrap::tape_caddy::EnsureResolved() ||
        !ue_wrap::tape_caddy::IsReelClass(clsObj)) {
        UE_LOGW("[PROP-DROP] HOST ReelEjectIntent from slot=%u rejected: class '%ls' is not a reel",
                senderSlot, cls.c_str());
        return;
    }
    OnPropDropIntent(session, p, senderSlot);  // same author: dup-guard + HostSpawnPlacedProp
}

void Reset() {
    UE_ASSERT_GAME_THREAD("prop_drop_intent::Reset");
    g_pending.clear();
    g_parkedKeys.clear();
    g_parkFifo.clear();
}

}  // namespace coop::prop_drop_intent
