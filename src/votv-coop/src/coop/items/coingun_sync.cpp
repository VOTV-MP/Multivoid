// coop/items/coingun_sync.cpp -- THE SALE LANE: a client shoots a prop, the HOST prices and mints.
// See coop/items/coingun_sync.h for the design and WHY, and coingun_internal.h for the 2026-08-25
// cut that moved the COLLECT lane (both pickup entries + the host's perform) to coingun_collect.cpp.

#include "coop/items/coingun_sync.h"

#include "coingun_internal.h"   // co-located private header (src tree, not include/)

#include "coop/comms/peer_action_feed.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"   // A50: the sender's puppet body, to measure its reach
#include "coop/props/prop_element_tracker.h"
#include "coop/session/world_load_episode.h"
#include "coop/world/world_actor_sync.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/core/vm_dispatch.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/world/economy.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace coop::coingun_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace GT = ue_wrap::game_thread;
namespace vm = ue_wrap::vm_dispatch;

}  // namespace

// The two verb NAMES both lanes key on. Definitions for the `extern` declarations in
// coingun_internal.h -- see that header for the cut, and vm_dispatch.h's contract box for why the
// NAME is the handle and never the id or `active`.
const wchar_t* const kVerbNameGunUse  = L"playerHandUse_LMB";
const wchar_t* const kVerbNameCollect = L"actionOptionIndex";

namespace {

std::atomic<coop::net::Session*> g_session{nullptr};
inline coop::net::Session* LoadSession() { return g_session.load(std::memory_order_acquire); }

std::atomic<bool> g_installed{false};
std::atomic<bool> g_verbRegistered{false};

// Resolved once at Install.
void* g_finishSpawnFn = nullptr;
void* g_libCdo        = nullptr;   // Default__lib_C -- sellObject's context
void* g_sellObjectFn  = nullptr;
void* g_coinClass     = nullptr;
void* g_gunClass      = nullptr;   // prop_coingun_C -- the Context gate (C-1) and the mint executor
void* g_sellFn        = nullptr;   // prop_coingun_C::sell -- cached (M-5: was re-resolved per sale)
int32_t g_offCoinSphere  = -1;   // Abaocoin_C::Sphere -- the SIMULATING component (NOT the root)
void* g_setSimFn      = nullptr;   // UPrimitiveComponent::SetSimulatePhysics
ue_wrap::CachedObjRef g_gunRef;     // a live gun instance, world-stamped (C-3: was a walk per sale)
int32_t g_offCoinPoints  = -1;   // Abaocoin_C::points  (@0x0240 in the dump; resolved by NAME)
int32_t g_offPropMesh    = -1;   // Aprop_C::StaticMesh (@0x0238 in the dump; resolved by NAME)

// ---- diagnostics (a 1/s line would be noise; these ride the event logs) -------------------------
std::atomic<unsigned long long> g_capturedCoins{0};
std::atomic<unsigned long long> g_barrierDestroyed{0};
std::atomic<unsigned long long> g_anomalyBirths{0};
std::atomic<unsigned long long> g_salesSent{0};
std::atomic<unsigned long long> g_salesRefused{0};
// ---- THE BARRIER QUEUE -------------------------------------------------------------------------
// Client-side coins captured inside the gun bracket, resolved at the next net-pump Tick. These are
// world-scoped engine objects crossing a frame boundary, so they are CachedObjRef (CLAUDE.md 4j: a
// dying world's actors are not kill-flagged until GC purge, measured 44+ s, so slot liveness alone
// hands out actors of a world that no longer exists). A bare AActor* here would be the exact defect
// the 2026-08-23 world-stamp arc converted 78 sites away from.
//
// v140: THE CAPTURE IS NOW PER-SHOT AND CONDITIONAL, which is the root fix the header had already
// named and the code had not implemented. The capture used to be UNCONDITIONAL -- it keyed on the
// gun verb bracket alone -- while the authorization that justifies it was decided LATER and
// ELSEWHERE. Four client-side paths eat a shot's coins and author no sale at all (the world-load
// episode return, the R-4a reconcile window, the kerfur capture, and SendSaleForDyingProp's own
// no-name return), and on every one of them the player lost the coins AND the prop AND got no
// sentence, which is exactly the principle-8 shape: a local artifact must not be suppressed until
// the authoritative one is CONFIRMED.
//
// So a shot opens a group at the verb bracket, coins land in it, and the group is only committed to
// destruction if that shot ACTUALLY SENT a sale. An unauthored shot RELEASES its coins: they stay,
// they credit locally, and that is precisely today's single-player behaviour -- no new loss, no
// phantom, nothing to heal.
struct PendingShot {
    std::vector<ue_wrap::CachedObjRef> coins;
    bool authored = false;
};
std::mutex g_pendingMu;
std::vector<PendingShot> g_pendingShots;

// THE CONSUMPTION GUARD (C-3, audit 2026-08-24; RE-KEYED AND GIVEN A REAL LIFETIME in v138/B1).
// The artifact must be CONSUMED, or a client can replay one CoinGunSell and mint for the same prop
// forever -- the host removes nothing itself (by design: the client's own PropDestroy does that),
// so nothing else stops a repeat. `order_sync`, this lane's precedent, confirms its commit with an
// OrderCount()+1 edge before charging; this is the equivalent.
//
// TWO v137 DEFECTS FIXED HERE, both measured, both in this comment's predecessor:
//   1. IT WAS KEYED ON THE EID. Once B1 names the artifact by KEY, an eid key collapses onto
//      g_soldEid[0] for exactly the props this lane exists for (a v122 client's own save-loaded
//      keyed prop has eid 0), so the FIRST client sale would poison the guard against EVERY later
//      one -- a free denial of the whole feature. The key is now the artifact NAME: the save key
//      when there is one, "#<eid>" for the keyless families, which is unique in each domain.
//   2. IT CLAIMED TO SELF-CLEAN AND WAS ERASED NOWHERE. The old comment said the map "self-cleans:
//      once that prop dies and the eid is recycled onto a different actor...". `[V]` It was written
//      at :393 and read at :317 and never erased at all, and the recycle it leaned on cannot happen
//      in a real session anyway -- registry.cpp:177 pushes a freed id to the FRONT while Alloc pops
//      the BACK, deferring reuse by ~28k allocations. So the map only ever grew, and a reconnecting
//      client carried a stale sold-set into a new world. It is erased in TWO places now: the ~1 Hz
//      host sweep in Tick() drops entries whose prop has died (a CachedObjRef reads dead as null,
//      including "the world moved on" -- CLAUDE.md 4j), and OnDisconnect clears it whole.
// The value is a world-stamped CachedObjRef, not a raw pointer: the guard compares IDENTITY, and a
// raw pointer to a freed actor can be matched by a recycled allocation.
std::unordered_map<std::wstring, ue_wrap::CachedObjRef> g_sold;

// The artifact's NAME: the save key when the prop has one, the eid otherwise.
//
// BOTH FORMS ARE PREFIXED BY US (item 11, v140). The previous version returned the key VERBATIM and
// justified it with "a save key never starts with '#'" -- which is a fact about SAVE KEYS being
// applied to an ATTACKER-SUPPLIED STRING. A client is free to send key="#5" and collide its artifact
// name with the keyless form for eid 5, which is a consumption-guard collision: one sale poisons the
// guard for an unrelated prop, or launders a replay past it. The generator, not the input, is what
// has to make the namespaces disjoint, so the key form now carries its own prefix and no wire string
// can reach the eid namespace at all.
std::wstring ArtifactName(const std::wstring& key, uint32_t eid) {
    if (!key.empty()) return L"k:" + key;
    return L"#" + std::to_wstring(static_cast<unsigned long>(eid));
}

}  // namespace

