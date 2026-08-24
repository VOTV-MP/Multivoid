// coop/items/coingun_sync.cpp -- see coop/items/coingun_sync.h for the design and WHY.

#include "coop/items/coingun_sync.h"

#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
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

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace coop::coingun_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace GT = ue_wrap::game_thread;
namespace vm = ue_wrap::vm_dispatch;

// C-1 (audit 2026-08-24): vm_dispatch's active-verb window is ONE global thread-local, so a verb id
// is a PROJECT-WIDE namespace, not a per-file one. Id 1 was already taken by four consumers
// (kerfur_form_assembler kVerbTurnOff, drive_sync kVerbPutDriveIn, meadow_db_sync kVerbMark,
// container_contents_sync kVerbDirty). Distinct id here -- but the id is NOT the correctness gate;
// the Context class check in IsInCoinGunVerb() is (see below).
constexpr int kVerbCoinGunUse = 6;

constexpr const wchar_t* kGunClassName  = L"prop_coingun_C";
constexpr const wchar_t* kCoinClassName = L"baocoin_C";

std::atomic<coop::net::Session*> g_session{nullptr};
inline coop::net::Session* LoadSession() { return g_session.load(std::memory_order_acquire); }

std::atomic<bool> g_installed{false};
std::atomic<bool> g_verbRegistered{false};

// Resolved once at Install.
void* g_finishSpawnFn = nullptr;
void* g_collectFn     = nullptr;   // Abaocoin_C's collect BndEvt (the PE-visible pickup)
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
// Client-side coins captured inside the gun bracket, destroyed at the next net-pump Tick. These are
// world-scoped engine objects crossing a frame boundary, so they are CachedObjRef (CLAUDE.md 4j: a
// dying world's actors are not kill-flagged until GC purge, measured 44+ s, so slot liveness alone
// hands out actors of a world that no longer exists). A bare AActor* here would be the exact defect
// the 2026-08-23 world-stamp arc converted 78 sites away from.
std::mutex g_pendingMu;
std::vector<ue_wrap::CachedObjRef> g_pendingKill;

// C-3 (audit 2026-08-24): the artifact must be CONSUMED, or a client can replay one CoinGunSell and
// mint for the same prop forever -- the host removes nothing itself (by design: the client's own
// PropDestroy does that), so nothing else stops a repeat. `order_sync`, which this lane cites as its
// precedent, confirms its commit with an OrderCount()+1 edge before charging; this is the equivalent.
// Keyed on eid AND the actor pointer so it self-cleans: once that prop dies and the eid is recycled
// onto a different actor, LivePropActor returns a new pointer and the sale is allowed again.
std::unordered_map<uint32_t, void*> g_soldEid;

