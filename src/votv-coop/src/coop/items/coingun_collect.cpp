// coop/items/coingun_collect.cpp -- the collect lane: somebody picks a coin up, and the host
// credits. Two entries reach the coin's credit block: the overlap delegate
// (ProcessEvent-visible, hooked and cancellable) and actionOptionIndex, the E-press, which
// mainPlayer dispatches as EX_LocalVirtualFunction (invisible to ProcessEvent and to a Func
// hook; only the 0x45 verb substrate observes it, and it cannot cancel). So the shape is
// forward-and-reconcile: the client forwards the coin's identity and the host runs the coin's
// own verb on the authoritative instance; the E-press's local credit is a phantom the host's
// balance broadcast corrects, and the overlap entry forwards first and then cancels. Game
// thread throughout.

#include "coop/items/coingun_sync.h"

#include "coingun_internal.h"   // co-located private header (src tree, not include/)

#include "coop/element/intent_authority.h"   // may this sender name this coin
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/player/players_registry.h"   // IsLocal, IsPuppet: who tripped the coin
#include "coop/net/session.h"
#include "coop/world/world_actor_sync.h"

#include "ue_wrap/core/cached_obj_ref.h"
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

// The one option baocoin_C::getActionOptions offers, a compile-time constant; actionOptionIndex
// never branches on `action`, so deriving it at runtime would cost a dispatch and a leaked
// engine array per collect for a value the callee never reads.
constexpr uint8_t kCoinCollectAction = 7;

std::atomic<bool> g_installed{false};
std::atomic<bool> g_verbRegistered{false};

void* g_collectFn   = nullptr;   // Abaocoin_C's collect BndEvt (the PE-visible overlap pickup)
void* g_actionOptFn = nullptr;   // baocoin_C::actionOptionIndex -- the host's collect executor

// Per-entry counters, split by role so two numbers printed together count the same population:
// which of the two entries do players use?
std::atomic<unsigned long long> g_seenPressHost{0};      // the E-press / 0x45 entry, host side
std::atomic<unsigned long long> g_seenPressClient{0};    //   ... client side
std::atomic<unsigned long long> g_seenOverlapHost{0};    // the BndEvt overlap entry, host side
std::atomic<unsigned long long> g_seenOverlapClient{0};  //   ... client side
std::atomic<unsigned long long> g_forwarded{0};
std::atomic<unsigned long long> g_performed{0};
std::atomic<unsigned long long> g_unresolved{0};
std::atomic<unsigned long long> g_noCredit{0};
std::atomic<unsigned long long> g_replayed{0};

// The consumption guard. "The coin will be dead next time" holds only if the credit block's
// K2_DestroyActor takes effect within our dispatch, and that timing is not measured; the
// reliable inbox drains unbounded per tick, so two CoinCollect packets naming one eid in a frame
// would credit twice. Keyed by eid, valued by a world-stamped CachedObjRef, so the guard
// compares identity and an entry reads dead once the coin or its world is gone.
std::unordered_map<uint32_t, ue_wrap::CachedObjRef> g_collected;

// "Is this coin the host's?" has one predicate. A coin collected inside its own materialisation
// window (shoot a prop at your feet and the coins land on you; a mirror's delegates bind during
// BeginPlay inside FinishSpawning, before the row is installed) must answer yes, and the second
// term is identity-checked: a bare "is some mirror being born" would judge a map-placed coin
// tripped during any materialisation as host-owned and forward it under the other actor's eid.
bool IsHostOwnedCoin(void* coin) {
    if (coop::world_actor_sync::IsMirroredActor(coin)) return true;
    return coin != nullptr && coop::world_actor_sync::MaterializingActor() == coin;
}

