// coop/items/coingun_sync.cpp -- the sale lane: a client shoots a prop, and the host prices and
// mints. This file is the client half (the gun verb bracket, the coin capture, the
// commit-or-release barrier, the outbound sale, the result the seller reads) plus the coin
// birth instrument; the host's receiver is coingun_arbiter.cpp and the pickup is
// coingun_collect.cpp. See coop/items/coingun_sync.h.

#include "coop/items/coingun_sync.h"

#include "coingun_internal.h"   // co-located private header (src tree, not include/)

#include "coop/comms/peer_action_feed.h"
#include "coop/element/registry.h"
#include "coop/props/prop_element_tracker.h"  // the host key index, for the NoSuchProp diagnosis
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
#include <unordered_map>
#include <vector>

namespace coop::coingun_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace vm = ue_wrap::vm_dispatch;

}  // namespace

// The two verb names both lanes key on, declared extern in coingun_internal.h; the name is the
// handle, never the id or the active flag.
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
void* g_gunClass      = nullptr;   // prop_coingun_C, the context gate; shared with the arbiter
int32_t g_offCoinSphere  = -1;   // Abaocoin_C::Sphere, the simulating component (not the root)
void* g_setSimFn      = nullptr;   // UPrimitiveComponent::SetSimulatePhysics

// Diagnostics; they ride the event logs.
std::atomic<unsigned long long> g_capturedCoins{0};
std::atomic<unsigned long long> g_barrierDestroyed{0};
std::atomic<unsigned long long> g_anomalyBirths{0};
std::atomic<unsigned long long> g_salesSent{0};
// The barrier queue: the client's coins captured inside the gun bracket, resolved at the next
// pump tick. World-scoped objects crossing a frame boundary, so CachedObjRef. The capture is
// per shot and conditional: a shot opens a group at the verb bracket, its coins land in it, and
// the group is destroyed only if that shot sent a sale; a shot that authored nothing (the
// world-load episode, the reconcile window, the kerfur capture, no name to send) releases its
// coins rather than eating them, since a local artifact must not be suppressed until the
// authoritative one is confirmed. A released coin is a client-local non-mirror: picking it up
// credits this client only, a phantom the host's next balance move erases, while the prop's
// destroy still replicates. So a release keeps the coins and loses the payment: better than
// losing the prop, the coins and the explanation, not harmless.
struct PendingShot {
    std::vector<ue_wrap::CachedObjRef> coins;
    bool authored = false;
};
std::mutex g_pendingMu;
std::vector<PendingShot> g_pendingShots;



}  // namespace

// The reads the collect lane shares, declared in coingun_internal.h.
namespace internal {

coop::net::Session* Session() { return LoadSession(); }

bool IsCoinActor(void* actor) {
    if (!actor) return false;
    if (g_coinClass) return R::ClassOf(actor) == g_coinClass;
    return R::ClassNameOf(actor) == kCoinClassName;   // pre-resolution fallback
}

// Inside this verb? The sale lane's ambient read (the collect lane reads its own bracket). A
// pointer compare first, since every caller passes a literal this module registered, then a
// string compare.
bool InVerb(const vm::ActiveVerb& av, const wchar_t* name) {
    if (!av.active || !av.verbName) return false;
    return av.verbName == name || std::wcscmp(av.verbName, name) == 0;
}

void*   CoinClass()        { return g_coinClass; }

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

// The coin birth: a Func-thunk post on FinishSpawningActor, firing mid-bytecode inside the gun's
// still-open bracket. Reads only: an engine call here corrupts, and sell applies an impulse to
// this very coin after Finish returns.
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
            // The verb entry opens the group; a coin arriving with none open gets one rather than
            // being dropped.
            if (g_pendingShots.empty()) g_pendingShots.emplace_back();
            g_pendingShots.back().coins.emplace_back();
            g_pendingShots.back().coins.back().Set(spawned);   // a read + a stamp; no dispatch
        }
        g_capturedCoins.fetch_add(1, std::memory_order_relaxed);
        UE_LOGI("coingun[client birth]: captured coin %p from our own shot -- held for the barrier, "
                "which destroys it ONLY if this shot actually authored a sale (v140)", spawned);
        return;
    }

    // Not our shot. A wire materialisation of a host coin is common; anything else fails open and
    // must be loud: this gate keys on the verb and the collect cancel on mirror-ness, so a wrong or
    // unresolved verb name fails them independently and would leave a client-local coin neither
    // destroyed nor cancelled, crediting locally in silence.
    if (coop::world_actor_sync::IsMaterializingMirror()) return;
    if (coop::world_load_episode::InEpisode()) return;   // world-rebuild churn is not a signal

    g_anomalyBirths.fetch_add(1, std::memory_order_relaxed);
    UE_LOGE("coingun[ANOMALY]: a baocoin_C was born on this CLIENT outside BOTH the gun verb bracket "
            "and a mirror materialization (actor=%p). Either the verb name stopped resolving (check "
            "vm_dispatch stats) or a producer we never censused exists. This coin will credit LOCALLY "
            "and diverge.", spawned);
}

