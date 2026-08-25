// coop/items/coingun_collect.cpp -- THE COLLECT LANE: somebody picks a coin up, the HOST credits.
//
// EXTRACTED from coingun_sync.cpp 2026-08-25 (it passed the 800-LOC soft cap at 932 the moment B2
// landed). The cut is by SUBSYSTEM, not by size: the sale lane and the collect lane share only the
// four reads in coingun_internal.h. See that header for the cut's shape and why it is clean.
// Design + the full WHY: coop/items/coingun_sync.h (THE SHAPE / field failure #4), and
// research/findings/inventory-items/votv-v137-field-defects-DESIGN-2026-08-24.md (B2).
//
// THE DEFECT THIS LANE EXISTS FOR (the user's second reported symptom, 2026-08-24: "он не может
// подбирать монеты хоста"). `[V]` TWO entries reach the coin's credit block
// `ExecuteUbergraph_baocoin:441`:
//   - the overlap `BndEvt__baocoin_collect_...` -> ubergraph 1871 -> cast -> Jump 441. PE-visible,
//     hooked since v137, and CANCELLABLE.
//   - `actionOptionIndex` -> 441 DIRECTLY. This is the E-press / radial interact, and `[V]`
//     mainPlayer's ubergraph @1022 dispatches it as EX_LocalVirtualFunction: PE-invisible AND
//     Func-invisible, so only the `0x45` substrate sees it -- and that substrate observes WITHOUT a
//     cancel primitive.
// v137 knew about the first only, and the field's six real credits produced ZERO lines from it: a
// seam that had never been observed firing at all. So a client's E-press credited ITSELF through an
// unhookable `lib_C::addPoints`, destroyed its own mirror, and the host's coin stayed on the ground
// while the next balance broadcast erased the phantom.
//
// THE SHAPE IS FORWARD-AND-RECONCILE, AND IT IS FORCED, NOT CHOSEN. A script UFunction reached via
// EX_Local* never reads `UFunction::Func` (docs/COOP_DISPATCH_VISIBILITY.md), so the E-press local
// credit CANNOT be suppressed -- it is a phantom the host's balance broadcast corrects. What we CAN
// do is forward the coin's identity so the host runs the coin's own verb on the authoritative
// instance. Act-as-host (COOP_SYNCER_MODEL §2b): the client authors the INTENT ("this coin was
// collected"), never the value. The overlap entry, being cancellable, is still cancelled -- but it
// forwards FIRST, which is the bug in v137's version: it cancelled and stopped there, so "a client
// never credits" was true and "the host credits" was not, because nothing told the host anything.
//
// Game thread throughout (the seams, the verb bracket and the wire receiver all run there).

#include "coop/items/coingun_sync.h"

#include "coingun_internal.h"   // co-located private header (src tree, not include/)

#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/player/players_registry.h"   // v140: IsLocal / IsPuppet -- WHO tripped the coin
#include "coop/net/session.h"
#include "coop/world/world_actor_sync.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/vm_dispatch.h"
#include "ue_wrap/world/economy.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <unordered_map>

namespace coop::coingun_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;
namespace vm = ue_wrap::vm_dispatch;
namespace I  = coop::coingun_sync::internal;

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

std::atomic<bool> g_installed{false};
std::atomic<bool> g_verbRegistered{false};

void* g_collectFn   = nullptr;   // Abaocoin_C's collect BndEvt (the PE-visible overlap pickup)
void* g_actionOptFn = nullptr;   // baocoin_C::actionOptionIndex -- the host's collect executor

// Per-ENTRY counters, so the log can answer the question v137 could not: which of the two entries do
// players actually use? `[V]` v137's overlap interceptor printed ZERO lines across six real credits,
// and a seam never observed firing is indistinguishable from a broken one.
std::atomic<unsigned long long> g_seenPress{0};      // the E-press / 0x45 entry
std::atomic<unsigned long long> g_seenOverlap{0};    // the BndEvt overlap entry
std::atomic<unsigned long long> g_forwarded{0};
std::atomic<unsigned long long> g_performed{0};
std::atomic<unsigned long long> g_unresolved{0};
std::atomic<unsigned long long> g_noCredit{0};

