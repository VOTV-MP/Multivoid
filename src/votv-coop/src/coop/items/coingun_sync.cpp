// coop/items/coingun_sync.cpp -- see coop/items/coingun_sync.h for the design and WHY.

#include "coop/items/coingun_sync.h"

#include "coop/comms/peer_action_feed.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
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

// THE TWO VERBS THIS MODULE WATCHES, AND WHY THE NAME IS THE HANDLE (2026-08-25, B2).
// `vm_dispatch`'s active-verb window is ONE global thread-local, so a verb id is a PROJECT-WIDE
// namespace and a CALLER-CHOSEN tag inside it: `[V]` container_contents_sync's kVerbDirty,
// meadow_db_sync's kVerbMark and drive_sync's kVerbPutDriveIn are all 1. `av.active` is worse still
// -- it is true for ANY registered verb on this thread. The header (`ue_wrap/core/vm_dispatch.h`)
// states the contract: GATE ON `verbName`, never on `active`, never on `verbId`. These reads used to
// test the id, which was safe only by luck; converting them is a PRECONDITION for this module
// registering a second verb, since otherwise being inside `actionOptionIndex` would satisfy a test
// meaning "inside the gun". The id survives only as the Bracket tag the callback switches on, which
// is scoped by construction.
constexpr const wchar_t* kVerbNameGunUse  = L"playerHandUse_LMB";
constexpr const wchar_t* kVerbNameCollect = L"actionOptionIndex";
constexpr int kVerbCoinGunUse = 6;
constexpr int kVerbCoinCollect = 7;

constexpr const wchar_t* kGunClassName  = L"prop_coingun_C";
constexpr const wchar_t* kCoinClassName = L"baocoin_C";

// `[V]` The one option `baocoin_C::getActionOptions` offers, read off its bytecode: stmt[0] is
// `EX_SetArray(K2Node_MakeArray_Array, [EX_ByteConst 7])` -- a single compile-time constant, no
// branch anywhere in the 8-statement function, identical for every coin.
//
// AND `[V]` IT IS INERT. The whole of `actionOptionIndex` is 4 `EX_LetValueOnPersistentFrame`
// param stashes + `ExecuteUbergraph_baocoin(441)` + return: no branch on `action`, no branch at all.
// The design pass specified deriving this at runtime from `getActionOptions` "so it is not
// hardcoded"; that is WITHDRAWN on this measurement. Deriving it would cost a ProcessEvent dispatch
// per collect plus a leaked engine-allocated TArray (ParamFrame has no destructor for those), to
// compute a value the callee provably never reads -- a fix whose radius exceeds its defect's
// (`[[feedback-a-converged-fix-should-shrink-not-grow]]`). It is a named constant citing the
// bytecode instead, which is what "not a magic number" actually buys.
constexpr uint8_t kCoinCollectAction = 7;

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
void* g_actionOptFn   = nullptr;   // baocoin_C::actionOptionIndex -- the host's collect executor
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
// B2: the collect lane. `collectSeen*` are per-ENTRY so the log can answer the question v137 could
// not -- which of the two entries actually fires in play. `[V]` v137's overlap interceptor printed
// ZERO lines across six real credits, and a seam never observed firing is indistinguishable from a
// broken one, so counting them separately is the whole point.
std::atomic<unsigned long long> g_collectSeenPress{0};    // the E-press / 0x45 entry
std::atomic<unsigned long long> g_collectSeenOverlap{0};  // the BndEvt overlap entry
std::atomic<unsigned long long> g_collectForwarded{0};
std::atomic<unsigned long long> g_collectPerformed{0};
std::atomic<unsigned long long> g_collectUnresolved{0};
std::atomic<unsigned long long> g_collectNoCredit{0};

// ---- THE BARRIER QUEUE -------------------------------------------------------------------------
// Client-side coins captured inside the gun bracket, destroyed at the next net-pump Tick. These are
// world-scoped engine objects crossing a frame boundary, so they are CachedObjRef (CLAUDE.md 4j: a
// dying world's actors are not kill-flagged until GC purge, measured 44+ s, so slot liveness alone
// hands out actors of a world that no longer exists). A bare AActor* here would be the exact defect
// the 2026-08-23 world-stamp arc converted 78 sites away from.
std::mutex g_pendingMu;
std::vector<ue_wrap::CachedObjRef> g_pendingKill;

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

