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
#include "coop/session/world_load_episode.h"
#include "coop/world/world_actor_sync.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/core/vm_dispatch.h"
#include "ue_wrap/engine/engine.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>
#include <vector>

namespace coop::coingun_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
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
void* g_coinClass     = nullptr;
void* g_gunClass      = nullptr;   // prop_coingun_C -- the Context gate (C-1); shared with the arbiter
int32_t g_offCoinSphere  = -1;   // Abaocoin_C::Sphere -- the SIMULATING component (NOT the root)
void* g_setSimFn      = nullptr;   // UPrimitiveComponent::SetSimulatePhysics
int32_t g_offCoinPoints  = -1;   // Abaocoin_C::points  (@0x0240 in the dump; resolved by NAME)
// (g_libCdo / g_sellObjectFn / g_sellFn / g_offPropMesh / g_gunRef / g_salesRefused / g_sold moved
//  to coingun_arbiter.cpp with the receiver, 2026-08-25 -- they are HOST-only and were read nowhere
//  else. See coingun_internal.h for the cut.)

// ---- diagnostics (a 1/s line would be noise; these ride the event logs) -------------------------
std::atomic<unsigned long long> g_capturedCoins{0};
std::atomic<unsigned long long> g_barrierDestroyed{0};
std::atomic<unsigned long long> g_anomalyBirths{0};
std::atomic<unsigned long long> g_salesSent{0};
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
// destruction if that shot ACTUALLY SENT a sale. An unauthored shot RELEASES its coins rather than
// eating them.
//
// WHAT A RELEASE ACTUALLY DEGRADES TO, stated honestly (audit I-5, 2026-08-25 -- the first version of
// this comment said "precisely today's single-player behaviour -- no new loss, no phantom", and the
// collect lane says the opposite about the same coin forty lines away). A released coin is a
// client-local NON-mirror, so picking it up takes coingun_collect's map-placed branch and credits
// THIS CLIENT ONLY -- a phantom the host's next balance move erases (residual A13) -- while the
// prop's own destroy is unchanged and still replicates. So the player keeps the coins and does NOT
// keep the payment: the release degrades to the pre-A37 LOSS, not to single-player. It is still
// strictly better than the alternative, which was to lose the prop, the coins AND the explanation --
// but "strictly better" is the claim, not "no loss".
struct PendingShot {
    std::vector<ue_wrap::CachedObjRef> coins;
    bool authored = false;
};
std::mutex g_pendingMu;
std::vector<PendingShot> g_pendingShots;



}  // namespace

// ---- the four reads the COLLECT lane shares (declared in coingun_internal.h) -------------------
namespace internal {

coop::net::Session* Session() { return LoadSession(); }

bool IsCoinActor(void* actor) {
    if (!actor) return false;
    if (g_coinClass) return R::ClassOf(actor) == g_coinClass;
    return R::ClassNameOf(actor) == kCoinClassName;   // pre-resolution fallback
}

// Am I inside THIS verb? The SALE lane's ambient-window read (the collect lane reads `b.ctx` off its
// own bracket) -- see vm_dispatch.h's contract box for why it is the NAME and not the id or `active`. Pointer-compares first because
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
    //
    // READ-ONLY ON `g_gunClass` -- NEVER RESOLVE HERE (audit CRITICAL C-2, 2026-08-25, a defect this
    // very function introduced). `R::FindClass` is an UNCACHED, negative-unlatched walk of the whole
    // GUObjectArray with a name render per object, and `prop_coingun_C`'s UClass is not resident in
    // the ordinary world (`[V]` the gun is placed in 3 of 261 maps) -- so resolving here bought one
    // full object-array walk on EVERY left click of all 146 of those classes, at click rate,
    // unthrottled. `Install` already retries the identical resolve inside its ~1 Hz throttle, so this
    // call could never buy anything the throttle does not deliver within a second.
    //
    // What the read-only form costs, stated rather than hidden: for at most one second after the gun
    // class first becomes resident, a shot opens no group -- its coins land in the defensive group
    // the birth seam opens, which is never marked authored, so they are RELEASED. That is the safe
    // direction (the player keeps their coins), and it is also why BOTH this gate and
    // IsInCoinGunVerb must read without resolving: if one of them resolved mid-bracket the other
    // would disagree with it inside a single shot, and the destroy seam would mark a stale group.
    auto* s = LoadSession();
    if (!s || !s->connected() || s->role() != coop::net::Role::Client) return;
    if (!b.ctx || !g_gunClass) return;
    if (R::ClassOf(b.ctx) != g_gunClass) return;
    std::lock_guard<std::mutex> lk(g_pendingMu);
    g_pendingShots.emplace_back();
}