// ---- the four reads the COLLECT lane shares (declared in coingun_internal.h) -------------------
namespace internal {

coop::net::Session* Session() { return LoadSession(); }

bool IsCoinActor(void* actor) {
    if (!actor) return false;
    if (g_coinClass) return R::ClassOf(actor) == g_coinClass;
    return R::ClassNameOf(actor) == kCoinClassName;   // pre-resolution fallback
}

// Am I inside THIS verb? The ONE ambient-window read either lane makes -- see vm_dispatch.h's
// contract box for why it is the NAME and not the id or `active`. Pointer-compares first because
// every caller passes a literal this module registered (RegisterVirtualVerb requires static
// lifetime, so the pointers are identical), then falls back to a compare for robustness.
bool InVerb(const vm::ActiveVerb& av, const wchar_t* name) {
    if (!av.active || !av.verbName) return false;
    return av.verbName == name || std::wcscmp(av.verbName, name) == 0;
}

void*   CoinClass()        { return g_coinClass; }
int32_t CoinPointsOffset() { return g_offCoinPoints; }

bool IsCapturedCoin(void* coin) {
    if (!coin) return false;
    std::lock_guard<std::mutex> lk(g_pendingMu);
    for (const auto& shot : g_pendingShots)
        for (const auto& ref : shot.coins)
            if (ref.Get() == coin) return true;
    return false;
}

}  // namespace internal