// The client half: forward. Called from both entries; true iff a forward was sent, so the
// overlap entry can log what it cancelled.
bool ForwardCollectToHost(void* coin, const wchar_t* entry) {
    auto* s = I::Session();
    if (!s || !s->connected() || s->role() != coop::net::Role::Client) return false;
    if (!I::IsCoinActor(coin)) return false;

    // Mirror-scoped: a non-mirror coin is one of the maps' placed instances, level content on both
    // peers and never enrolled, so forwarding it would name an eid the host does not have. Left
    // native.
    if (!IsHostOwnedCoin(coin)) return false;

    // One identity source, in order: the actor's own row first, the materialisation window's eid
    // only as the fallback, since registering the spawning actor's collision can fire delegates on
    // other, already-mirrored actors it lands on, and those have rows of their own.
    coop::element::ElementId eid = coop::element::Registry::Get().EidForActor(coin);
    const wchar_t* idFrom = L"row";
    if (eid == coop::element::kInvalidId || eid == 0u) {
        // The window's eid may name this coin only if this coin is the actor being spawned.
        if (coop::world_actor_sync::MaterializingActor() == coin) {
            if (const unsigned int born = coop::world_actor_sync::MaterializingEid()) {
                eid    = static_cast<coop::element::ElementId>(born);
                idFrom = L"materializing";
            }
        }
    }
    if (eid == coop::element::kInvalidId || eid == 0u) {
        // Loud: a mirror with no eid and no open window means the reverse map and the mirror set
        // disagree, an identity fault; silent, it would look exactly like the bug this lane fixes.
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
    // The proof line, greppable: "coingun[collect seam]".
    UE_LOGI("coingun[collect seam] entry=%ls ctx=%p forwarding eid=%u (id from %ls) -- the host "
            "performs the collect on its own coin; our local credit (if the entry allowed one) is a "
            "phantom the balance broadcast corrects", entry, coin, p.elementId, idFrom);
    return true;
}

// Entry 1, the overlap: one interceptor for both roles. The host never cancels; it observes so
// the log can tell "no overlap fired" from "fired, credit refused". The client forwards and
// then cancels for a mirror or a coin still materialising, and leaves a non-mirror coin native,
// since the maps' placed coins would otherwise become uncollectable and ghosted.
bool OnCollectPre(void* self, void* params) {
    if (!I::IsCoinActor(self)) return false;

    auto* s = I::Session();
    if (!s || !s->connected()) return false;     // solo: the native path is correct

    // The delegate signature is (UPrimitiveComponent* Overlapped, AActor* OtherActor, ...): the
    // tripping actor is the second pointer.
    void* other = nullptr;
    if (params) other = *reinterpret_cast<void**>(static_cast<uint8_t*>(params) + sizeof(void*));

    if (s->role() == coop::net::Role::Host) {
        // Through the module's one reader of the points field.
        const int32_t pts = ReadCoinPoints(self);
        UE_LOGI("coingun[host collect]: coin=%p points=%d TRIPPED BY actor=%p class='%ls' -- the "
                "native credit runs, balance_sync will broadcast the new total",
                self, pts, other, other ? R::ClassNameOf(other).c_str() : L"<null>");
        g_seenOverlapHost.fetch_add(1, std::memory_order_relaxed);
        return false;                            // observe only; the host credits natively
    }

    // The client.
    g_seenOverlapClient.fetch_add(1, std::memory_order_relaxed);

    // Who tripped it decides what may be done, and the game's own gate is the authority: the coin's
    // overlap path casts OtherActor to mainPlayer_C and credits nobody on a miss. This interceptor
    // runs before that cast, and a sale spawns dozens of coins in one spot, so coin-on-coin
    // overlaps are real.
    const bool byLocal  = other && coop::players::Registry::Get().IsLocal(other);
    const bool byPuppet = other && coop::players::Registry::Get().IsPuppet(other);

    if (!byLocal && !byPuppet) {
        // Not a player: the native path bails at the cast, so there is nothing to suppress or
        // forward.
        return false;
    }

    if (IsHostOwnedCoin(self)) {
        if (byPuppet) {
            // Another peer's body tripped our mirror of the host's coin. Suppressed, since a puppet
            // is a mainPlayer_C and the native path would credit this client for their pickup; not
            // forwarded, since that peer forwards its own (or the host already credited natively),
            // and a forward from every peer that can see it would deliver one pickup many times.
            UE_LOGI("coingun[client collect]: SUPPRESSED on mirror coin=%p tripped by PUPPET %p -- no "
                    "forward: the collecting peer authors its own. Cancelled because a puppet is a "
                    "mainPlayer_C and the native cast would otherwise credit US for their pickup.",
                    self, other);
            return true;
        }
        // Forward first, then cancel: after `return true` there is no seam left.
        const bool fwd = ForwardCollectToHost(self, L"overlap");
        UE_LOGI("coingun[client collect]: SUPPRESSED on mirror coin=%p (tripped by LOCAL %p), "
                "forwarded=%d. The local credit is cancelled outright on THIS entry (unlike the "
                "E-press, which is EX_LocalVirtualFunction and uncancellable); the host performs the "
                "collect on its own coin.", self, other, fwd ? 1 : 0);
        return true;                             // cancel: no local addPoints, no local destroy
    }

    // Our own pre-barrier coins: the ones our shot just spawned, held by the barrier. Crediting for
    // them is a phantom whenever the shot authors a sale (the host mints the real ones).
    // Suppressed, nothing forwarded (there is no eid to name); if the shot authors nothing, the
    // barrier releases the coin and it is pickable again.
    if (I::IsCapturedCoin(self)) {
        UE_LOGI("coingun[client collect]: SUPPRESSED on OUR OWN pre-barrier coin=%p (tripped by %p) "
                "-- our shot spawned it and the barrier still holds it. If the shot authors a sale "
                "the host mints the real coins; if it does not, the barrier releases this one and it "
                "is pickable again.", self, other);
        return true;
    }

    if (byPuppet) {
        // A map-placed coin tripped by someone else's body: their event, not ours. Suppressed,
        // nothing forwarded.
        UE_LOGI("coingun[client collect]: SUPPRESSED on NON-mirror coin=%p tripped by PUPPET %p -- a "
                "map-placed coin is level content on both peers, and their body picking it up is "
                "their event, not ours.", self, other);
        return true;
    }

    UE_LOGW("coingun[client collect]: NON-MIRROR coin=%p collected locally (a map-placed coin: two "
            "cooked maps carry them, they are level content on both peers and never enrolled). "
            "This credits THIS CLIENT only. If this player's puppet also trips the host's copy, the "
            "host's broadcast overwrites the number shortly; if it does not, this number is wrong "
            "until the host's balance next moves. Pre-existing (A13), deliberately not cancelled.",
            self);
    return false;
}

// Entry 2: the E-press, through the 0x45 verb, observe-only.
void OnCollectVerb(const vm::Bracket& b) {
    // actionOptionIndex is the interaction entry of many classes, and vm_dispatch matches on name
    // alone; the class check is the consumer's discrimination.
    if (!I::IsCoinActor(b.ctx)) return;

    auto* s = I::Session();
    if (!s || !s->connected()) return;            // solo: the native credit is correct

    if (s->role() == coop::net::Role::Host) {
        g_seenPressHost.fetch_add(1, std::memory_order_relaxed);
        // The host's own press: observe only, the native credit runs. The positive control for the
        // client's silence: without a line where the observer is expected to fire, a quiet client
        // log is indistinguishable from a dead hook. The host's performed collects below are
        // dispatched by ProcessEvent, not through GNatives[0x45], so there is no echo to subtract.
        UE_LOGI("coingun[collect seam] entry=press HOST ctx=%p -- native credit runs here; this line "
                "proves the 0x45 bracket is live in this session", b.ctx);
        return;
    }

    g_seenPressClient.fetch_add(1, std::memory_order_relaxed);

    // Uncancellable (see the header): the local credit and the local self-destroy of our mirror
    // happen; the phantom credit is corrected by the balance broadcast, and the mirror's absence by
    // the host's WorldActorDestroy, or by the re-announce below.
    ForwardCollectToHost(b.ctx, L"press");
}

}  // namespace