// ---- ONE QUESTION, ONE ANSWER (v140) -----------------------------------------------------------
// "Is this coin the host's?" had TWO predicates in v139 and they disagreed: OnCollectPre asked
// `IsMirroredActor || IsMaterializingMirror` (which world_actor_sync.h explicitly documents as THE
// test) while ForwardCollectToHost asked `IsMirroredActor` alone. A coin collected inside its own
// materialization window -- shoot a prop at your own feet and the coins land on you, and a mirror's
// delegates bind during BeginPlay INSIDE FinishSpawning, before the row is installed -- was therefore
// SUPPRESSED by the first and NOT FORWARDED by the second. Neither peer credited it. Two predicates
// for one question is the shape (`[[feedback-recurring-bug-is-architectural]]`); this is the one.
bool IsHostOwnedCoin(void* coin) {
    return coop::world_actor_sync::IsMirroredActor(coin) ||
           coop::world_actor_sync::IsMaterializingMirror();
}

// ---- the CLIENT half: forward -----------------------------------------------------------------
// Called from BOTH entries. Returns true iff a forward was actually sent, so the overlap entry can
// log honestly about what it cancelled.
bool ForwardCollectToHost(void* coin, const wchar_t* entry) {
    auto* s = I::Session();
    if (!s || !s->connected() || s->role() != coop::net::Role::Client) return false;
    if (!I::IsCoinActor(coin)) return false;

    // MIRROR-SCOPED, through the ONE predicate. A NON-mirror coin is one of the two cooked maps'
    // placed instances -- level content on both peers, never enrolled, never the host's. Forwarding
    // it would name an eid the host does not have; leaving it native keeps today's behaviour
    // (residual A13, see the header).
    if (!IsHostOwnedCoin(coin)) return false;

    // ONE IDENTITY SOURCE, in the right order. The actor's own row first; the materialization
    // window's eid ONLY as the fallback, because the window belongs to the actor being SPAWNED and
    // registering its collision can fire delegates on OTHER, already-mirrored actors it lands on --
    // and those have rows of their own, so asking the actor first is what keeps the fallback honest.
    coop::element::ElementId eid = coop::element::Registry::Get().EidForActor(coin);
    const wchar_t* idFrom = L"row";
    if (eid == coop::element::kInvalidId || eid == 0u) {
        if (const unsigned int born = coop::world_actor_sync::MaterializingEid()) {
            eid    = static_cast<coop::element::ElementId>(born);
            idFrom = L"materializing";
        }
    }
    if (eid == coop::element::kInvalidId || eid == 0u) {
        // Loud: a mirror with no eid AND no open window means the actor->eid reverse and the mirror
        // set disagree, which is an identity fault, not a routine miss. Without this the collect
        // would silently do nothing and look exactly like the bug this lane exists to fix.
        UE_LOGE("coingun[collect seam]: %ls entry on MIRROR coin=%p that has NO element id and no "
                "open materialization window -- cannot forward. The mirror set and the actor->eid "
                "reverse disagree about this actor; the credit will stay local and be erased by the "
                "host's next balance broadcast.", entry, coin);
        return false;
    }

    coop::net::CoinCollectPayload p{};
    p.elementId = static_cast<uint32_t>(eid);
    s->SendReliable(coop::net::ReliableKind::CoinCollect, &p, sizeof(p));
    g_forwarded.fetch_add(1, std::memory_order_relaxed);
    // THE PROOF LINE. Greppable on purpose: a re-run's verdict is a one-line grep for
    // "coingun[collect seam]" rather than a judgement call about whether a seam fired.
    UE_LOGI("coingun[collect seam] entry=%ls ctx=%p forwarding eid=%u (id from %ls) -- the host "
            "performs the collect on its own coin; our local credit (if the entry allowed one) is a "
            "phantom the balance broadcast corrects", entry, coin, p.elementId, idFrom);
    return true;
}