bool IsCoinActor(void* actor) {
    if (!actor) return false;
    if (g_coinClass) return R::ClassOf(actor) == g_coinClass;
    return R::ClassNameOf(actor) == kCoinClassName;   // pre-resolution fallback
}

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
    const bool inGunVerb = av.active && av.verbId == kVerbCoinGunUse;

    if (inGunVerb) {
        {
            std::lock_guard<std::mutex> lk(g_pendingMu);
            g_pendingKill.emplace_back();
            g_pendingKill.back().Set(spawned);   // a read + a stamp; no dispatch
        }
        g_capturedCoins.fetch_add(1, std::memory_order_relaxed);
        UE_LOGI("coingun[client birth]: captured coin %p from our own shot -- deferred to the barrier "
                "(the host mints the real ones)", spawned);
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

// ---- 2. THE COLLECT SEAM -----------------------------------------------------------------------
// One PE interceptor serving both roles.
//   HOST: never cancels. Observes so the log can distinguish "no overlap ever fired" from "overlap
//         fired, credit refused" -- without that a null result in the client-collect test cells is
//         uninterpretable.
//   CLIENT: cancels for a MIRROR (or a coin still materializing -- the collect delegate binds during
//         BeginPlay, i.e. INSIDE FinishSpawning, before the lane installs the mirror row, so the row
//         can never be the sole discriminator). Leaves a NON-mirror coin native: the two maps' placed
//         coins would otherwise become permanently uncollectable AND ghosted, a new loss.
bool OnCollectPre(void* self, void* params) {
    if (!IsCoinActor(self)) return false;

    auto* s = LoadSession();
    if (!s || !s->connected()) return false;     // solo: the native path is correct

    // The delegate signature is (UPrimitiveComponent* Overlapped, AActor* OtherActor, ...), so the
    // tripping actor is the second pointer-sized param.
    void* other = nullptr;
    if (params) other = *reinterpret_cast<void**>(static_cast<uint8_t*>(params) + sizeof(void*));

    if (s->role() == coop::net::Role::Host) {
        int32_t pts = -1;
        if (g_offCoinPoints >= 0)
            pts = *reinterpret_cast<const int32_t*>(static_cast<const uint8_t*>(self) + g_offCoinPoints);
        UE_LOGI("coingun[host collect]: coin=%p points=%d TRIPPED BY actor=%p class='%ls' -- the "
                "native credit runs, balance_sync will broadcast the new total",
                self, pts, other, other ? R::ClassNameOf(other).c_str() : L"<null>");
        return false;                            // observe only; the host credits natively
    }

    // ---- client ----
    const bool isMirror = coop::world_actor_sync::IsMirroredActor(self) ||
                          coop::world_actor_sync::IsMaterializingMirror();
    if (isMirror) {
        UE_LOGI("coingun[client collect]: SUPPRESSED on mirror coin=%p (tripped by %p). A client never "
                "credits; this player's puppet trips the HOST's real coin and the host credits.",
                self, other);
        return true;                             // cancel: no local addPoints, no local destroy
    }

    UE_LOGW("coingun[client collect]: NON-MIRROR coin=%p collected locally (a map-placed coin -- `[V]` "
            "two cooked maps carry them, and they are level content on both peers, never enrolled). "
            "This credits THIS CLIENT only. If this player's puppet also trips the host's copy, the "
            "host's broadcast overwrites the number shortly; if it does not, this number is wrong "
            "until the host's balance next moves. Pre-existing (A13), deliberately not cancelled.",
            self);
    return false;
}

// ---- 3. THE VERB -------------------------------------------------------------------------------
void OnVerbEntry(const vm::Bracket& b) {
    // Nothing to do at entry -- the work keys off CurrentThreadVerb() in the seams below. Kept so the
    // registration has a callback and so a missing bracket is visible in vm_dispatch's stats.
    (void)b;
}

// ---- host helpers ------------------------------------------------------------------------------
void* FindLiveGun() {
    // `[V]` `sell` reads ZERO gun state and positions coins from the SOLD PROP's component, so ANY
    // live instance behaves identically -- there is nothing to prefer.
    // C-3 (audit): "once per sale" is NOT cold enough -- the sale rate is attacker-controlled, and
    // FindObjectByClass is a full GUObjectArray walk. Cache it and walk only on a miss.
    if (void* cached = g_gunRef.Get()) return cached;
    void* found = R::FindObjectByClass(kGunClassName);
    if (found) g_gunRef.Set(found);
    return found;
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
    if (!g_setSimFn) g_setSimFn = R::FindFunction(R::ClassOf(sphere), L"SetSimulatePhysics");
    if (!g_setSimFn) {
        UE_LOGW("coingun[mirror]: SetSimulatePhysics unresolved on the coin's Sphere component");
        return;
    }
    ue_wrap::ParamFrame f(g_setSimFn);
    if (!f.valid()) return;
    f.Set<bool>(L"bSimulate", false);
    const bool ok = ue_wrap::Call(sphere, f);
    UE_LOGI("coingun[mirror]: coin %p -- Sphere(%p) SetSimulatePhysics(false) dispatch=%d (a pose-driven "
            "mirror must not also simulate)", coin, sphere, ok ? 1 : 0);
}

bool IsInCoinGunVerb() {
    const vm::ActiveVerb av = vm::CurrentThreadVerb();
    if (!av.active || av.verbId != kVerbCoinGunUse) return false;
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

void SendSaleForDyingProp(uint32_t elementId) {
    auto* s = LoadSession();
    if (!s || !s->connected()) return;
    if (s->role() != coop::net::Role::Client) return;      // the host's own sale needs no wire
    if (elementId == 0u || elementId == static_cast<uint32_t>(coop::element::kInvalidId)) {
        UE_LOGW("coingun[client sale]: the gun's victim has no element id -- nothing to name, so no "
                "sale is authored (the destroy still goes, i.e. today's behaviour)");
        return;
    }
    coop::net::CoinGunSellPayload p{};
    p.elementId = elementId;
    s->SendReliable(coop::net::ReliableKind::CoinGunSell, &p, sizeof(p));
    g_salesSent.fetch_add(1, std::memory_order_relaxed);
    UE_LOGI("coingun[client sale]: sent CoinGunSell(eid=%u) -- rides IN FRONT of our own unchanged "
            "PropDestroy on this lane, so the host mints while its copy is still alive", elementId);
}

void Tick() {
    std::vector<ue_wrap::CachedObjRef> pending;
    {
        std::lock_guard<std::mutex> lk(g_pendingMu);
        if (g_pendingKill.empty()) return;
        pending.swap(g_pendingKill);
    }
    for (auto& ref : pending) {
        void* actor = ref.Get();                 // null if dead, or if the world moved on
        if (!actor) continue;
        E::DestroyActor(actor);
        g_barrierDestroyed.fetch_add(1, std::memory_order_relaxed);
    }
    UE_LOGI("coingun[barrier]: destroyed %zu of our own client-side coins (captured=%llu, total "
            "destroyed=%llu)", pending.size(),
            g_capturedCoins.load(std::memory_order_relaxed),
            g_barrierDestroyed.load(std::memory_order_relaxed));
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

    // --- resolve the artifact against OUR OWN world -----------------------------------------------
    void* prop = coop::element::LivePropActor(static_cast<coop::element::ElementId>(p.elementId));
    if (!prop) {
        g_salesRefused.fetch_add(1, std::memory_order_relaxed);
        UE_LOGW("coingun[host]: REFUSED slot=%u eid=%u -- REASON=eid-unresolved. No live prop of ours "
                "carries it. Known cause: the rejoin residual where one actor holds a provisional "
                "client-band eid AND an adopted host eid. The client's own destroy still lands, so "
                "this degrades to pre-A37 behaviour (item lost, nothing credited) -- it does not "
                "manufacture a NEW loss.", senderSlot, p.elementId);
        return;
    }

    // C-3: has this exact prop already been sold? (see g_soldEid)
    {
        auto it = g_soldEid.find(p.elementId);
        if (it != g_soldEid.end() && it->second == prop) {
            g_salesRefused.fetch_add(1, std::memory_order_relaxed);
            UE_LOGW("coingun[host]: REFUSED slot=%u eid=%u -- REASON=already-sold. This prop was already "
                    "minted for and its destroy has not yet removed it. A replayed or duplicated sale "
                    "mints nothing.", senderSlot, p.elementId);
            return;
        }
    }

    void* gun = FindLiveGun();
    if (!gun) {
        g_salesRefused.fetch_add(1, std::memory_order_relaxed);
        UE_LOGW("coingun[host]: REFUSED slot=%u eid=%u -- REASON=no-live-coingun. Nothing in our world "
                "can execute `sell` (the sender's hand mirror may have been stowed within the RTT). "
                "We mint nothing rather than inventing an instance.", senderSlot, p.elementId);
        return;
    }

    // --- price it from OUR OWN copy ---------------------------------------------------------------
    if (!g_sellObjectFn || !g_libCdo) {
        UE_LOGW("coingun[host]: REFUSED slot=%u eid=%u -- REASON=sellObject-unresolved", senderSlot,
                p.elementId);
        return;
    }
    const std::wstring propNameStr = ue_wrap::prop::GetPropNameString(prop);
    const R::FName propName = ue_wrap::fname_utils::StringToFName(propNameStr);
    ue_wrap::ParamFrame f(g_sellObjectFn);
    if (!f.valid()) {
        UE_LOGW("coingun[host]: REFUSED slot=%u eid=%u -- REASON=sellObject-frame-invalid", senderSlot,
                p.elementId);
        return;
    }
    f.Set<R::FName>(L"object", propName);
    f.Set<bool>(L"onlyShop", true);              // the GUN passes TRUE -- match it exactly
    f.Set<void*>(L"objectToSell", prop);
    f.Set<void*>(L"__WorldContext", prop);
    if (!ue_wrap::Call(g_libCdo, f)) {
        UE_LOGW("coingun[host]: REFUSED slot=%u eid=%u -- REASON=sellObject-dispatch-failed",
                senderSlot, p.elementId);
        return;
    }
    const int32_t points = f.Get<int32_t>(L"Points");
    const bool    sold   = f.Get<bool>(L"sold");
    if (!sold) {
        g_salesRefused.fetch_add(1, std::memory_order_relaxed);
        UE_LOGW("coingun[host]: REFUSED slot=%u eid=%u -- REASON=not-sellable (sellObject said sold=0 "
                "for name='%ls'). `[V]` with onlyShop=TRUE a list_props row priced <=1 is refused, and "
                "an unknown name is refused outright. Our prop is left UNMUTATED; the client's destroy "
                "still lands, exactly as today.", senderSlot, p.elementId, propNameStr.c_str());
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
        UE_LOGW("coingun[host]: REFUSED slot=%u eid=%u -- REASON=sell-unresolved", senderSlot,
                p.elementId);
        return;
    }
    ue_wrap::ParamFrame sf(sellFn);
    if (!sf.valid()) {
        UE_LOGW("coingun[host]: REFUSED slot=%u eid=%u -- REASON=sell-frame-invalid", senderSlot,
                p.elementId);
        return;
    }
    sf.Set<int32_t>(L"Index", points);           // `[V]` `Index` IS the price
    sf.Set<void*>(L"comp", meshComp);
    const bool ok = ue_wrap::Call(gun, sf);
    g_soldEid[p.elementId] = prop;   // C-3: consume the artifact

    // The coins the mint spawns are EX_CallMath BeginDeferreds from the gun's bytecode, so
    // npc_world_enum's source-gated Func thunk catches them and drains to
    // world_actor_sync::HostEnrollExSpawn -- that is what allocates each eid and broadcasts
    // WorldActorSpawn. Nothing here has to enroll anything.
    UE_LOGI("coingun[host]: SOLD slot=%u eid=%u name='%ls' price=%d -> minted via gun=%p comp=%p "
            "dispatch=%d. Coins are HOST-OWNED; whoever's body trips one credits the host. The "
            "client's own PropDestroy follows on this lane and removes the prop.",
            senderSlot, p.elementId, propNameStr.c_str(), points, gun, meshComp, ok ? 1 : 0);
}

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_installed.load(std::memory_order_acquire)) return;
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
    if (!g_collectFn && g_coinClass)
        g_collectFn = R::FindFunction(g_coinClass,
            L"BndEvt__baocoin_collect_K2Node_ComponentBoundEvent_1_ComponentBeginOverlapSignature__DelegateSignature");

    // The verb resolves its FName on the game thread; drive the pump every Install.
    if (!g_verbRegistered.load(std::memory_order_acquire)) {
        if (vm::RegisterVirtualVerb(L"playerHandUse_LMB", kVerbCoinGunUse, &OnVerbEntry)) {
            g_verbRegistered.store(true, std::memory_order_release);
            UE_LOGI("coingun: registered the 0x45 verb 'playerHandUse_LMB' (id=%d)", kVerbCoinGunUse);
        }
    }
    vm::TickResolvePending();

    if (!g_coinClass || !g_finishSpawnFn || !g_collectFn || !g_sellObjectFn) return;  // retry next tick

    const bool a = ue_wrap::ufunction_hook::InstallPostHook(g_finishSpawnFn, &OnFinishSpawnPost);
    const bool b = GT::RegisterInterceptor(g_collectFn, &OnCollectPre);
    if (!a || !b) {
        UE_LOGE("coingun: seam install FAILED (finishSpawnPost=%d collectPre=%d) -- coin sync DISABLED",
                a ? 1 : 0, b ? 1 : 0);
        return;
    }
    g_installed.store(true, std::memory_order_release);
    UE_LOGI("coingun: installed -- verb bracket + FinishSpawningActor POST + collect interceptor. "
            "baocoin_C is on the WorldActor allowlist and prop_coingun_C on the EX-spawn source list.");
}

}  // namespace coop::coingun_sync