// The gun verb. The collect verb has its own registration and callback in coingun_collect.cpp;
// vm_dispatch is one callback per name.
void OnVerbEntry(const vm::Bracket& b) {
    // Open a shot group; the birth seam appends to it, the destroy seam marks it authored, and the
    // barrier destroys its coins only if a sale went out. The context gate is not optional:
    // vm_dispatch matches on the verb name, and playerHandUse_LMB is declared by every hand-usable
    // tool, so without it every knife swing would open and release an empty group. Read-only on the
    // gun class, never resolved here: FindClass is an uncached full walk with a name render per
    // object, the gun's class is not resident in the ordinary world, and resolving here once cost a
    // full walk on every left click. Install retries the resolve inside its 1 Hz throttle. The
    // cost: for at most a second after the class becomes resident a shot opens no group, its coins
    // land in the defensive group the birth seam opens and are released, the safe direction; and
    // both this gate and IsInCoinGunVerb must read rather than resolve, or they could disagree
    // inside one shot.
    auto* s = LoadSession();
    if (!s || !s->connected() || s->role() != coop::net::Role::Client) return;
    if (!b.ctx || !g_gunClass) return;
    if (R::ClassOf(b.ctx) != g_gunClass) return;
    std::lock_guard<std::mutex> lk(g_pendingMu);
    g_pendingShots.emplace_back();
}

// Host helpers.

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
    // The simulating component by name: Abaocoin_C declares the collect sphere first, the mesh
    // second and `Sphere` (the one shipping with physics on) third, so a root-component call would
    // log physics-off while Sphere kept simulating and fought the pose drive. A component is a
    // UObject, so it takes a normal reflected call.
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
    // Resolved on the declaring class: FindFunction matches the owning class exactly and does not
    // climb, and SetSimulatePhysics is declared on UPrimitiveComponent, so asking the sphere's own
    // class could never succeed, and each miss was a full walk, once per mirrored coin. The
    // negative is latched, so a failure costs one walk.
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