// The artifact's NAME: the save key when the prop has one, "#<eid>" otherwise. The two spaces
// cannot collide -- a save key is never empty and never starts with '#'.
std::wstring ArtifactName(const std::wstring& key, uint32_t eid) {
    if (!key.empty()) return key;
    return L"#" + std::to_wstring(static_cast<unsigned long>(eid));
}

bool IsCoinActor(void* actor) {
    if (!actor) return false;
    if (g_coinClass) return R::ClassOf(actor) == g_coinClass;
    return R::ClassNameOf(actor) == kCoinClassName;   // pre-resolution fallback
}

// Am I inside THIS verb? The ONE ambient-window read this module makes -- see the kVerbName block
// above for why it is the name and not the id or `active`. Pointer-compares first because both
// callers pass a literal this module itself registered (so `RegisterVirtualVerb`'s static-lifetime
// requirement makes the pointers identical), then falls back to a compare for robustness.
bool InVerb(const vm::ActiveVerb& av, const wchar_t* name) {
    if (!av.active || !av.verbName) return false;
    return av.verbName == name || std::wcscmp(av.verbName, name) == 0;
}

bool ForwardCollectToHost(void* coin, const wchar_t* entry);   // section 4, below

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
    g_collectSeenOverlap.fetch_add(1, std::memory_order_relaxed);
    const bool isMirror = coop::world_actor_sync::IsMirroredActor(self) ||
                          coop::world_actor_sync::IsMaterializingMirror();
    if (isMirror) {
        // B2: FORWARD FIRST, THEN CANCEL. v137 cancelled and stopped there, which is why "a client
        // never credits" was true and "the host credits" was not: nothing told the host anything.
        // The player's own body tripping a mirror is a real collect and must reach the authoritative
        // coin. Forwarding BEFORE the cancel matters -- after `return true` there is no seam left.
        const bool fwd = ForwardCollectToHost(self, L"overlap");
        UE_LOGI("coingun[client collect]: SUPPRESSED on mirror coin=%p (tripped by %p), forwarded=%d. "
                "The local credit is cancelled outright on THIS entry (unlike the E-press, which is "
                "EX_LocalVirtualFunction and uncancellable); the host performs the collect on its own "
                "coin.", self, other, fwd ? 1 : 0);
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

// ---- 3. THE VERBS ------------------------------------------------------------------------------
// ONE callback for both registrations; the Bracket's id is scoped by construction (it is the tag
// this module handed the substrate for THIS registration), so switching on it here is safe in a way
// the ambient read is not -- see the kVerbName block at the top.
void OnVerbEntry(const vm::Bracket& b) {
    if (b.verbId == kVerbCoinGunUse) {
        // The gun. Nothing to do at entry -- the work keys off CurrentThreadVerb() in the birth and
        // destroy seams. Kept so a missing bracket is visible in vm_dispatch's stats.
        return;
    }
    if (b.verbId != kVerbCoinCollect) return;

    // THE E-PRESS COLLECT (B2). `actionOptionIndex` is the interaction entry point of MANY classes,
    // and `vm_dispatch` matches on NAME alone and says so in its header: "any further class/authority
    // discrimination is the CONSUMER's job". The ctx class check IS that discrimination -- without it
    // every radial/E interaction in the world would land here.
    if (!IsCoinActor(b.ctx)) return;

    auto* s = LoadSession();
    if (!s || !s->connected()) return;            // solo: the native credit is correct
    if (s->role() == coop::net::Role::Host) {
        // The host's own press. Observe only -- the native credit is authoritative and already runs.
        // This is the POSITIVE CONTROL for the client's silence: without a line that fires where the
        // observer is EXPECTED to fire, a quiet client log is indistinguishable from a dead hook
        // (`[[lesson-an-instrument-blind-to-the-phenomenon-always-passes]]`, which is exactly how
        // v137's overlap interceptor passed review while never having fired).
        g_collectSeenPress.fetch_add(1, std::memory_order_relaxed);
        UE_LOGI("coingun[collect seam] entry=press HOST ctx=%p -- native credit runs here; this line "
                "proves the 0x45 bracket is live in this session", b.ctx);
        return;
    }

    g_collectSeenPress.fetch_add(1, std::memory_order_relaxed);
    // WE CANNOT CANCEL THIS. `[V]` `actionOptionIndex` is reached as EX_LocalVirtualFunction from
    // mainPlayer's ubergraph @1022, and a script UFunction dispatched that way never reads
    // UFunction::Func -- so neither ProcessEvent nor a Func patch can intercept it, and the `0x45`
    // substrate observes without a cancel primitive. The local credit and the local self-destroy of
    // our mirror WILL happen. Forward-and-reconcile is FORCED here, not chosen: the phantom credit
    // is corrected by the host's balance broadcast, and the mirror's disappearance is corrected by
    // the host's own WorldActorDestroy (or, if the host refuses, by the stale-row re-materialise).
    ForwardCollectToHost(b.ctx, L"press");
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
void Refuse(coop::net::Session* s, uint8_t slot, coop::net::CoinGunResultCode code) {
    g_salesRefused.fetch_add(1, std::memory_order_relaxed);
    SendResult(s, slot, code, 0);
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
    }
    return L"could not sell that";
}

// ---- 4. THE COLLECT FORWARD (client) -----------------------------------------------------------
// B2. A client cannot perform a collect: the credit is `lib_C::addPoints`, `EX_LocalVirtualFunction`,
// which no peer but the owner may author. So it forwards the coin's identity and the HOST runs the
// coin's own verb -- act-as-host, and the intent names WHICH coin, never what it is worth.
//
// Called from BOTH entries. `[V]` TWO of them reach the credit block `ExecuteUbergraph_baocoin:441`:
// the overlap BndEvt (which we CAN cancel, and do) and `actionOptionIndex` (which we cannot -- see
// the header). v137 knew about one, and the field's six real credits produced zero lines from it.
//
// Returns true iff a forward was actually sent, so the overlap entry can log honestly.
bool ForwardCollectToHost(void* coin, const wchar_t* entry) {
    auto* s = LoadSession();
    if (!s || !s->connected() || s->role() != coop::net::Role::Client) return false;
    if (!IsCoinActor(coin)) return false;

    // MIRROR-SCOPED. A NON-mirror coin is one of the two cooked maps' placed instances -- level
    // content on both peers, never enrolled, never the host's. Forwarding it would name an eid the
    // host does not have; leaving it native keeps today's behaviour (residual A13, see the header).
    if (!coop::world_actor_sync::IsMirroredActor(coin)) return false;

    const coop::element::ElementId eid = coop::element::Registry::Get().EidForActor(coin);
    if (eid == coop::element::kInvalidId || eid == 0u) {
        // Loud: a mirror with no eid means the actor->eid reverse and the mirror set disagree, which
        // is an identity fault, not a routine miss. Without this the collect would silently do
        // nothing and look exactly like the bug B2 exists to fix.
        UE_LOGE("coingun[collect seam]: %ls entry on MIRROR coin=%p that has NO element id -- cannot "
                "forward. The mirror set and the actor->eid reverse disagree about this actor; the "
                "credit will stay local and be erased by the host's next balance broadcast.",
                entry, coin);
        return false;
    }

    coop::net::CoinCollectPayload p{};
    p.elementId = static_cast<uint32_t>(eid);
    s->SendReliable(coop::net::ReliableKind::CoinCollect, &p, sizeof(p));
    g_collectForwarded.fetch_add(1, std::memory_order_relaxed);
    // THE PROOF LINE. Greppable on purpose: the user's re-run is a one-line grep for
    // "coingun[collect seam]" rather than a judgement call about whether a seam fired.
    UE_LOGI("coingun[collect seam] entry=%ls ctx=%p forwarding eid=%u -- the host performs the "
            "collect on its own coin; our local credit (if the entry allowed one) is a phantom the "
            "balance broadcast corrects", entry, coin, p.elementId);
    return true;
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

    // --- resolve the artifact against OUR OWN world -----------------------------------------------
    // KEY FIRST. This is the whole of B1: v137 resolved the eid alone, and `[V]` a v122 client mints
    // no Element row for its own save-loaded keyed prop, so the eid it sent was 0 for exactly the
    // props a player shoots -- 3 of 3 field sales died right here. The key is the identity that
    // survives, and ResolveLiveActorByKey is the same primitive the PropDestroy receiver uses.
    void* prop = nullptr;
    const wchar_t* how = L"key";
    if (!keyStr.empty()) prop = coop::prop_element_tracker::ResolveLiveActorByKey(keyStr);
    if (!prop && p.elementId != 0u) {
        // The keyless fallback. `[V]` It is NOT reachable through the coin gun today -- the gun's
        // ubergraph casts the hit actor with `asProp` and returns on failure, while the keyless
        // families (prop_garbageClump_C / actorChipPile / trashBitsPile) are NOT Aprop_C descendants
        // (ue_wrap/actors/prop.cpp:145-152 adds them by UNION for exactly that reason). It stays as
        // the fallback for an Aprop_C whose Key genuinely reads None, and it costs nothing: the 4
        // bytes sit in alignment padding the payload needs regardless.
        prop = coop::element::LivePropActor(static_cast<coop::element::ElementId>(p.elementId));
        how  = L"eid";
    }
    if (!prop) {
        Refuse(s, senderSlot, coop::net::CoinGunResultCode::NoSuchProp);
        UE_LOGW("coingun[host]: REFUSED slot=%u artifact='%ls' (key='%ls' eid=%u) -- "
                "REASON=no-such-prop. NEITHER name resolves to a live prop in our world, so the two "
                "peers disagreed about this prop BEFORE anyone fired: a pre-existing stable-ID "
                "divergence this lane exposes, not one it causes. The client's own destroy still "
                "lands, so this degrades to pre-A37 behaviour (item lost, nothing credited) -- it "
                "does not manufacture a NEW loss, and the seller is TOLD.",
                senderSlot, artifact.c_str(), keyStr.empty() ? L"None" : keyStr.c_str(),
                p.elementId);
        return;
    }

    // THE CONSUMPTION GUARD -- has this exact artifact already been minted for? (see g_sold)
    {
        auto it = g_sold.find(artifact);
        if (it != g_sold.end() && it->second.Get() == prop) {
            Refuse(s, senderSlot, coop::net::CoinGunResultCode::AlreadySold);
            UE_LOGW("coingun[host]: REFUSED slot=%u artifact='%ls' -- REASON=already-sold. This prop "
                    "was already minted for and its destroy has not yet removed it. A replayed or "
                    "duplicated sale mints nothing.", senderSlot, artifact.c_str());
            return;
        }
    }

    void* gun = FindLiveGun();
    if (!gun) {
        Refuse(s, senderSlot, coop::net::CoinGunResultCode::NoGun);
        UE_LOGW("coingun[host]: REFUSED slot=%u artifact='%ls' -- REASON=no-live-coingun. Nothing in "
                "our world can execute `sell` (the sender's hand mirror may have been stowed within "
                "the RTT). We mint nothing rather than inventing an instance -- `[V]` a CDO executor "
                "would mint ZERO coins anyway, since EX_Self is the WorldContextObject of every "
                "deferred spawn inside `sell`.", senderSlot, artifact.c_str());
        return;
    }

    // --- price it from OUR OWN copy ---------------------------------------------------------------
    if (!g_sellObjectFn || !g_libCdo) {
        Refuse(s, senderSlot, coop::net::CoinGunResultCode::HostInternal);
        UE_LOGW("coingun[host]: REFUSED slot=%u artifact='%ls' -- REASON=sellObject-unresolved",
                senderSlot, artifact.c_str());
        return;
    }
    const std::wstring propNameStr = ue_wrap::prop::GetPropNameString(prop);
    const R::FName propName = ue_wrap::fname_utils::StringToFName(propNameStr);
    ue_wrap::ParamFrame f(g_sellObjectFn);
    if (!f.valid()) {
        Refuse(s, senderSlot, coop::net::CoinGunResultCode::HostInternal);
        UE_LOGW("coingun[host]: REFUSED slot=%u artifact='%ls' -- REASON=sellObject-frame-invalid",
                senderSlot, artifact.c_str());
        return;
    }
    f.Set<R::FName>(L"object", propName);
    f.Set<bool>(L"onlyShop", true);              // the GUN passes TRUE -- match it exactly
    f.Set<void*>(L"objectToSell", prop);
    f.Set<void*>(L"__WorldContext", prop);
    if (!ue_wrap::Call(g_libCdo, f)) {
        Refuse(s, senderSlot, coop::net::CoinGunResultCode::HostInternal);
        UE_LOGW("coingun[host]: REFUSED slot=%u artifact='%ls' -- REASON=sellObject-dispatch-failed",
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
        Refuse(s, senderSlot, coop::net::CoinGunResultCode::NotSellable);
        UE_LOGW("coingun[host]: REFUSED slot=%u artifact='%ls' -- REASON=not-sellable (sellObject said "
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
        Refuse(s, senderSlot, coop::net::CoinGunResultCode::HostInternal);
        UE_LOGW("coingun[host]: REFUSED slot=%u artifact='%ls' -- REASON=sell-unresolved", senderSlot,
                artifact.c_str());
        return;
    }
    ue_wrap::ParamFrame sf(sellFn);
    if (!sf.valid()) {
        Refuse(s, senderSlot, coop::net::CoinGunResultCode::HostInternal);
        UE_LOGW("coingun[host]: REFUSED slot=%u artifact='%ls' -- REASON=sell-frame-invalid",
                senderSlot, artifact.c_str());
        return;
    }
    sf.Set<int32_t>(L"Index", points);           // `[V]` `Index` IS the price
    sf.Set<void*>(L"comp", meshComp);
    const bool ok = ue_wrap::Call(gun, sf);
    if (!ok) {
        // The dispatch itself failed, so nothing was minted -- do NOT consume the artifact, and do
        // NOT tell the seller it sold. v137 stamped the sold-set unconditionally right here.
        Refuse(s, senderSlot, coop::net::CoinGunResultCode::HostInternal);
        UE_LOGE("coingun[host]: REFUSED slot=%u artifact='%ls' -- REASON=sell-dispatch-failed "
                "(gun=%p comp=%p price=%d). No coins were minted and the artifact is NOT consumed.",
                senderSlot, artifact.c_str(), gun, meshComp, points);
        return;
    }
    g_sold[artifact].Set(prop);      // consume the artifact (see THE CONSUMPTION GUARD)
    SendResult(s, senderSlot, coop::net::CoinGunResultCode::Sold, points);

    // The coins the mint spawns are EX_CallMath BeginDeferreds from the gun's bytecode, so
    // npc_world_enum's source-gated Func thunk catches them and drains to
    // world_actor_sync::HostEnrollExSpawn -- that is what allocates each eid and broadcasts
    // WorldActorSpawn. Nothing here has to enroll anything.
    UE_LOGI("coingun[host]: SOLD slot=%u artifact='%ls' (resolved by %ls) name='%ls' price=%d -> "
            "minted via gun=%p comp=%p. Coins are HOST-OWNED; whoever's body trips one credits the "
            "host. The client's own PropDestroy follows on this lane and removes the prop.",
            senderSlot, artifact.c_str(), how, propNameStr.c_str(), points, gun, meshComp);
}

void OnCoinCollect(const uint8_t* payload, int len, uint8_t senderSlot, void* localPlayer) {
    auto* s = LoadSession();
    if (!s || s->role() != coop::net::Role::Host) {
        UE_LOGW("coingun[host]: CoinCollect received off the HOST -- dropping");
        return;
    }
    if (!payload || len < static_cast<int>(sizeof(coop::net::CoinCollectPayload))) {
        UE_LOGW("coingun[host]: CoinCollect payload too small (len=%d) -- dropping", len);
        return;
    }
    coop::net::CoinCollectPayload p{};
    std::memcpy(&p, payload, sizeof(p));

    // Resolve against OUR OWN registry, fail-closed on TYPE. `LiveActorOfType` refuses an eid naming
    // an Element of any other kind, so a client cannot address, say, a Prop through this lane.
    void* coin = coop::element::LiveActorOfType(
        static_cast<coop::element::ElementId>(p.elementId), coop::element::ElementType::WorldActor);
    if (!coin) {
        // POSITIVE KNOWLEDGE, NOT ABSENCE OF EVIDENCE: the host's WorldActor registry is
        // authoritative for its OWN eids, so "no live actor under this eid" means the coin is
        // genuinely gone -- somebody already collected it, and this forward lost the race. That is
        // the ordinary outcome of two players reaching for one coin, so it is INFO, not a warning.
        g_collectUnresolved.fetch_add(1, std::memory_order_relaxed);
        UE_LOGI("coingun[host collect]: slot=%u eid=%u does not resolve to a live WorldActor -- the "
                "coin is already gone (collected by someone else, or destroyed). Nothing to do.",
                senderSlot, p.elementId);
        return;
    }
    // CLASS gate. The eid resolving is not enough: a WorldActor eid could name a piramid or a wisp,
    // and dispatching `actionOptionIndex` on one of those would run an unrelated interaction.
    if (!IsCoinActor(coin)) {
        UE_LOGW("coingun[host collect]: slot=%u eid=%u resolves to a '%ls', not a baocoin_C -- "
                "REFUSING. A collect intent may only name a coin.",
                senderSlot, p.elementId, R::ClassNameOf(coin).c_str());
        return;
    }
    if (!g_actionOptFn) g_actionOptFn = R::FindFunction(R::ClassOf(coin), kVerbNameCollect);
    if (!g_actionOptFn) {
        UE_LOGE("coingun[host collect]: slot=%u eid=%u -- baocoin_C::actionOptionIndex unresolved, "
                "cannot perform the collect", senderSlot, p.elementId);
        return;
    }

    // Read the balance around the dispatch. This is the ONLY way to know the collect actually took:
    // `[V]` the credit is `lib_C::addPoints`, EX_LocalVirtualFunction, so there is no return value
    // and no seam to observe -- and a silent no-op here would leave the client's mirror destroyed
    // (the E-press entry is uncancellable) while the host's coin lives on: invisible but real.
    int32_t before = 0;
    const bool haveBefore = ue_wrap::economy::ReadPoints(&before);

    ue_wrap::ParamFrame f(g_actionOptFn);
    if (!f.valid()) {
        UE_LOGE("coingun[host collect]: slot=%u eid=%u -- actionOptionIndex frame invalid",
                senderSlot, p.elementId);
        return;
    }
    // `player`: the HOST's own mainPlayer. `[RD]` the credit block cannot depend on it -- block 441
    // is reached from the overlap BndEvt too, and that entry never writes K2Node_Event_player, so a
    // block reading it would break the game's own overlap pickup. But the truthful value is correct
    // under either answer, and act-as-host says the HOST is the one performing this, so we pass the
    // host's player rather than null. Same reasoning for leaving `hit` zeroed (ParamFrame zeroes the
    // frame) and `lookAtComponent` null: neither is available to the overlap entry either.
    // `action` is `[V]` provably inert -- see kCoinCollectAction.
    f.Set<void*>(L"player", localPlayer);
    f.Set<uint8_t>(L"action", kCoinCollectAction);
    const bool dispatched = ue_wrap::Call(coin, f);

    int32_t after = 0;
    const bool haveAfter = ue_wrap::economy::ReadPoints(&after);
    const bool credited  = haveBefore && haveAfter && after != before;

    if (dispatched && credited) {
        g_collectPerformed.fetch_add(1, std::memory_order_relaxed);
        UE_LOGI("coingun[host collect]: PERFORMED slot=%u eid=%u coin=%p -- balance %d -> %d (+%d). "
                "The coin's own verb ran, so its native credit and self-destroy are the game's, not "
                "ours; the WorldActorDestroy that follows removes every peer's mirror.",
                senderSlot, p.elementId, coin, before, after, after - before);
        return;
    }

    // (c) THE COIN IS LIVE AND SOMETHING WENT WRONG. The forwarding client has already destroyed its
    // own mirror on the E-press path (that entry is uncancellable), so leaving this alone makes the
    // host's coin permanently invisible to that peer -- a NEW loss, authored by the fix.
    g_collectNoCredit.fetch_add(1, std::memory_order_relaxed);

    if (!haveBefore || !haveAfter) {
        // WE DO NOT KNOW, AND NOT KNOWING IS NOT THE SAME AS "NO CREDIT". Treating an unreadable
        // balance as a failed collect would (a) act on absence of evidence -- the fail-open family
        // this whole pass keeps catching, inverted -- and (b) fire a full world re-announce on EVERY
        // collect for as long as the read stays broken, turning a diagnostic into a burst storm.
        UE_LOGE("coingun[host collect]: slot=%u eid=%u coin=%p dispatch=%d -- the balance was NOT "
                "readable (before=%d after=%d), so whether this credited is UNKNOWN. Taking no "
                "repair action: acting on an unreadable value would be inventing a verdict.",
                senderSlot, p.elementId, coin, dispatched ? 1 : 0, haveBefore ? 1 : 0,
                haveAfter ? 1 : 0);
        return;
    }

    // Known: the coin lives, the balance did not move. Re-announce the world to that slot so the
    // mirror comes back. The stale-row guard in world_actor_mirror is what lets that announce LAND
    // on a row whose actor the client already killed -- without it this repair is dropped as a
    // duplicate. Throttled per slot: QueueConnectBroadcastForSlot re-announces EVERY world actor,
    // so an error that repeats must not multiply into a burst.
    static std::unordered_map<uint8_t, long long> s_lastRepairMs;
    const long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    auto& last = s_lastRepairMs[senderSlot];
    const bool repair = (nowMs - last) >= 2000;
    UE_LOGE("coingun[host collect]: slot=%u eid=%u coin=%p dispatch=%d -- the coin is LIVE and the "
            "balance did NOT move (%d -> %d). The client's mirror is already gone on its side, so "
            "%s.", senderSlot, p.elementId, coin, dispatched ? 1 : 0, before, after,
            repair ? "re-announcing world actors to that slot"
                   : "SKIPPING the re-announce (one fired for this slot < 2 s ago)");
    if (repair) {
        last = nowMs;
        coop::world_actor_sync::QueueConnectBroadcastForSlot(static_cast<int>(senderSlot));
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
    UE_LOGI("coingun: SESSION SUMMARY -- sales{sent=%llu refused=%llu} coins{captured=%llu "
            "barrierDestroyed=%llu anomalyBirths=%llu} collect{seenPress=%llu seenOverlap=%llu "
            "forwarded=%llu performed=%llu unresolved=%llu noCredit=%llu} soldSet=%zu pendingKill=%zu",
            g_salesSent.load(std::memory_order_relaxed),
            g_salesRefused.load(std::memory_order_relaxed),
            g_capturedCoins.load(std::memory_order_relaxed),
            g_barrierDestroyed.load(std::memory_order_relaxed),
            g_anomalyBirths.load(std::memory_order_relaxed),
            g_collectSeenPress.load(std::memory_order_relaxed),
            g_collectSeenOverlap.load(std::memory_order_relaxed),
            g_collectForwarded.load(std::memory_order_relaxed),
            g_collectPerformed.load(std::memory_order_relaxed),
            g_collectUnresolved.load(std::memory_order_relaxed),
            g_collectNoCredit.load(std::memory_order_relaxed),
            g_sold.size(), g_pendingKill.size());

    // Every WORLD-scoped thing goes. v137 had no OnDisconnect at all, so a reconnecting peer carried
    // a stale sold-set and a pending-kill list of actors belonging to a dead world into the new one.
    // The resolved UClass / UFunction / CDO pointers are NOT world-scoped and stay (CLAUDE.md 4j);
    // the counters stay monotonic on purpose, so the summary above spans the whole process.
    g_sold.clear();
    {
        std::lock_guard<std::mutex> lk(g_pendingMu);
        g_pendingKill.clear();
    }
    g_gunRef = ue_wrap::CachedObjRef{};
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

    // The verbs resolve their FNames on the game thread; drive the pump every Install.
    // BOTH must register or NEITHER counts as done -- a half-registration would leave the E-press
    // collect blind while everything else looked installed.
    if (!g_verbRegistered.load(std::memory_order_acquire)) {
        const bool a1 = vm::RegisterVirtualVerb(kVerbNameGunUse,  kVerbCoinGunUse,  &OnVerbEntry);
        const bool a2 = vm::RegisterVirtualVerb(kVerbNameCollect, kVerbCoinCollect, &OnVerbEntry);
        if (a1 && a2) {
            g_verbRegistered.store(true, std::memory_order_release);
            UE_LOGI("coingun: registered the 0x45 verbs '%ls' (id=%d) + '%ls' (id=%d)",
                    kVerbNameGunUse, kVerbCoinGunUse, kVerbNameCollect, kVerbCoinCollect);
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