// ---- ENTRY 1: the overlap (PE-visible, cancellable) --------------------------------------------
// One PE interceptor serving both roles.
//   HOST: never cancels. Observes so the log can distinguish "no overlap ever fired" from "overlap
//         fired, credit refused" -- without that a null result in the collect cells is
//         uninterpretable.
//   CLIENT: forwards then cancels for a MIRROR (or a coin still materializing -- the collect
//         delegate binds during BeginPlay, i.e. INSIDE FinishSpawning, before the lane installs the
//         mirror row, so the row can never be the sole discriminator). Leaves a NON-mirror coin
//         native: the two maps' placed coins would otherwise become permanently uncollectable AND
//         ghosted, a new loss.
bool OnCollectPre(void* self, void* params) {
    if (!I::IsCoinActor(self)) return false;

    auto* s = I::Session();
    if (!s || !s->connected()) return false;     // solo: the native path is correct

    // The delegate signature is (UPrimitiveComponent* Overlapped, AActor* OtherActor, ...), so the
    // tripping actor is the second pointer-sized param.
    void* other = nullptr;
    if (params) other = *reinterpret_cast<void**>(static_cast<uint8_t*>(params) + sizeof(void*));

    if (s->role() == coop::net::Role::Host) {
        int32_t pts = -1;
        const int32_t off = I::CoinPointsOffset();
        if (off >= 0)
            pts = *reinterpret_cast<const int32_t*>(static_cast<const uint8_t*>(self) + off);
        UE_LOGI("coingun[host collect]: coin=%p points=%d TRIPPED BY actor=%p class='%ls' -- the "
                "native credit runs, balance_sync will broadcast the new total",
                self, pts, other, other ? R::ClassNameOf(other).c_str() : L"<null>");
        return false;                            // observe only; the host credits natively
    }

    // ---- client ----
    g_seenOverlap.fetch_add(1, std::memory_order_relaxed);

    // WHO TRIPPED IT decides what we may do, and the authority for that question is the GAME's own
    // gate, not ours. `[V]` the coin's overlap path is BndEvt -> ExecuteUbergraph_baocoin @1871
    // `cast<mainPlayer_C>(OtherActor)` -> IFNOT POP: a non-player overlap credits NOBODY natively.
    // Our interceptor runs BEFORE that cast, so without mirroring it we were answering a broader
    // question than the game asks -- and a sale spawns ~47 coins in one spot, so coin-on-coin
    // overlaps are not hypothetical.
    const bool byLocal  = other && coop::players::Registry::Get().IsLocal(other);
    const bool byPuppet = other && coop::players::Registry::Get().IsPuppet(other);

    if (!byLocal && !byPuppet) {
        // Not a player at all. The native path bails at the cast, so there is nothing to suppress and
        // nothing to forward -- cancelling here would be us INVENTING behaviour the game does not
        // have. Leave it entirely alone.
        return false;
    }

    if (IsHostOwnedCoin(self)) {
        if (byPuppet) {
            // Another peer's body tripped OUR mirror of the host's coin. Suppress -- a puppet IS a
            // mainPlayer_C, so the native path would credit THIS client for someone else's pickup --
            // but do NOT forward: that peer's own client forwards its own collect (or, if the puppet
            // is the host's, the host already credited natively). Forwarding here would make one
            // pickup arrive at the host from every peer that can see it.
            UE_LOGI("coingun[client collect]: SUPPRESSED on mirror coin=%p tripped by PUPPET %p -- no "
                    "forward: the collecting peer authors its own. Cancelled because a puppet is a "
                    "mainPlayer_C and the native cast would otherwise credit US for their pickup.",
                    self, other);
            return true;
        }
        // FORWARD FIRST, THEN CANCEL -- after `return true` there is no seam left.
        const bool fwd = ForwardCollectToHost(self, L"overlap");
        UE_LOGI("coingun[client collect]: SUPPRESSED on mirror coin=%p (tripped by LOCAL %p), "
                "forwarded=%d. The local credit is cancelled outright on THIS entry (unlike the "
                "E-press, which is EX_LocalVirtualFunction and uncancellable); the host performs the "
                "collect on its own coin.", self, other, fwd ? 1 : 0);
        return true;                             // cancel: no local addPoints, no local destroy
    }

    if (byPuppet) {
        // A map-placed coin tripped by someone else's body. Native would credit us for their pickup,
        // which is wrong for the same reason as above and is not the A13 residual (that one is about
        // OUR OWN pickup of level content). Suppress, forward nothing.
        UE_LOGI("coingun[client collect]: SUPPRESSED on NON-mirror coin=%p tripped by PUPPET %p -- a "
                "map-placed coin is level content on both peers, and their body picking it up is "
                "their event, not ours.", self, other);
        return true;
    }

    UE_LOGW("coingun[client collect]: NON-MIRROR coin=%p collected locally (a map-placed coin -- `[V]` "
            "two cooked maps carry them, and they are level content on both peers, never enrolled). "
            "This credits THIS CLIENT only. If this player's puppet also trips the host's copy, the "
            "host's broadcast overwrites the number shortly; if it does not, this number is wrong "
            "until the host's balance next moves. Pre-existing (A13), deliberately not cancelled.",
            self);
    return false;
}