// The coin's birth value.
namespace {

// Per-class offset caches, the negative cached too, so a class that lacks the property costs
// one walk rather than one per actor; a sale mints dozens of coins and every one lands here.
std::mutex g_birthMu;
std::unordered_map<void*, int32_t> g_pointsOffByClass;   // UClass* -> the points offset (-1 = absent)
std::unordered_map<void*, int32_t> g_meshOffByClass;     // UClass* -> the mesh component offset
void* g_getMaterialFn = nullptr;                          // UPrimitiveComponent::GetMaterial
bool  g_getMaterialResolveFailed = false;                 // the negative latched; a miss is a full walk

// The declaring class, resolved once for this file, positive and negative both latched: two
// functions each resolving it cost two full walks on a client's first mirrored coin.
void* g_primCompCls = nullptr;
bool  g_primCompResolveFailed = false;

void* PrimitiveComponentClass() {
    if (g_primCompCls || g_primCompResolveFailed) return g_primCompCls;
    g_primCompCls = R::FindClass(L"PrimitiveComponent");
    if (!g_primCompCls) {
        g_primCompResolveFailed = true;
        UE_LOGW("coingun: UPrimitiveComponent unresolved -- the mirror park and the birth instrument "
                "are both disabled for this session. Latched: this walk is not repeated per coin.");
    }
    return g_primCompCls;
}

int32_t PointsOffsetFor(void* cls) {
    if (!cls) return -1;
    std::lock_guard<std::mutex> lk(g_birthMu);
    auto it = g_pointsOffByClass.find(cls);
    if (it != g_pointsOffByClass.end()) return it->second;
    const int32_t off = R::FindPropertyOffset(cls, L"points");
    g_pointsOffByClass[cls] = off;
    return off;
}

int32_t MeshOffsetFor(void* cls) {
    if (!cls) return -1;
    std::lock_guard<std::mutex> lk(g_birthMu);
    auto it = g_meshOffByClass.find(cls);
    if (it != g_meshOffByClass.end()) return it->second;
    // By name, and the name is `baocoin`: the ubergraph sets the material on that component on
    // every branch; `collect` is the root and `Sphere` the simulating body.
    const int32_t off = R::FindPropertyOffset(cls, L"baocoin");
    g_meshOffByClass[cls] = off;
    return off;
}

}  // namespace

// The class precondition, owned here rather than by the callers' gates: these take a bare
// pointer and resolve `points` by name, so without it a caller could read, and SeedCoinMirror
// write, the points of any class that declares one.
bool IsCoinClass(void* cls) {
    if (!cls) return false;
    return cls == g_coinClass || R::NameEquals(R::NameOf(cls), kCoinClassName);
}

bool IsCoinActor_(void* actor) {
    return actor && IsCoinClass(R::ClassOf(actor));
}

int32_t ReadCoinPoints(void* coin) {
    if (!IsCoinActor_(coin)) return -1;
    // The spelling: the property is declared `Points`, and this asks for `points`; it resolves only
    // because FindPropertyOffset compares case-insensitively. Load-bearing: a case-sensitive
    // resolver would return -1 here and every mirrored coin would fall back to the default colour.
    const int32_t off = PointsOffsetFor(R::ClassOf(coin));
    if (off < 0) return -1;
    return *reinterpret_cast<const int32_t*>(static_cast<const uint8_t*>(coin) + off);
}

bool SeedCoinMirror(void* coin, int32_t points) {
    if (!IsCoinActor_(coin)) return false;
    const int32_t off = PointsOffsetFor(R::ClassOf(coin));
    if (off < 0) {
        UE_LOGW("coingun[mirror]: cannot resolve baocoin_C::points on this mirror -- it will be born at "
                "the CDO default and paint the wrong colour. (Resolved off the actor's own class, so "
                "this is a real reflection failure, not the Install-latch race.)");
        return false;
    }
    *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(coin) + off) = points;
    return true;
}

void DescribeCoin(void* coin, int32_t& outPoints, std::wstring& outMaterial) {
    outPoints = -1;
    outMaterial.clear();
    if (!IsCoinActor_(coin)) return;
    outPoints = ReadCoinPoints(coin);

    void* cls = R::ClassOf(coin);
    const int32_t meshOff = MeshOffsetFor(cls);
    if (meshOff < 0) return;
    void* mesh = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(coin) + meshOff);
    if (!mesh) return;

    // Declared on UPrimitiveComponent, and FindFunction does not climb, so the declarer is asked.
    // The resolve happens outside the lock (a full walk, and idempotent), and the result is read
    // into a local under it, since the pointer is plain storage.
    void* getMatFn = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_birthMu);
        getMatFn = g_getMaterialFn;
    }
    if (!getMatFn) {
        void* primCls = PrimitiveComponentClass();
        void* resolved = primCls ? R::FindFunction(primCls, L"GetMaterial") : nullptr;
        std::lock_guard<std::mutex> lk(g_birthMu);
        if (!g_getMaterialFn) {
            g_getMaterialFn = resolved;
            if (!resolved && !g_getMaterialResolveFailed) {
                g_getMaterialResolveFailed = true;
                UE_LOGW("coingun[birth]: UPrimitiveComponent::GetMaterial unresolved -- the birth "
                        "instrument will print mat='<unresolved>' for the rest of this session. "
                        "Latched: this walk is not repeated per coin. (audit M-3)");
            }
        }
        getMatFn = g_getMaterialFn;
    }
    if (!getMatFn) return;

    ue_wrap::ParamFrame f(getMatFn);
    if (!f.valid()) return;
    f.Set<int32_t>(L"ElementIndex", 0);
    if (!ue_wrap::Call(mesh, f)) return;
    void* mat = f.Get<void*>(L"ReturnValue");
    if (!mat) return;
    outMaterial = R::ToString(R::NameOf(mat));
}