namespace {

using internal::IsCoinActor;
using internal::InVerb;

// ---- 1. COIN BIRTH (client) --------------------------------------------------------------------
// Func-thunk POST on FinishSpawningActor. Fires MID-BYTECODE inside the gun's still-open bracket:
// READS ONLY. See THE BARRIER RULE in the header -- an engine call here corrupts, and `sell` also
// applies an impulse to this very coin after Finish returns.
void OnFinishSpawnPost(void* /*context*/, void* /*sourceObject*/, void* spawned) {
    if (!IsCoinActor(spawned)) return;

    auto* s = LoadSession();
    const bool isClient = s && s->connected() && s->role() == coop::net::Role::Client;
    if (!isClient) return;                       // the HOST's coins are the real ones -- never touch

    const vm::ActiveVerb av = vm::CurrentThreadVerb();
    const bool inGunVerb = InVerb(av, kVerbNameGunUse);

    if (inGunVerb) {
        {
            std::lock_guard<std::mutex> lk(g_pendingMu);
            // Defensive: the verb ENTRY callback opens the group, but if a coin somehow reaches us
            // with no group open we open one rather than dropping the capture on the floor.
            if (g_pendingShots.empty()) g_pendingShots.emplace_back();
            g_pendingShots.back().coins.emplace_back();
            g_pendingShots.back().coins.back().Set(spawned);   // a read + a stamp; no dispatch
        }
        g_capturedCoins.fetch_add(1, std::memory_order_relaxed);
        UE_LOGI("coingun[client birth]: captured coin %p from our own shot -- held for the barrier, "
                "which destroys it ONLY if this shot actually authored a sale (v140)", spawned);
        return;
    }

    // Not our shot. A wire materialization of a host coin is legitimate and common; anything else is
    // a FAIL-OPEN and must be loud, because the two gates key on DIFFERENT things (this one on the
    // VERB, the collect cancel on MIRROR-NESS), so a wrong or unresolved verb name fails them
    // INDEPENDENTLY and would leave a client-local coin neither destroyed nor cancelled -- crediting
    // locally, in silence.
    if (coop::world_actor_sync::IsMaterializingMirror()) return;
    if (coop::world_load_episode::InEpisode()) return;   // belt: world-rebuild churn is not a signal

    g_anomalyBirths.fetch_add(1, std::memory_order_relaxed);
    UE_LOGE("coingun[ANOMALY]: a baocoin_C was born on this CLIENT outside BOTH the gun verb bracket "
            "and a mirror materialization (actor=%p). Either the verb name stopped resolving (check "
            "vm_dispatch stats) or a producer we never censused exists. This coin will credit LOCALLY "
            "and diverge.", spawned);
}

// ---- 3. THE GUN VERB ---------------------------------------------------------------------------
// The COLLECT verb (`actionOptionIndex`) has its own registration and its own callback in
// coingun_collect.cpp -- `vm_dispatch` is one callback per NAME, and the names differ, so neither
// lane needs the other's entry point.
void OnVerbEntry(const vm::Bracket& b) {
    // v140: OPEN A SHOT GROUP. The birth seam appends to it and the destroy seam marks it authored;
    // the barrier then destroys the group's coins only if a sale really went out. Before this the
    // entry callback did nothing at all and the capture was unconditional -- see THE BARRIER QUEUE.
    //
    // THE CTX GATE IS NOT OPTIONAL HERE, for the same reason it is not optional in IsInCoinGunVerb:
    // `vm_dispatch` matches on the verb NAME and `playerHandUse_LMB` is declared by 146 classes, so
    // without it every knife swing, hacksaw cut and flamethrower burst in the game would open (and
    // then release) an empty group -- turning the barrier's release WARNING into a false alarm on
    // every left click. Matching the gate the capture itself uses also keeps the two from drifting.
    auto* s = LoadSession();
    if (!s || !s->connected() || s->role() != coop::net::Role::Client) return;
    if (!b.ctx) return;
    if (!g_gunClass) g_gunClass = R::FindClass(kGunClassName);
    if (!g_gunClass || R::ClassOf(b.ctx) != g_gunClass) return;
    std::lock_guard<std::mutex> lk(g_pendingMu);
    g_pendingShots.emplace_back();
}

// ---- host helpers ------------------------------------------------------------------------------

// `[V]` prop_coingun's ubergraph statement [4], read this session off research/bp_reflection:
//     K2Node_Event_player->CALLVIRT arm(1000.0, out arm_start, out arm_end, out arm_rotation)
//     LineTraceSingleForObjects(self, arm_start, arm_end, ...)
// The reach is 1000 uu and the trace originates AT THE PLAYER, not at the gun -- which is exactly
// why the sender's own puppet is the right thing to measure against.
constexpr float kGunReachUU = 1000.0f;

// The pose-staleness budget. The host's copy of a client's body is behind reality by the one-way
// latency + RemotePlayer::kInterpWindowMs (75 ms) + one send interval, and the native trace starts
// at the player's ARM, which sits above the actor root that GetActorLocation reports. 600 uu covers
// roughly half a second at a sprint plus the eye-height offset. Deliberately generous: this gate
// exists to stop WORLD-WIDE enumeration, not to adjudicate centimetres, and a false refusal costs a
// real player a real sale. Widening it does not re-open A50 -- the arbiter's own destroy does that
// work; this bound only decides how far a peer may reach to spend the world's props.
constexpr float kPoseStalenessUU = 600.0f;

// A50 (2026-08-25). Is the named prop within the SENDER'S OWN reach, measured entirely on the HOST's
// copies of both bodies? Before this existed, `senderSlot` occurred in this file only as a reply
// address and a printf argument: the receiver asked five questions about the ARTIFACT and none about
// the ACTOR, so any peer could name any prop in the world.
//
// MTA does exactly this for a write it did not witness --
// `reference/mtasa-blue/Server/mods/deathmatch/logic/CUnoccupiedVehicleSync.cpp:244` and `:491`:
// `IsPointNearPoint3D(vecVehiclePosition, pPlayer->GetPosition(), fMaxDistance)`. Same shape, same
// reason, same place in the flow (before the state change is honoured, not after).
//
// FAIL-CLOSED: no live puppet on the host means there is no body to measure a reach from, so the
// answer is no. A joining client whose puppet has not spawned yet is refused and TOLD (TooFarAway),
// which is the principle-8 answer for this lane: the sale is the client's to retry, its own prop is
// gone either way, and inventing a reach for a body we cannot see is precisely the enumeration hole.
bool SenderMayReach(uint8_t senderSlot, void* prop, float& outDist, float& outAllowed) {
    outDist = outAllowed = -1.f;
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(senderSlot);
    void* puppet = (rp && rp->valid()) ? rp->GetActor() : nullptr;
    if (!puppet) return false;

    const ue_wrap::FVector body = E::GetActorLocation(puppet);

    // The prop's ORIGIN is not its surface, and the native trace stops at whatever it HITS -- so a
    // large prop is legitimately sellable from further away than reach-from-origin. Measure the real
    // bounds instead of inventing a constant fudge for "big things".
    ue_wrap::FVector origin{}, extent{};
    float propRadius = 0.f;
    if (E::GetActorBounds(prop, /*onlyColliding=*/true, origin, extent)) {
        propRadius = std::sqrt(extent.X * extent.X + extent.Y * extent.Y + extent.Z * extent.Z);
    } else {
        origin = E::GetActorLocation(prop);
    }

    const float dx = origin.X - body.X, dy = origin.Y - body.Y, dz = origin.Z - body.Z;
    outDist    = std::sqrt(dx * dx + dy * dy + dz * dz);
    outAllowed = kGunReachUU + propRadius + kPoseStalenessUU;
    return outDist <= outAllowed;
}

void* FindLiveGun() {
    // `[V]` `sell` reads ZERO gun state and positions coins from the SOLD PROP's component, so ANY
    // live instance behaves identically -- there is nothing to prefer.
    // C-3 (audit): "once per sale" is NOT cold enough -- the sale rate is attacker-controlled, and
    // FindObjectByClass is a full GUObjectArray walk. Cache it and walk only on a miss.
    if (void* cached = g_gunRef.Get()) return cached;
    // A51 (v140): the POSITIVE result was cached and the NEGATIVE one was not, so a world with no gun
    // in it re-walked GUObjectArray on every single sale -- and `[V]` prop_coingun is placed in only
    // 3 of 261 maps, so "no gun" is the ordinary world, not the exotic one. Throttle the miss. 250 ms
    // collapses a packet burst (the reliable inbox drains unbounded per tick) to one walk while
    // staying far below the time it takes a human to buy a gun from the laptop and fire it, so a real
    // sale is never refused for want of a re-walk.
    using Clock = std::chrono::steady_clock;
    static Clock::time_point sLastMiss{};
    const Clock::time_point now = Clock::now();
    if (sLastMiss.time_since_epoch().count() != 0 &&
        now - sLastMiss < std::chrono::milliseconds(250)) {
        return nullptr;
    }
    void* found = R::FindObjectByClass(kGunClassName);
    if (found) g_gunRef.Set(found);
    else       sLastMiss = now;
    return found;
}

// ---- the host's ANSWER (v138 B1) ---------------------------------------------------------------
// Every sale gets one, success or refusal. A refusal that says nothing renders on the seller's
// screen as the exact bug the user reported: their prop vanishes (its own destroy is unchanged and
// still lands) and no coins appear. See the header's THE RESULT SENTENCE.
void SendResult(coop::net::Session* s, uint8_t slot, coop::net::CoinGunResultCode code,
                int32_t points) {
    if (!s) return;
    coop::net::CoinGunResultPayload r{};
    r.code   = static_cast<uint8_t>(code);
    r.points = points;
    s->SendReliableToSlot(static_cast<int>(slot), coop::net::ReliableKind::CoinGunResult, &r,
                          sizeof(r));
}

// Refuse + answer + count, in one place so a new refusal path cannot silently skip the sentence.
//
// RETURNS whether this refusal should be LOGGED (item 7, v140). Every refusal path here is reachable
// from the trust boundary at whatever rate a sender likes, and each one carried an unconditional
// UE_LOGW -- so one peer could fill the log faster than anything else in it could be read, which is
// the same denial the A51 walk was, one layer up. The R-1e latch shape from prop_destroy_seam: the
// first five, then every tenth to a hundred, then every hundredth. PER CODE, so a flood of
// no-such-prop cannot hide the first not-sellable.
bool Refuse(coop::net::Session* s, uint8_t slot, coop::net::CoinGunResultCode code) {
    g_salesRefused.fetch_add(1, std::memory_order_relaxed);
    SendResult(s, slot, code, 0);
    static uint32_t sSeen[8] = {};
    const unsigned idx = static_cast<unsigned>(code) < 8u ? static_cast<unsigned>(code) : 0u;
    const uint32_t n = ++sSeen[idx];
    return n <= 5 || (n <= 100 && n % 10 == 0) || n % 100 == 0;
}

const wchar_t* ResultText(coop::net::CoinGunResultCode code) {
    switch (code) {
        case coop::net::CoinGunResultCode::Sold:         return L"sold it";
        case coop::net::CoinGunResultCode::NoSuchProp:
            return L"could not sell that: the host does not have it";
        case coop::net::CoinGunResultCode::AlreadySold:
            return L"could not sell that: it was already sold";
        case coop::net::CoinGunResultCode::NoGun:
            return L"could not sell that: no coin gun exists in the host's world";
        case coop::net::CoinGunResultCode::NotSellable:
            return L"could not sell that: the host's store will not take it";
        case coop::net::CoinGunResultCode::HostInternal:
            return L"could not sell that: the host hit an internal error";
        case coop::net::CoinGunResultCode::TooFarAway:
            return L"could not sell that: the host does not see you next to it";
    }
    return L"could not sell that";
}

}  // namespace