// ---- ENTRY 2: the E-press (0x45, observe-only) -------------------------------------------------
void OnCollectVerb(const vm::Bracket& b) {
    // `actionOptionIndex` is the interaction entry point of MANY classes, and `vm_dispatch` matches
    // on NAME alone and says so in its header: "any further class/authority discrimination is the
    // CONSUMER's job". The ctx class check IS that discrimination -- without it every radial/E
    // interaction in the world would land here.
    if (!I::IsCoinActor(b.ctx)) return;

    auto* s = I::Session();
    if (!s || !s->connected()) return;            // solo: the native credit is correct

    g_seenPress.fetch_add(1, std::memory_order_relaxed);

    if (s->role() == coop::net::Role::Host) {
        // The host's own press. Observe only -- the native credit is authoritative and already runs.
        // This is the POSITIVE CONTROL for the client's silence: without a line that fires where the
        // observer is EXPECTED to fire, a quiet client log is indistinguishable from a dead hook
        // (`[[lesson-an-instrument-blind-to-the-phenomenon-always-passes]]`, which is exactly how
        // v137's overlap interceptor passed review while never having fired). Note this does NOT
        // count the host's own performed collects below: those are dispatched by ProcessEvent, which
        // does not route through GNatives[0x45], so there is no echo to subtract.
        UE_LOGI("coingun[collect seam] entry=press HOST ctx=%p -- native credit runs here; this line "
                "proves the 0x45 bracket is live in this session", b.ctx);
        return;
    }

    // WE CANNOT CANCEL THIS -- see the file header. The local credit and the local self-destroy of
    // our mirror WILL happen; the phantom credit is corrected by the host's balance broadcast, and
    // the mirror's disappearance by the host's own WorldActorDestroy (or, if the host could not
    // perform it, by the re-announce below landing on the stale-row guard).
    ForwardCollectToHost(b.ctx, L"press");
}

}  // namespace