bool IsInCoinGunVerb() {
    const vm::ActiveVerb av = vm::CurrentThreadVerb();
    if (!InVerb(av, kVerbNameGunUse)) return false;
    // The correctness gate: vm_dispatch matches on the verb name alone, and playerHandUse_LMB is
    // declared by every hand-usable tool (the knife, the hacksaw, the flamethrower, the toolgun);
    // without this a client destroying a keyed prop with any of them would author a sale and the
    // host would mint coins for it. Read-only on the gun class, as in OnVerbEntry: the two gates
    // must agree within a single bracket, so both read the value Install publishes.
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
        // The one "nothing to name" case: a keyless prop with no element row is unnameable in both
        // identity domains. If this line appears for an ordinary keyed prop, the destroy seam's key
        // read is the defect.
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
    // This authorises the barrier to destroy the coins this shot spawned: set only after the send
    // happened, on the group this shot opened, so every path that returned before here leaves its
    // coins to be released.
    {
        std::lock_guard<std::mutex> lk(g_pendingMu);
        if (!g_pendingShots.empty()) g_pendingShots.back().authored = true;
    }
    UE_LOGI("coingun[client sale]: sent CoinGunSell(key='%ls' eid=%u) -- rides IN FRONT of our own "
            "unchanged PropDestroy on this lane, so the host mints while its copy is still alive",
            key.empty() ? L"None" : key.c_str(), eid);
}