void PrepareCoinMirror(void* coin) {
    // I-5 (audit 2026-08-24). The first draft called E::SetActorSimulatePhysics, which applies to the
    // ROOT component -- and `[V]` Abaocoin_C declares `collect` (USphereComponent) FIRST, `baocoin`
    // (mesh) second, and `Sphere` (the one shipping bSimulatePhysics=True) THIRD. The BP root is
    // conventionally the first-declared component, so that call would have logged physics-off=1 while
    // `Sphere` kept simulating and fought the pose drive. Target the component BY NAME instead: a
    // UActorComponent is a UObject, so it takes a normal reflected call.
    if (!coin) return;
    if (g_offCoinSphere < 0 && g_coinClass)
        g_offCoinSphere = R::FindPropertyOffset(g_coinClass, L"Sphere");
    if (g_offCoinSphere < 0) {
        UE_LOGW("coingun[mirror]: coin %p -- 'Sphere' offset unresolved, cannot stop the mirror "
                "simulating; it may drift from the host's authoritative pose", coin);
        return;
    }
    void* sphere = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(coin) + g_offCoinSphere);
    if (!sphere) return;
    // RESOLVE ON THE DECLARING CLASS (item 10, v140). This used to ask
    // `FindFunction(ClassOf(sphere), L"SetSimulatePhysics")`, and `[V]` that could never succeed:
    // `R::FindFunction` matches `OuterOf(obj) == owningClass` EXACTLY -- it does not climb
    // SuperStruct (reflection.cpp:468-479) -- while `[V]` `SetSimulatePhysics` is declared on
    // UPrimitiveComponent (CXXHeaderDump/Engine.hpp:17349, inside `class UPrimitiveComponent : public
    // USceneComponent`) and the component here is a USphereComponent. Worse, FindFunction's miss is a
    // FULL GUObjectArray walk, and this runs once per mirrored coin -- ~47 per sale -- so the failure
    // was not merely silent, it was the most expensive thing in the lane. Ask the class that actually
    // owns the function, and latch the negative so a future resolve failure costs one walk, not one
    // per coin forever. (The general "FindFunction should walk SuperStruct" item stays on the backlog;
    // changing it globally would alter identity semantics at every other call site.)
    static bool sSetSimResolveFailed = false;
    if (!g_setSimFn && !sSetSimResolveFailed) {
        if (void* primCls = R::FindClass(L"PrimitiveComponent"))
            g_setSimFn = R::FindFunction(primCls, L"SetSimulatePhysics");
        if (!g_setSimFn) {
            sSetSimResolveFailed = true;
            UE_LOGW("coingun[mirror]: SetSimulatePhysics unresolved on UPrimitiveComponent -- mirrors "
                    "will keep simulating and may drift from the host's pose. Latched: this walk is "
                    "not repeated per coin.");
        }
    }
    if (!g_setSimFn) return;
    ue_wrap::ParamFrame f(g_setSimFn);
    if (!f.valid()) return;
    f.Set<bool>(L"bSimulate", false);
    const bool ok = ue_wrap::Call(sphere, f);
    UE_LOGI("coingun[mirror]: coin %p -- Sphere(%p) SetSimulatePhysics(false) dispatch=%d (a pose-driven "
            "mirror must not also simulate)", coin, sphere, ok ? 1 : 0);
}

bool IsInCoinGunVerb() {
    const vm::ActiveVerb av = vm::CurrentThreadVerb();
    if (!InVerb(av, kVerbNameGunUse)) return false;
    // THE CORRECTNESS GATE (C-1, audit 2026-08-24). `vm_dispatch` matches on the verb NAME alone and
    // says so: "any further class/authority discrimination is the CONSUMER's job". `playerHandUse_LMB`
    // is declared by 146 classes in the CXX dump -- prop_knife, prop_hacksaw, prop_flamethrower,
    // prop_garbageGun, prop_arirDisint, prop_toolgun... Without this check a client destroying a keyed
    // prop with ANY of them would author a sale and the host would MINT COINS FOR IT: a free-money
    // path in ordinary play, manufacturing the very defect this lane exists to close. The header
    // promised this check from the first draft and the code did not have it, which is
    // `[[lesson-false-security-comment-worse-than-none]]` in its purest form.
    if (!av.ctx) return false;
    if (!g_gunClass) g_gunClass = R::FindClass(kGunClassName);
    return g_gunClass && R::ClassOf(av.ctx) == g_gunClass;
}

void SendSaleForDyingProp(const std::wstring& key, uint32_t elementId) {
    auto* s = LoadSession();
    if (!s || !s->connected()) return;
    if (s->role() != coop::net::Role::Client) return;      // the host's own sale needs no wire
    const uint32_t eid =
        (elementId == static_cast<uint32_t>(coop::element::kInvalidId)) ? 0u : elementId;
    if (key.empty() && eid == 0u) {
        // B1: this is the ONLY remaining "nothing to name" case, and it is genuinely empty -- a
        // keyless prop with no element row is unnameable in BOTH identity domains. v137 reached
        // this branch for every ordinary keyed prop because it only ever looked at the eid.
        UE_LOGW("coingun[client sale]: the gun's victim has neither a save key nor an element id -- "
                "nothing to name, so no sale is authored (the destroy still goes, i.e. today's "
                "behaviour). If this line appears for an ordinary keyed prop, the destroy seam's "
                "key read is the defect, not this lane.");
        return;
    }
    coop::net::CoinGunSellPayload p{};
    p.key.len = 0;
    for (size_t i = 0; i < key.size() && i < sizeof(p.key.data); ++i)
        p.key.data[p.key.len++] = static_cast<char>(key[i]);
    p.elementId = eid;
    s->SendReliable(coop::net::ReliableKind::CoinGunSell, &p, sizeof(p));
    g_salesSent.fetch_add(1, std::memory_order_relaxed);
    // v140: THIS is what authorizes the barrier to destroy the coins this shot spawned. It is set
    // only after the send actually happened, on the group this shot opened -- so every path that
    // returns before here (all four of them) leaves its coins to be RELEASED, not eaten.
    {
        std::lock_guard<std::mutex> lk(g_pendingMu);
        if (!g_pendingShots.empty()) g_pendingShots.back().authored = true;
    }
    UE_LOGI("coingun[client sale]: sent CoinGunSell(key='%ls' eid=%u) -- rides IN FRONT of our own "
            "unchanged PropDestroy on this lane, so the host mints while its copy is still alive",
            key.empty() ? L"None" : key.c_str(), eid);
}