// ---- the HOST half: perform --------------------------------------------------------------------
void OnCoinCollect(const uint8_t* payload, int len, uint8_t senderSlot, void* localPlayer) {
    auto* s = I::Session();
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
        g_unresolved.fetch_add(1, std::memory_order_relaxed);
        UE_LOGI("coingun[host collect]: slot=%u eid=%u does not resolve to a live WorldActor -- the "
                "coin is already gone (collected by someone else, or destroyed). Nothing to do.",
                senderSlot, p.elementId);
        return;
    }
    // CLASS gate. The eid resolving is not enough: a WorldActor eid could name a piramid or a wisp,
    // and dispatching `actionOptionIndex` on one of those would run an unrelated interaction.
    if (!I::IsCoinActor(coin)) {
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
        g_performed.fetch_add(1, std::memory_order_relaxed);
        UE_LOGI("coingun[host collect]: PERFORMED slot=%u eid=%u coin=%p -- balance %d -> %d (+%d). "
                "The coin's own verb ran, so its native credit and self-destroy are the game's, not "
                "ours; the WorldActorDestroy that follows removes every peer's mirror.",
                senderSlot, p.elementId, coin, before, after, after - before);
        return;
    }

    // THE COIN IS LIVE AND SOMETHING WENT WRONG. The forwarding client has already destroyed its own
    // mirror on the E-press path (that entry is uncancellable), so leaving this alone makes the
    // host's coin permanently invisible to that peer -- a NEW loss, authored by the fix.
    g_noCredit.fetch_add(1, std::memory_order_relaxed);

    if (!haveBefore || !haveAfter) {
        // WE DO NOT KNOW, AND NOT KNOWING IS NOT THE SAME AS "NO CREDIT". Treating an unreadable
        // balance as a failed collect would (a) act on absence of evidence -- the fail-open family
        // this whole pass keeps catching, inverted -- and (b) fire a full world re-announce on EVERY
        // collect for as long as the read stayed broken, turning a diagnostic into a burst storm.
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

bool internal::CollectInstalled() { return g_installed.load(std::memory_order_acquire); }

void internal::InstallCollect() {
    if (g_installed.load(std::memory_order_acquire)) return;

    // The sale lane's Install owns class resolution and already retries at ~1 Hz; stay inert until
    // it lands rather than running a second GUObjectArray walk on the same tick.
    void* coinClass = internal::CoinClass();
    if (!coinClass) return;

    if (!g_verbRegistered.load(std::memory_order_acquire)) {
        if (vm::RegisterVirtualVerb(kVerbNameCollect, kVerbCoinCollect, &OnCollectVerb)) {
            g_verbRegistered.store(true, std::memory_order_release);
            UE_LOGI("coingun[collect]: registered the 0x45 verb '%ls' (id=%d) -- the E-press entry",
                    kVerbNameCollect, kVerbCoinCollect);
        }
    }
    vm::TickResolvePending();

    if (!g_collectFn)
        g_collectFn = R::FindFunction(coinClass,
            L"BndEvt__baocoin_collect_K2Node_ComponentBoundEvent_1_ComponentBeginOverlapSignature__DelegateSignature");
    if (!g_collectFn) return;   // retry next tick

    if (!GT::RegisterInterceptor(g_collectFn, &OnCollectPre)) {
        UE_LOGE("coingun[collect]: overlap interceptor install FAILED -- the overlap entry is BLIND. "
                "The E-press entry is unaffected (it rides the 0x45 verb).");
        return;
    }
    g_installed.store(true, std::memory_order_release);
    UE_LOGI("coingun[collect]: installed -- both entries live (0x45 '%ls' + the overlap BndEvt "
            "interceptor)", kVerbNameCollect);
}

void internal::OnDisconnectCollect() {
    // The free measurement, per lane. `[[lesson-your-own-session-end-summary-is-a-free-measurement]]`:
    // the two entry counters are here so the next field run answers BY GREP which entry players
    // actually use -- the question v137 could not answer about its own seam.
    UE_LOGI("coingun[collect]: SESSION SUMMARY -- entries{press=%llu overlap=%llu} forwarded=%llu "
            "host{performed=%llu unresolved=%llu noCredit=%llu}",
            g_seenPress.load(std::memory_order_relaxed),
            g_seenOverlap.load(std::memory_order_relaxed),
            g_forwarded.load(std::memory_order_relaxed),
            g_performed.load(std::memory_order_relaxed),
            g_unresolved.load(std::memory_order_relaxed),
            g_noCredit.load(std::memory_order_relaxed));
    // Nothing world-scoped is held: g_actionOptFn is a UFunction (not world-scoped, CLAUDE.md 4j)
    // and the repair throttle is a timestamp map keyed by slot, which a new session may reuse
    // safely -- a stale 2 s window can only DELAY a repair, never cause one.
}

}  // namespace coop::coingun_sync