// The host half: perform.
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

    // The eid range: the host band only. A baocoin_C is a host-minted WorldActor with no save key,
    // spawned by the host's own sell and announced with this exact eid, so an id outside the host's
    // band cannot name a coin.
    if (!coop::element::Registry::IsAllowedHostAllocatedEid(p.elementId)) {
        static uint32_t sBad[coop::players::kMaxPeers] = {};
        const uint32_t n = ++sBad[senderSlot < coop::players::kMaxPeers ? senderSlot : 0u];
        if (n <= 5 || (n <= 100 && n % 10 == 0) || n % 100 == 0)
            UE_LOGW("coingun[host]: CoinCollect #%u from slot=%u names eid=0x%08x, which is not in "
                    "the HOST allocation band -- dropping. A coin is host-minted by construction, so "
                    "no other band can name one. (Rate-latched: this receiver is reachable from the "
                    "trust boundary at whatever rate a sender likes.)", n, senderSlot, p.elementId);
        return;
    }

    // Resolved against our own registry, fail-closed on type, and gated on reach: may this sender
    // name this coin? The reach is mainPlayer.armLength, 200 units, the game's own reach for
    // picking a coin up. A refusal costs a retry, never an item: the client's phantom credit is
    // corrected by the next balance broadcast either way, and the coin stays on the ground for
    // whoever stands near it.
    constexpr float kCollectReachUU = 200.0f;
    const auto tok = coop::element::IntentTarget::ForClientIntent(*s, senderSlot, kCollectReachUU);
    const coop::element::IntentSubject sub = tok.Resolve(
        static_cast<coop::element::ElementId>(p.elementId), coop::element::ElementType::WorldActor);
    if (sub.outcome == coop::element::IntentOutcome::OutOfReach ||
        sub.outcome == coop::element::IntentOutcome::NoBody) {
        UE_LOGW("coingun[host collect]: REFUSED slot=%u eid=%u -- REASON=%s (dist=%.0f allowed=%.0f). "
                "'no-body' means the sender has no live puppet on the host, so there is no body to "
                "measure a reach from and we refuse rather than assume one -- the same fail-closed "
                "answer the sale lane gives. The coin stays where it is.",
                senderSlot, p.elementId, coop::element::OutcomeName(sub.outcome),
                sub.distUU, sub.reachUU);
        return;
    }
    void* coin = sub ? sub.actor : nullptr;
    if (!coin) {
        // The host's registry is authoritative for its own eids, so no live actor under this eid
        // means the coin is gone: somebody else collected it, the ordinary outcome of two players
        // reaching for one coin.
        g_unresolved.fetch_add(1, std::memory_order_relaxed);
        UE_LOGI("coingun[host collect]: slot=%u eid=%u does not resolve to a live WorldActor -- the "
                "coin is already gone (collected by someone else, or destroyed). Nothing to do.",
                senderSlot, p.elementId);
        return;
    }
    // The consumption guard: was a collect already performed for this exact coin?
    {
        auto it = g_collected.find(p.elementId);
        if (it != g_collected.end() && it->second.Get() == coin) {
            g_replayed.fetch_add(1, std::memory_order_relaxed);
            UE_LOGW("coingun[host collect]: REFUSED slot=%u eid=%u -- already collected, and the coin "
                    "has not left the world yet. A replayed or duplicated forward credits nothing.",
                    senderSlot, p.elementId);
            return;
        }
        // A bounded sweep: the map holds coins credited for that have not finished dying, normally
        // none or one. Erased here and cleared whole on disconnect.
        for (auto e = g_collected.begin(); e != g_collected.end();) {
            if (e->second.Get() == nullptr) e = g_collected.erase(e);
            else                            ++e;
        }
    }

    // The class gate: a WorldActor eid could name a pyramid or a wisp, and actionOptionIndex on one
    // of those would run an unrelated interaction.
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

    // The balance is read around the dispatch, the only way to know the collect took: the credit is
    // lib_C::addPoints, EX_LocalVirtualFunction, with no return value and no seam. A silent no-op
    // would leave the client's mirror destroyed while the host's coin lives on.
    int32_t before = 0;
    const bool haveBefore = ue_wrap::economy::ReadPoints(&before);

    ue_wrap::ParamFrame f(g_actionOptFn);
    if (!f.valid()) {
        UE_LOGE("coingun[host collect]: slot=%u eid=%u -- actionOptionIndex frame invalid",
                senderSlot, p.elementId);
        return;
    }
    // `player` is the host's own mainPlayer: the credit block is reached from the overlap entry
    // too, which never writes the player, so it cannot depend on it, and the truthful value is
    // right either way. `hit` stays zeroed and `lookAtComponent` null, as on the overlap entry;
    // `action` is inert (kCoinCollectAction).
    f.Set<void*>(L"player", localPlayer);
    f.Set<uint8_t>(L"action", kCoinCollectAction);
    const bool dispatched = ue_wrap::Call(coin, f);

    int32_t after = 0;
    const bool haveAfter = ue_wrap::economy::ReadPoints(&after);
    const bool credited  = haveBefore && haveAfter && after != before;

    if (dispatched && credited) {
        g_performed.fetch_add(1, std::memory_order_relaxed);
        g_collected[p.elementId].Set(coin);   // consumed
        UE_LOGI("coingun[host collect]: PERFORMED slot=%u eid=%u coin=%p -- balance %d -> %d (+%d). "
                "The coin's own verb ran, so its native credit and self-destroy are the game's, not "
                "ours; the WorldActorDestroy that follows removes every peer's mirror.",
                senderSlot, p.elementId, coin, before, after, after - before);
        return;
    }

    // The coin is live and something went wrong. The forwarding client already destroyed its own
    // mirror on the E-press path, so leaving this alone makes the host's coin invisible to that
    // peer for good.
    g_noCredit.fetch_add(1, std::memory_order_relaxed);

    if (!haveBefore || !haveAfter) {
        // Unknown is not "no credit": treating an unreadable balance as a failed collect would act
        // on absence of evidence, and fire a world re-announce on every collect while the read
        // stayed broken.
        UE_LOGE("coingun[host collect]: slot=%u eid=%u coin=%p dispatch=%d -- the balance was NOT "
                "readable (before=%d after=%d), so whether this credited is UNKNOWN. Taking no "
                "repair action: acting on an unreadable value would be inventing a verdict.",
                senderSlot, p.elementId, coin, dispatched ? 1 : 0, haveBefore ? 1 : 0,
                haveAfter ? 1 : 0);
        return;
    }

    // Known: the coin lives and the balance did not move. The world is re-announced to that slot so
    // the mirror comes back; the stale-row guard in world_actor_mirror lets that announce land on a
    // row whose actor the client already killed. Throttled per slot, since the announce carries
    // every world actor.
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

    // The sale lane's Install owns class resolution and retries at about 1 Hz; inert until it lands
    // rather than a second GUObjectArray walk on the same tick.
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
    // The session summary: the two entry counters, so the next field run answers by grep which
    // entry players use. The labels follow the role, so the first pair is always the comparable
    // one.
    auto* sess = I::Session();
    const bool asHost = sess && sess->role() == coop::net::Role::Host;
    UE_LOGI("coingun[collect]: SESSION SUMMARY (%ls) -- thisPeer{press=%llu overlap=%llu} "
            "otherRole{press=%llu overlap=%llu} forwarded=%llu "
            "host{performed=%llu unresolved=%llu noCredit=%llu replayed=%llu} -- the FIRST pair is the "
            "comparable one: both entries counted on the same side.",
            asHost ? L"host" : L"client",
            (asHost ? g_seenPressHost   : g_seenPressClient).load(std::memory_order_relaxed),
            (asHost ? g_seenOverlapHost : g_seenOverlapClient).load(std::memory_order_relaxed),
            (asHost ? g_seenPressClient : g_seenPressHost).load(std::memory_order_relaxed),
            (asHost ? g_seenOverlapClient : g_seenOverlapHost).load(std::memory_order_relaxed),
            g_forwarded.load(std::memory_order_relaxed),
            g_performed.load(std::memory_order_relaxed),
            g_unresolved.load(std::memory_order_relaxed),
            g_noCredit.load(std::memory_order_relaxed),
            g_replayed.load(std::memory_order_relaxed));
    // Nothing world-scoped is held apart from g_collected, which goes whole: g_actionOptFn is a
    // UFunction, and the repair throttle is a timestamp map by slot that a new session may reuse (a
    // stale window can only delay a repair). A consumption guard keyed on a dead world's eids must
    // not survive into a new session.
    g_collected.clear();
}

}  // namespace coop::coingun_sync