void Tick() {
    // THE HOST HALF: erase consumed artifacts whose prop has died. This is what gives the
    // consumption guard a real lifetime -- v137's comment CLAIMED the map self-cleaned while the
    // map was erased nowhere at all, so it only ever grew. A CachedObjRef reads dead as null,
    // including "the world moved on" (CLAUDE.md 4j), so this is exactly the liveness test the guard
    // itself uses. Throttled to ~1 Hz at the 125 Hz pump rate; the map holds only props that were
    // sold and have not died yet, which is normally zero or one entry.
    if (!g_sold.empty()) {
        static uint32_t sSweepN = 0;
        if ((sSweepN++ % 125u) == 0u) {
            size_t dropped = 0;
            for (auto it = g_sold.begin(); it != g_sold.end();) {
                if (it->second.Get() == nullptr) { it = g_sold.erase(it); ++dropped; }
                else                             { ++it; }
            }
            if (dropped)
                UE_LOGI("coingun[sold-set]: swept %zu consumed artifact(s) whose prop is gone "
                        "(%zu still live)", dropped, g_sold.size());
        }
    }

    // THE BARRIER, v140: commit-or-release, per shot. The bracket that opened each group completed
    // synchronously on this same thread before we got here, so `authored` is final by now.
    std::vector<PendingShot> shots;
    {
        std::lock_guard<std::mutex> lk(g_pendingMu);
        if (g_pendingShots.empty()) return;
        shots.swap(g_pendingShots);
    }
    size_t destroyed = 0, released = 0, releasedShots = 0;
    for (auto& shot : shots) {
        if (!shot.authored) {
            // NO SALE WENT OUT for this shot, so there is no authoritative coin to defer to. Leave
            // ours alone: they credit locally, which is exactly single-player behaviour. Destroying
            // them here is what made the four principle-8 paths cost the player the prop AND the
            // coins AND any explanation.
            released += shot.coins.size();
            ++releasedShots;
            continue;
        }
        for (auto& ref : shot.coins) {
            void* actor = ref.Get();             // null if dead, or if the world moved on
            if (!actor) continue;
            E::DestroyActor(actor);
            ++destroyed;
            g_barrierDestroyed.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (released) {
        UE_LOGW("coingun[barrier]: RELEASED %zu coin(s) from %zu shot(s) that authored NO sale -- they "
                "stay in the world and credit locally, which is what single-player does. If this "
                "fires, the sale was blocked upstream (world-load episode / reconcile window / kerfur "
                "capture / no name to send) and the player keeps their coins instead of losing both "
                "the prop and the payment.", released, releasedShots);
    }
    if (destroyed) {
        UE_LOGI("coingun[barrier]: destroyed %zu of our own client-side coins across %zu authored "
                "shot(s) (captured=%llu, total destroyed=%llu)", destroyed,
                shots.size() - releasedShots,
                g_capturedCoins.load(std::memory_order_relaxed),
                g_barrierDestroyed.load(std::memory_order_relaxed));
    }
}

void OnReliable(const uint8_t* payload, int len, uint8_t senderSlot) {
    auto* s = LoadSession();
    if (!s || s->role() != coop::net::Role::Host) {
        UE_LOGW("coingun[host]: CoinGunSell received off the HOST -- dropping");
        return;
    }
    if (!payload || len < static_cast<int>(sizeof(coop::net::CoinGunSellPayload))) {
        UE_LOGW("coingun[host]: CoinGunSell payload too small (len=%d) -- dropping", len);
        return;
    }
    coop::net::CoinGunSellPayload p{};
    std::memcpy(&p, payload, sizeof(p));

    // --- read the artifact's NAME off the wire ----------------------------------------------------
    // WireKey's contract is that bytes beyond `len` are zero, but a hostile sender is not bound by a
    // contract: clamp the length and stop at the first NUL rather than trusting either.
    std::wstring keyStr;
    {
        const uint8_t klen = (p.key.len <= sizeof(p.key.data))
                                 ? p.key.len
                                 : static_cast<uint8_t>(sizeof(p.key.data));
        for (uint8_t i = 0; i < klen; ++i) {
            const char ch = p.key.data[i];
            if (ch == '\0') break;
            keyStr.push_back(static_cast<wchar_t>(static_cast<unsigned char>(ch)));
        }
    }
    const std::wstring artifact = ArtifactName(keyStr, p.elementId);

    // --- eid RANGE trust (item 6, v140) -----------------------------------------------------------
    // The `IsAllowedHostAllocatedEid` idiom is obeyed at 14 other receive sites and was missing here,
    // while the header claimed this receiver "fully range-checks the payload" -- a sentence deleted
    // rather than softened when it was measured false. EITHER range is accepted for the same reason
    // the PropDestroy receiver accepts either (event_dispatch_entity.cpp): a sale REFERENCES an
    // existing shared entity rather than allocating one, and the keyless families can carry an eid
    // allocated by the peer that first expressed them. A genuinely invalid id (0 / kInvalidId / out
    // of both ranges) never reaches the resolver.
    if (p.elementId != 0u && p.elementId != coop::element::kInvalidId &&
        !coop::element::Registry::IsAllowedHostAllocatedEid(p.elementId) &&
        !coop::element::Registry::IsAllowedPeerAllocatedEid(p.elementId)) {
        const bool logIt = Refuse(s, senderSlot, coop::net::CoinGunResultCode::NoSuchProp);
        if (logIt)
            UE_LOGW("coingun[host]: REFUSED slot=%u artifact='%ls' -- REASON=eid-out-of-range "
                    "(0x%08x is in neither the host nor the peer allocation band). Nothing that names "
                    "an id we could not have issued gets as far as the resolver.",
                    senderSlot, artifact.c_str(), p.elementId);
        return;
    }

    // --- resolve the artifact against OUR OWN world -----------------------------------------------
    // KEY FIRST. This is the whole of B1: v137 resolved the eid alone, and `[V]` a v122 client mints
    // no Element row for its own save-loaded keyed prop, so the eid it sent was 0 for exactly the
    // props a player shoots -- 3 of 3 field sales died right here. The key is the identity that
    // survives, and the eid stays as the keyless fallback exactly as the PropDestroy receiver does.
    //
    // INDEX ONLY (A51, v140). This used to call `ResolveLiveActorByKey`, whose cold fallback is a
    // full `ue_wrap::prop::FindByKeyString` GUObjectArray walk with a key-string read per prop -- and
    // an attacker-chosen key is a GUARANTEED miss, so every hostile packet bought one whole walk, on
    // the host's game thread, at whatever rate the sender likes (the reliable inbox drains with an
    // unbounded `while (TryGetReliable(msg))` per tick, so they compound inside one frame). The
    // mitigation was already written 200 lines above in this same file, applied to the OTHER
    // attacker-steered walk (FindLiveGun): cache, and walk only on a miss. Here the right answer is
    // stronger -- do not walk at all. A key that is not in the maintained index is not a prop this
    // peer can legitimately have shot: the client only KNOWS the key because it mirrored the prop
    // from us, which required us to hold a Prop Element for it, which is precisely what puts it in
    // the index. "Not indexed" is therefore already the NoSuchProp answer this site gives.
    void* prop = nullptr;
    const wchar_t* how = L"key";
    if (!keyStr.empty()) prop = coop::prop_element_tracker::FindLiveActorByKey(keyStr);
    if (!prop && p.elementId != 0u) {
        // The keyless fallback, for an Aprop_C whose Key genuinely reads None. It costs nothing: the
        // 4 bytes sit in alignment padding the payload needs regardless, and LivePropActor is an O(1)
        // Registry lookup, not a walk.
        //
        // (This comment used to assert the fallback is unreachable through the gun "because the
        // keyless families are not Aprop_C descendants", citing ue_wrap/actors/prop.cpp's union
        // predicate. That reasoning was wrong twice over and is corrected here rather than left
        // standing: `[V]` the gun's gate is NOT class descent -- prop_coingun's ubergraph @792 is
        // `BooleanAND(DoesImplementInterface(HitActor, int_player_C), NOT IsChildActor)` and
        // trashBitsPile_C / actorChipPile_C DO implement that interface and DO pass it; they die one
        // statement later at `IsValid(asProp_return)`, because `prop_C::asProp = EX_Self` is the only
        // non-null implementation among all 34 implementors. And our own union predicate can only
        // ever be evidence about our code, never about the game's class tree.)
        prop = coop::element::LivePropActor(static_cast<coop::element::ElementId>(p.elementId));
        how  = L"eid";
    }
    if (!prop) {
        const bool logIt = Refuse(s, senderSlot, coop::net::CoinGunResultCode::NoSuchProp);
        if (logIt) UE_LOGW("coingun[host]: REFUSED slot=%u artifact='%ls' (key='%ls' eid=%u) -- "
                "REASON=no-such-prop. NEITHER name resolves to a live prop in our world, so the two "
                "peers disagreed about this prop BEFORE anyone fired: a pre-existing stable-ID "
                "divergence this lane exposes, not one it causes. The client's own destroy still "
                "lands, so this degrades to pre-A37 behaviour (item lost, nothing credited) -- it "
                "does not manufacture a NEW loss, and the seller is TOLD.",
                senderSlot, artifact.c_str(), keyStr.empty() ? L"None" : keyStr.c_str(),
                p.elementId);
        return;
    }

    // --- THE AUTHORIZATION GATE (A50, v140) -------------------------------------------------------
    // FIRST question about the ACTOR, and it comes before every remaining question about the
    // artifact so that a peer probing keys learns nothing it did not already have. Everything above
    // this line is identity resolution; everything below it spends the world's props.
    {
        float dist = -1.f, allowed = -1.f;
        if (!SenderMayReach(senderSlot, prop, dist, allowed)) {
            const bool logIt = Refuse(s, senderSlot, coop::net::CoinGunResultCode::TooFarAway);
            if (logIt) UE_LOGW("coingun[host]: REFUSED slot=%u artifact='%ls' -- REASON=too-far-away "
                    "(dist=%.0f allowed=%.0f; -1 for both means the sender has no live puppet here, "
                    "so there is no body to measure a reach from and we refuse rather than assume "
                    "one). The gun `[V]` traces arm(1000.0) FROM THE PLAYER, so a prop outside that "
                    "reach was not shot -- naming it is enumeration, not a sale.",
                    senderSlot, artifact.c_str(), dist, allowed);
            return;
        }
    }

    // THE CONSUMPTION GUARD -- has this exact artifact already been minted for? (see g_sold)
    {
        auto it = g_sold.find(artifact);
        if (it != g_sold.end() && it->second.Get() == prop) {
            const bool logIt = Refuse(s, senderSlot, coop::net::CoinGunResultCode::AlreadySold);
            if (logIt) UE_LOGW("coingun[host]: REFUSED slot=%u artifact='%ls' -- REASON=already-sold. This prop "
                    "was already minted for and its destroy has not yet removed it. A replayed or "
                    "duplicated sale mints nothing.", senderSlot, artifact.c_str());
            return;
        }
    }

    void* gun = FindLiveGun();
    if (!gun) {
        const bool logIt = Refuse(s, senderSlot, coop::net::CoinGunResultCode::NoGun);
        if (logIt) UE_LOGW("coingun[host]: REFUSED slot=%u artifact='%ls' -- REASON=no-live-coingun. Nothing in "
                "our world can execute `sell` (the sender's hand mirror may have been stowed within "
                "the RTT). We mint nothing rather than inventing an instance -- `[V]` a CDO executor "
                "would mint ZERO coins anyway, since EX_Self is the WorldContextObject of every "
                "deferred spawn inside `sell`.", senderSlot, artifact.c_str());
        return;
    }

    // --- price it from OUR OWN copy ---------------------------------------------------------------
    if (!g_sellObjectFn || !g_libCdo) {
        const bool logIt = Refuse(s, senderSlot, coop::net::CoinGunResultCode::HostInternal);
        if (logIt) UE_LOGW("coingun[host]: REFUSED slot=%u artifact='%ls' -- REASON=sellObject-unresolved",
                senderSlot, artifact.c_str());
        return;
    }
    const std::wstring propNameStr = ue_wrap::prop::GetPropNameString(prop);
    const R::FName propName = ue_wrap::fname_utils::StringToFName(propNameStr);
    ue_wrap::ParamFrame f(g_sellObjectFn);
    if (!f.valid()) {
        const bool logIt = Refuse(s, senderSlot, coop::net::CoinGunResultCode::HostInternal);
        if (logIt) UE_LOGW("coingun[host]: REFUSED slot=%u artifact='%ls' -- REASON=sellObject-frame-invalid",
                senderSlot, artifact.c_str());
        return;
    }
    f.Set<R::FName>(L"object", propName);
    f.Set<bool>(L"onlyShop", true);              // the GUN passes TRUE -- match it exactly
    f.Set<void*>(L"objectToSell", prop);
    f.Set<void*>(L"__WorldContext", prop);
    if (!ue_wrap::Call(g_libCdo, f)) {
        const bool logIt = Refuse(s, senderSlot, coop::net::CoinGunResultCode::HostInternal);
        if (logIt) UE_LOGW("coingun[host]: REFUSED slot=%u artifact='%ls' -- REASON=sellObject-dispatch-failed",
                senderSlot, artifact.c_str());
        return;
    }
    const int32_t points = f.Get<int32_t>(L"Points");
    const bool    sold   = f.Get<bool>(L"sold");
    if (!sold) {
        // NEAR-UNREACHABLE BY CONSTRUCTION, and kept fail-closed anyway. `[V]` the refusal gate is
        // `row.price <= 1 && onlyShop` over `list_props` -- pure cooked TABLE data, byte-identical on
        // both peers -- so a client whose own `sell` refused never destroyed the prop, never entered
        // our destroy seam and never authored a sale at all. Reaching here means the two peers'
        // tables or prop NAMES disagree, which is a version/identity fault worth seeing, not a
        // routine outcome. (Refusal by price is very much routine in SP: measured over the shipped
        // list_props, every loose-garbage row -- ntrash*, g3_bag_*, garbBin2, garbContainer,
        // trashClump -- prices at 1 and is refused, so shooting garbage does nothing on ANY peer.)
        const bool logIt = Refuse(s, senderSlot, coop::net::CoinGunResultCode::NotSellable);
        if (logIt) UE_LOGW("coingun[host]: REFUSED slot=%u artifact='%ls' -- REASON=not-sellable (sellObject said "
                "sold=0 for name='%ls'). Our prop is left UNMUTATED; the client's destroy still "
                "lands, exactly as today. If this fires, the peers disagree about this prop's name "
                "or about list_props itself.", senderSlot, artifact.c_str(), propNameStr.c_str());
        return;
    }

    // --- mint through the game's own verb ---------------------------------------------------------
    // `comp` is the SOLD PROP's static mesh -- `[V]` `sell` does DynamicCast<UStaticMeshComponent>(comp)
    // and `rnd(c, comp)` derives the spawn location from it. Aprop_C::StaticMesh (prop.hpp:9).
    if (g_offPropMesh < 0) g_offPropMesh = R::FindPropertyOffset(R::ClassOf(prop), L"StaticMesh");
    void* meshComp = (g_offPropMesh >= 0)
        ? *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(prop) + g_offPropMesh)
        : nullptr;
    if (!g_sellFn) g_sellFn = R::FindFunction(R::ClassOf(gun), L"sell");   // M-5: cache, not per sale
    void* sellFn = g_sellFn;
    if (!sellFn) {
        const bool logIt = Refuse(s, senderSlot, coop::net::CoinGunResultCode::HostInternal);
        if (logIt) UE_LOGW("coingun[host]: REFUSED slot=%u artifact='%ls' -- REASON=sell-unresolved", senderSlot,
                artifact.c_str());
        return;
    }
    ue_wrap::ParamFrame sf(sellFn);
    if (!sf.valid()) {
        const bool logIt = Refuse(s, senderSlot, coop::net::CoinGunResultCode::HostInternal);
        if (logIt) UE_LOGW("coingun[host]: REFUSED slot=%u artifact='%ls' -- REASON=sell-frame-invalid",
                senderSlot, artifact.c_str());
        return;
    }
    sf.Set<int32_t>(L"Index", points);           // `[V]` `Index` IS the price
    sf.Set<void*>(L"comp", meshComp);
    const bool ok = ue_wrap::Call(gun, sf);
    if (!ok) {
        // The dispatch itself failed, so nothing was minted -- do NOT consume the artifact, and do
        // NOT tell the seller it sold. v137 stamped the sold-set unconditionally right here.
        const bool logIt = Refuse(s, senderSlot, coop::net::CoinGunResultCode::HostInternal);
        if (logIt) UE_LOGE("coingun[host]: REFUSED slot=%u artifact='%ls' -- REASON=sell-dispatch-failed "
                "(gun=%p comp=%p price=%d). No coins were minted and the artifact is NOT consumed.",
                senderSlot, artifact.c_str(), gun, meshComp, points);
        return;
    }
    g_sold[artifact].Set(prop);      // consume the artifact (see THE CONSUMPTION GUARD)
    SendResult(s, senderSlot, coop::net::CoinGunResultCode::Sold, points);

    // --- THE ARBITER CONSUMES WHAT IT PAID FOR (A50, v140) ----------------------------------------
    // `[V]` the sold prop's destroy lives in the GUN's ubergraph at [26] (`HitActor->K2_DestroyActor`,
    // right after [24] `self->sell(...)`), NOT inside `sell` -- so calling `sell` alone mints coins
    // and leaves the prop standing. Until v140 this half of the transaction was delegated to the
    // client's own PropDestroy arriving behind us on the same lane, and the success log below said so
    // out loud: a cost paid on the counterparty's honour is not a transaction. An attacker simply
    // omitted the destroy.
    //
    // We run it HERE, in the native order (sell, then destroy), on the host's own copy. Nothing new
    // has to broadcast it: E::DestroyActor dispatches Actor.K2_DestroyActor, which is the exact
    // UFunction prop_destroy_seam Func-patches, so DestroySeamBody sees this destroy and sends the
    // ordinary PropDestroy to every peer -- one owner, one mechanism. (SendSaleForDyingProp inside
    // that seam is a no-op for us: it returns immediately off a Client role, and we are the host.)
    // The seller's own PropDestroy then arrives at a host that no longer resolves the prop and lands
    // as the steady-state no-op OnDestroyImpl_ already implements for an echo.
    //
    // This also makes the consumption guard's job much smaller: g_sold now only has to survive the
    // window between this destroy and the actor actually leaving the key index, i.e. a replay landing
    // in the SAME drained batch. It stays because that window is real -- the reliable inbox drains
    // unbounded per tick, so two CoinGunSell packets in one frame is the cheapest thing an attacker
    // can do.
    E::DestroyActor(prop);

    // The coins the mint spawns are EX_CallMath BeginDeferreds from the gun's bytecode, so
    // npc_world_enum's source-gated Func thunk catches them and drains to
    // world_actor_sync::HostEnrollExSpawn -- that is what allocates each eid and broadcasts
    // WorldActorSpawn. Nothing here has to enroll anything.
    UE_LOGI("coingun[host]: SOLD slot=%u artifact='%ls' (resolved by %ls) name='%ls' price=%d -> "
            "minted via gun=%p comp=%p, and WE destroyed the prop ourselves (v140 A50: the arbiter "
            "performs the whole transaction; the destroy rides the ordinary K2_DestroyActor seam out "
            "to every peer). Coins are HOST-OWNED; whoever's body trips one credits the host.",
            senderSlot, artifact.c_str(), how, propNameStr.c_str(), points, gun, meshComp);
}

void OnReliableResult(const uint8_t* payload, int len) {
    auto* s = LoadSession();
    if (!s || s->role() == coop::net::Role::Host) {
        UE_LOGW("coingun[client]: CoinGunResult received on the HOST -- dropping");
        return;
    }
    if (!payload || len < static_cast<int>(sizeof(coop::net::CoinGunResultPayload))) {
        UE_LOGW("coingun[client]: CoinGunResult payload too small (len=%d) -- dropping", len);
        return;
    }
    coop::net::CoinGunResultPayload r{};
    std::memcpy(&r, payload, sizeof(r));
    const auto code = static_cast<coop::net::CoinGunResultCode>(r.code);

    std::wstring line;
    if (code == coop::net::CoinGunResultCode::Sold) {
        // The PRICE, not a bare ack. `[V]` getPriceMultiplier is per-instance and divergent
        // (prop_batts by energy, prop_food by uses/ripeness, prop_cementBag, prop_garbBagRoll), so
        // the toast this player's own local `sell` just printed can legitimately name a DIFFERENT
        // number than the host actually minted. Saying the host's number makes that visible.
        line = L"sold it for " + std::to_wstring(static_cast<long long>(r.points)) +
               L" points (the host's price)";
    } else {
        line = ResultText(code);
    }
    // AnnounceDirect, not Announce: this is functional feedback about the player's own action, not
    // cosmetic ambience, so the ui.chat.peer_actions toggle must not be able to hide it (the
    // order_sync refusal takes the same seam for the same reason).
    coop::peer_action_feed::AnnounceDirect(
        static_cast<uint8_t>(coop::players::Registry::Get().LocalPeerId()), line);
    UE_LOGI("coingun[client]: CoinGunResult code=%u points=%d -- '%ls'",
            static_cast<unsigned>(r.code), r.points, line.c_str());
}

void OnDisconnect() {
    // The free measurement: a session that ends having measured nothing is the failure
    // `[[lesson-your-own-session-end-summary-is-a-free-measurement]]` exists to prevent. One grep
    // of this line answers "did this lane do anything at all this run" without reading the body.
    UE_LOGI("coingun[sale]: SESSION SUMMARY -- sales{sent=%llu refused=%llu} coins{captured=%llu "
            "barrierDestroyed=%llu anomalyBirths=%llu} soldSet=%zu pendingShots=%zu",
            g_salesSent.load(std::memory_order_relaxed),
            g_salesRefused.load(std::memory_order_relaxed),
            g_capturedCoins.load(std::memory_order_relaxed),
            g_barrierDestroyed.load(std::memory_order_relaxed),
            g_anomalyBirths.load(std::memory_order_relaxed),
            g_sold.size(), g_pendingShots.size());
    internal::OnDisconnectCollect();   // the collect lane dumps its own half

    // Every WORLD-scoped thing goes. v137 had no OnDisconnect at all, so a reconnecting peer carried
    // a stale sold-set and a pending-kill list of actors belonging to a dead world into the new one.
    // The resolved UClass / UFunction / CDO pointers are NOT world-scoped and stay (CLAUDE.md 4j);
    // the counters stay monotonic on purpose, so the summary above spans the whole process.
    g_sold.clear();
    {
        std::lock_guard<std::mutex> lk(g_pendingMu);
        g_pendingShots.clear();
    }
    g_gunRef = ue_wrap::CachedObjRef{};
}

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    // BOTH lanes must be done before this stops running. The two have DIFFERENT dependencies -- the
    // sale lane needs FinishSpawningActor + sellObject, the collect lane the coin's overlap BndEvt --
    // so either can resolve first, and gating on the sale lane's latch alone would silently strand
    // the collect lane forever on any tick where its own resolve had not landed yet.
    if (g_installed.load(std::memory_order_acquire) && internal::CollectInstalled()) return;
    // C-4 (audit 2026-08-24). This runs at the 125 Hz pump rate, and every resolve below is a linear
    // GUObjectArray walk with a name render per entry. The g_installed latch only helps AFTER success;
    // in a world where baocoin_C's UClass is not resident (it loads on demand with the gun asset) this
    // would burn up to five full walks per tick, forever, for every player in every session. Bound the
    // retry to ~1 Hz -- the same shape wisp_attack_sync.cpp:277 already carries, added there by a
    // 2026-06-14 audit as its own CRITICAL.
    static uint32_t sResolveN = 0;
    if ((sResolveN++ % 125u) != 0u) return;

    if (!g_coinClass)     g_coinClass     = R::FindClass(kCoinClassName);
    if (!g_gunClass)      g_gunClass      = R::FindClass(kGunClassName);
    if (g_offCoinPoints < 0 && g_coinClass)
        g_offCoinPoints = R::FindPropertyOffset(g_coinClass, L"points");
    if (!g_libCdo)        g_libCdo        = R::FindClassDefaultObject(L"lib_C");
    if (!g_sellObjectFn && g_libCdo)
        g_sellObjectFn = R::FindFunction(R::ClassOf(g_libCdo), L"sellObject");
    if (!g_finishSpawnFn) g_finishSpawnFn = R::FindFunction(R::FindClass(L"GameplayStatics"),
                                                            L"FinishSpawningActor");
    // The gun verb resolves its FName on the game thread; drive the pump every Install. The COLLECT
    // verb has its own registration in the collect lane (one vm_dispatch callback per NAME).
    if (!g_verbRegistered.load(std::memory_order_acquire)) {
        if (vm::RegisterVirtualVerb(kVerbNameGunUse, kVerbCoinGunUse, &OnVerbEntry)) {
            g_verbRegistered.store(true, std::memory_order_release);
            UE_LOGI("coingun[sale]: registered the 0x45 verb '%ls' (id=%d)",
                    kVerbNameGunUse, kVerbCoinGunUse);
        }
    }
    vm::TickResolvePending();

    // Drive the collect lane's own install AFTER our class resolve above (it reads CoinClass()) and
    // OUTSIDE our early-return below, so a sale-lane resolve that never lands cannot silently keep
    // the collect lane -- a different subsystem with different dependencies -- from installing.
    internal::InstallCollect();

    if (g_installed.load(std::memory_order_acquire)) return;          // sale lane already done
    if (!g_coinClass || !g_finishSpawnFn || !g_sellObjectFn) return;  // retry next tick

    if (!ue_wrap::ufunction_hook::InstallPostHook(g_finishSpawnFn, &OnFinishSpawnPost)) {
        UE_LOGE("coingun[sale]: FinishSpawningActor POST install FAILED -- the client-coin barrier is "
                "DISABLED, so a client's own coins would survive its shot");
        return;
    }
    g_installed.store(true, std::memory_order_release);
    UE_LOGI("coingun[sale]: installed -- verb bracket + FinishSpawningActor POST. baocoin_C is on the "
            "WorldActor allowlist and prop_coingun_C on the EX-spawn source list.");
}

}  // namespace coop::coingun_sync