void Tick() {
    // The host's key index, periodically: every NoSuchProp refusal is a lookup into it, and the
    // refusal alone cannot say whether the key was wrong or the index empty. On the tick rather
    // than the teardown summary, since a killed process never reaches a teardown.
    {
        // Periodic: a one-shot fired on the first connected tick and read the index while the world
        // was still loading, a number that says nothing about the state a sale is judged against.
        static uint64_t sNextMs = 0;
        const uint64_t nowMs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        auto* ls = LoadSession();
        if (nowMs >= sNextMs && ls && ls->connected() && ls->role() == coop::net::Role::Host) {
            sNextMs = nowMs + 30000;
            UE_LOGI("coingun[arbiter]: host key index holds %zu keyed prop(s) -- a NoSuchProp "
                    "refusal against a NEAR-ZERO index is an enrollment gap, not a bad key",
                    coop::prop_element_tracker::KeyIndexSize());
        }
    }
    // The host half: consumed artifacts whose prop has died are erased, the consumption guard's
    // lifetime. About 1 Hz at the pump rate; the map normally holds none or one entry.
    {
        static uint32_t sSweepN = 0;
        if ((sSweepN++ % 125u) == 0u) internal::SweepSoldSet();
    }

    // The barrier: commit or release, per shot. The bracket that opened each group completed
    // synchronously on this thread before this runs, so `authored` is final.
    std::vector<PendingShot> shots;
    {
        std::lock_guard<std::mutex> lk(g_pendingMu);
        if (g_pendingShots.empty()) return;
        shots.swap(g_pendingShots);
    }
    size_t destroyed = 0, released = 0, releasedShots = 0;
    for (auto& shot : shots) {
        if (!shot.authored) {
            // No sale went out for this shot, so there is no authoritative coin to defer to; ours
            // are left alone, and they credit locally.
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
        // The price, not a bare acknowledgement: the price multiplier is per instance and can
        // diverge (a battery by its charge, food by its uses), so the toast the player's own local
        // sell printed can name a different number than the host minted, and saying the host's
        // makes that visible.
        line = L"sold it for " + std::to_wstring(static_cast<long long>(r.points)) +
               L" points (the host's price)";
    } else {
        line = ResultText(code);
    }
    // AnnounceDirect, not Announce: functional feedback about the player's own action, which the
    // peer-actions toggle must not be able to hide.
    coop::peer_action_feed::AnnounceDirect(
        static_cast<uint8_t>(coop::players::Registry::Get().LocalPeerId()), line);
    UE_LOGI("coingun[client]: CoinGunResult code=%u points=%d -- '%ls'",
            static_cast<unsigned>(r.code), r.points, line.c_str());
}

void OnDisconnect() {
    // The session summary: one grep answers whether this lane did anything at all this run.
    UE_LOGI("coingun[sale]: SESSION SUMMARY -- salesSent=%llu coins{captured=%llu "
            "barrierDestroyed=%llu anomalyBirths=%llu} pendingShots=%zu",
            g_salesSent.load(std::memory_order_relaxed),
            g_capturedCoins.load(std::memory_order_relaxed),
            g_barrierDestroyed.load(std::memory_order_relaxed),
            g_anomalyBirths.load(std::memory_order_relaxed),
            g_pendingShots.size());
    internal::OnDisconnectCollect();   // the collect lane dumps its own half

    // Every world-scoped thing goes; the resolved class, function and CDO pointers stay, and the
    // counters stay monotonic so the summary spans the whole process.
    internal::OnDisconnectArbiter();   // the host half clears its own world-scoped state
    {
        std::lock_guard<std::mutex> lk(g_pendingMu);
        g_pendingShots.clear();
    }
}

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    // Both lanes must be done before this stops running: the sale lane needs FinishSpawningActor
    // and sellObject, the collect lane the coin's overlap delegate, so either can resolve first,
    // and gating on one latch alone would strand the other.
    if (g_installed.load(std::memory_order_acquire) && internal::CollectInstalled()) return;
    // This runs at the pump rate, and every resolve below is a full walk with a name render per
    // entry; in a world where the coin class is not resident (it loads with the gun asset) that
    // would be several walks per tick, so the retry is bound to about 1 Hz.
    static uint32_t sResolveN = 0;
    if ((sResolveN++ % 125u) != 0u) return;

    if (!g_coinClass)     g_coinClass     = R::FindClass(kCoinClassName);
    if (!g_gunClass)      g_gunClass      = R::FindClass(kGunClassName);
    internal::InstallArbiter();   // the HOST half's own resolves, inside this same 1 Hz throttle
    if (!g_finishSpawnFn) g_finishSpawnFn = R::FindFunction(R::FindClass(L"GameplayStatics"),
                                                            L"FinishSpawningActor");
    // The gun verb resolves its name on the game thread, so the pending resolves are driven every
    // Install; the collect verb has its own registration in the collect lane.
    if (!g_verbRegistered.load(std::memory_order_acquire)) {
        if (vm::RegisterVirtualVerb(kVerbNameGunUse, kVerbCoinGunUse, &OnVerbEntry)) {
            g_verbRegistered.store(true, std::memory_order_release);
            UE_LOGI("coingun[sale]: registered the 0x45 verb '%ls' (id=%d)",
                    kVerbNameGunUse, kVerbCoinGunUse);
        }
    }
    vm::TickResolvePending();

    // The collect lane's install runs after the class resolve above (it reads CoinClass) and
    // outside the early return below, so a sale-lane resolve that never lands cannot keep it from
    // installing.
    internal::InstallCollect();

    if (g_installed.load(std::memory_order_acquire)) return;          // sale lane already done
    // The client barrier's install is gated on the host arbiter's sellObject resolve, which it does
    // not depend on; a client whose lib_C CDO never resolves gets no barrier. Filed as its own
    // item.
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