// ---- host helpers ------------------------------------------------------------------------------

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
    // READ-ONLY on g_gunClass -- see OnVerbEntry for why neither gate may resolve here. This one is
    // colder (it is reached only from the destroy seam, for a keyed prop actually dying) but it must
    // agree with OnVerbEntry within a single bracket, and the only way to guarantee that is for both
    // to read the value Install publishes rather than race to produce it.
    if (!av.ctx || !g_gunClass) return false;
    return R::ClassOf(av.ctx) == g_gunClass;
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
    {
        static uint32_t sSweepN = 0;
        if ((sSweepN++ % 125u) == 0u) internal::SweepSoldSet();
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
                "stay in the world, and picking them up credits THIS CLIENT ONLY (a phantom the "
                "host's next balance move erases -- residual A13), while the prop's own destroy still "
                "replicates. So this degrades to the pre-A37 LOSS, not to single-player: strictly "
                "better than eating the coins too, not harmless. If this fires, the sale was blocked "
                "upstream (world-load episode / reconcile window / kerfur capture / no name to send) "
                "and THAT is the thing to fix.", released, releasedShots);
    }
    if (destroyed) {
        UE_LOGI("coingun[barrier]: destroyed %zu of our own client-side coins across %zu authored "
                "shot(s) (captured=%llu, total destroyed=%llu)", destroyed,
                shots.size() - releasedShots,
                g_capturedCoins.load(std::memory_order_relaxed),
                g_barrierDestroyed.load(std::memory_order_relaxed));
    }
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
    UE_LOGI("coingun[sale]: SESSION SUMMARY -- salesSent=%llu coins{captured=%llu "
            "barrierDestroyed=%llu anomalyBirths=%llu} pendingShots=%zu",
            g_salesSent.load(std::memory_order_relaxed),
            g_capturedCoins.load(std::memory_order_relaxed),
            g_barrierDestroyed.load(std::memory_order_relaxed),
            g_anomalyBirths.load(std::memory_order_relaxed),
            g_pendingShots.size());
    internal::OnDisconnectCollect();   // the collect lane dumps its own half

    // Every WORLD-scoped thing goes. v137 had no OnDisconnect at all, so a reconnecting peer carried
    // a stale sold-set and a pending-kill list of actors belonging to a dead world into the new one.
    // The resolved UClass / UFunction / CDO pointers are NOT world-scoped and stay (CLAUDE.md 4j);
    // the counters stay monotonic on purpose, so the summary above spans the whole process.
    internal::OnDisconnectArbiter();   // the host half clears its own world-scoped state
    {
        std::lock_guard<std::mutex> lk(g_pendingMu);
        g_pendingShots.clear();
    }
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
    internal::InstallArbiter();   // the HOST half's own resolves, inside this same 1 Hz throttle
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
    // NOTE (2026-08-25, preserved verbatim across the arbiter extraction, NOT introduced by it):
    // this gates the CLIENT barrier's install on the HOST arbiter's sellObject resolve. The two have
    // nothing to do with each other, so a client whose lib_C CDO never resolves gets no barrier at
    // all. Kept because an extraction commit does not change behaviour; filed as its own item.
    if (!g_coinClass || !g_finishSpawnFn || !internal::ArbiterResolved()) return;  // retry next tick

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
