// coop/props/pack_trash_intent.cpp -- see coop/props/pack_trash_intent.h.

#include "coop/props/pack_trash_intent.h"

#include "coop/element/element.h"
#include "coop/element/intent_authority.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/props/active_drive.h"
#include "coop/props/prop_snapshot.h"
#include "coop/props/remote_prop.h"
#include "coop/props/trash_channel.h"

#include "ue_wrap/actors/garbage_bag.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/engine/engine_pawn.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <iterator>
#include <unordered_map>
#include <vector>

namespace coop::pack_trash_intent {
namespace {

namespace E  = ue_wrap::engine;
namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;

// The verb every hand-usable tool declares, so the watch is by name and the callback gates on the
// tool's class -- the coin gun's lane learned that the hard way.
const wchar_t* const kHandUseVerb = L"playerHandUse_LMB";
constexpr int kTagPackTrash = 0x5041434b;  // 'PACK'

// A pack is an arm's-length hand use, so the sender's reach is the pile grab's: a target further
// than this from the sender's body is not theirs to bag.
constexpr float kPackReachUU = 400.0f;

// One press is one bag, and a stalled client's backlog must neither stall a frame nor be refused
// on arrival: queue per peer, spend a token a tick (the broom stroke's convention).
constexpr float    kPackBurst      = 3.0f;
constexpr float    kPacksPerSecond = 3.0f;
constexpr size_t   kMaxPending     = 8;
constexpr uint64_t kRefusalSayMs   = 10000;

// A target of the wrong type is answered with the truth about it, not with a destroy, and not more
// often than this per element.
constexpr uint64_t kRemedyDebounceMs = 5000;

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_watchInstalled = false;
bool g_watchLive = false;
// The two tool classes, produced by Install under its throttle and only COMPARED in the callback.
// Null until the world holds one: the bags are ordinary props, so a base map may well not.
void* g_foldCls = nullptr;
void* g_rollCls = nullptr;

// ---- client side -----------------------------------------------------------------------------
// The tools this peer's own presses spent, consumed from Tick rather than inside the gate
// callback: the body we refuse belongs to the tool, and destroying it from under the caller's
// frame is a hazard the next tick does not have.
struct SpentTool {
    void*   actor = nullptr;
    int32_t idx = -1;
    bool    roll = false;
};
std::vector<SpentTool> g_spent;
uint64_t g_sent = 0, g_refusedGhost = 0;

// ---- host side -------------------------------------------------------------------------------
struct Bucket {
    float    tokens = kPackBurst;
    uint64_t lastMs = 0;
    uint64_t nextSayMs = 0;
};
Bucket g_rate[coop::net::kMaxPeers];
std::deque<coop::net::PackTrashIntentPayload> g_pending[coop::net::kMaxPeers];
std::unordered_map<uint32_t, uint64_t> g_remedyAtMs;
uint64_t g_packed = 0, g_denied = 0;

bool TakeToken(uint8_t slot) {
    Bucket& b = g_rate[slot];
    const uint64_t now = coop::active_drive::NowMs();
    if (b.lastMs != 0 && now > b.lastMs) {
        b.tokens += kPacksPerSecond * static_cast<float>(now - b.lastMs) / 1000.0f;
        if (b.tokens > kPackBurst) b.tokens = kPackBurst;
    }
    b.lastMs = now;
    if (b.tokens < 1.0f) return false;
    b.tokens -= 1.0f;
    return true;
}

// The truth about a target that is not what the sender named, at most once per element per window.
void RemedyOnce(void* actor, coop::element::ElementId eid) {
    const uint64_t now = coop::active_drive::NowMs();
    auto it = g_remedyAtMs.find(eid);
    if (it != g_remedyAtMs.end() && now - it->second < kRemedyDebounceMs) return;
    // Drop what the window has passed, so the debounce ledger stays the size of what is actually
    // being refused rather than of every id ever refused this session.
    for (auto e = g_remedyAtMs.begin(); e != g_remedyAtMs.end();)
        e = (now - e->second >= kRemedyDebounceMs) ? g_remedyAtMs.erase(e) : std::next(e);
    g_remedyAtMs[eid] = now;
    coop::prop_snapshot::ExpressIncrementalSpawn(actor);
}

// The host's own pack: the bag the game would have spawned, then the target's destroy. Both ride
// the existing prop seams (the finish-spawn watcher and the destroy seam), so nothing is
// broadcast by hand here.
bool PerformPack(void* target, uint8_t slot, coop::element::ElementId eid) {
    void* bagCls = ue_wrap::garbage_bag::FilledClass();
    if (!bagCls) {
        UE_LOGW("[PACK-TRASH] prop_garbageBag_C not loaded -- cannot pack eid=%u", eid);
        return false;
    }
    const uint8_t chipType = ue_wrap::prop::GetChipType(target);
    ue_wrap::FVector loc{};
    if (!E::TryGetActorLocation(target, loc)) {
        UE_LOGW("[PACK-TRASH] the pile's location could not be read for eid=%u -- not packed", eid);
        return false;
    }
    const ue_wrap::FRotator rot = E::GetActorRotation(target);
    void* bag = E::BeginDeferredSpawn(bagCls, loc, rot);
    if (!bag) {
        UE_LOGW("[PACK-TRASH] BeginDeferredSpawn(prop_garbageBag_C) failed for eid=%u", eid);
        return false;
    }
    // The props-table row before finishing, as a placed prop's spawn does: the init pass resolves
    // the true mesh, mass and collision from it, and an empty name gives the default cube. The Key
    // is left to the game's own mint -- this bag is host-authored, so the minted one is canonical.
    ue_wrap::prop::WriteSpParityIdentity(bag, ue_wrap::fname_utils::StringToFName(L"garbBag"),
                             /*isStatic=*/false, /*removeWOrespawn=*/false,
                             /*frozen=*/false, /*sleep=*/false);
    if (!E::FinishDeferredSpawn(bag, loc, rot)) {
        UE_LOGW("[PACK-TRASH] FinishDeferredSpawn(prop_garbageBag_C) failed for eid=%u", eid);
        return false;
    }
    ue_wrap::prop::SetChipType(bag, chipType);
    E::DestroyActor(target);
    coop::trash_channel::ForgetEid(eid);
    ++g_packed;
    UE_LOGI("[PACK-TRASH] PACKED eid=%u slot=%u chipType=%u at (%.0f,%.0f,%.0f) -> bag=%p",
            eid, static_cast<unsigned>(slot), static_cast<unsigned>(chipType),
            loc.X, loc.Y, loc.Z, bag);
    return true;
}

void Execute(coop::net::Session& s, const coop::net::PackTrashIntentPayload& p, uint8_t slot) {
    const coop::element::ElementId eid = p.eid;
    // The body first: a sender without a live puppet has no reach to measure, and answering that
    // with an identity remedy would let a bodiless sender reach the ghost-heal destroy.
    coop::RemotePlayer* puppet = coop::players::Registry::Get().Puppet(slot);
    if (!puppet || !puppet->valid() || !puppet->GetActor()) {
        ++g_denied;
        UE_LOGI("[PACK-TRASH] DENY eid=%u slot=%u -- no live body", eid, static_cast<unsigned>(slot));
        return;
    }
    // The TOOL, on this machine. The game's own gate on a pack is that the body belongs to a bag in
    // the presser's hand, and cancelling that body on the client took the gate with it: without this
    // a sender with empty hands could name a pile and have the host bag it, at the token rate, for
    // nothing. The host asks its own mirror of that peer's hand, the way it asks for a broom before
    // running a client's stroke. The tool is still SPENT by the client -- that half is per-peer
    // state -- but whether one is held is a question the host answers here.
    void* const held = coop::hand_item::MirrorActorForSlot(slot);
    const bool holdsTool = p.toolKind == 1 ? ue_wrap::garbage_bag::IsRoll(held)
                                           : ue_wrap::garbage_bag::IsFold(held);
    if (!holdsTool) {
        ++g_denied;
        UE_LOGI("[PACK-TRASH] DENY eid=%u slot=%u -- the sender holds no %s here", eid,
                static_cast<unsigned>(slot), p.toolKind == 1 ? "bag roll" : "folded bag");
        return;
    }
    const auto token = coop::element::IntentTarget::ForClientIntent(s, slot, kPackReachUU);
    const coop::element::IntentSubject sub =
        token.Resolve(eid, coop::element::ElementType::Prop);
    switch (sub.outcome) {
        case coop::element::IntentOutcome::Ok:
            break;
        case coop::element::IntentOutcome::NoRow:
        case coop::element::IntentOutcome::StaleDead: {
            // The sender holds a row this host no longer has: heal the ghost rather than leave it
            // pressable forever.
            coop::net::PropDestroyPayload dp{};
            dp.key.len = 0;
            dp.elementId = eid;
            s.SendPropDestroy(dp);
            ++g_denied;
            UE_LOGI("[PACK-TRASH] DENY eid=%u slot=%u -- %s (ghost heal sent)", eid,
                    static_cast<unsigned>(slot), coop::element::OutcomeName(sub.outcome));
            return;
        }
        default:
            ++g_denied;
            UE_LOGI("[PACK-TRASH] DENY eid=%u slot=%u -- %s", eid, static_cast<unsigned>(slot),
                    coop::element::OutcomeName(sub.outcome));
            if (sub.actor && sub.outcome == coop::element::IntentOutcome::WrongType)
                RemedyOnce(sub.actor, eid);
            return;
    }
    void* target = sub.actor;
    const bool isPile = ue_wrap::prop::IsChipPile(target);
    const bool isClump = ue_wrap::prop::IsGarbageClump(target);
    if (!isPile && !isClump) {
        ++g_denied;
        UE_LOGI("[PACK-TRASH] DENY eid=%u slot=%u -- target is neither a pile nor a clump", eid,
                static_cast<unsigned>(slot));
        RemedyOnce(target, eid);
        return;
    }
    // A roll bags piles only, as its own body does.
    if (p.toolKind == 1 && !isPile) {
        ++g_denied;
        UE_LOGI("[PACK-TRASH] DENY eid=%u slot=%u -- a roll bags piles only", eid,
                static_cast<unsigned>(slot));
        return;
    }
    // A target under an open carry is about to be destroyed with a hand drive and a settle latch
    // still on it; the carry closes first, and this press is refused rather than half-applied.
    if (coop::trash_channel::IsCarrying(eid) || coop::trash_channel::HasPendingSettle(eid)) {
        ++g_denied;
        UE_LOGI("[PACK-TRASH] DENY eid=%u slot=%u -- carried or settling", eid,
                static_cast<unsigned>(slot));
        return;
    }
    PerformPack(target, slot, eid);
}

// ---- the client's gate -----------------------------------------------------------------------
sg::Verdict OnHandUsePre(const sg::Call& call) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Client) return sg::Verdict::Run;
    // The class gate: every hand-usable tool declares this verb -- 146 classes do -- so without it a
    // knife swing would enter here. COMPARE only: the two classes are PRODUCED by Install, under its
    // throttle. Resolving them here would call FindClass on every left click of every one of those
    // classes, and a FindClass miss is not memoised, so in a world holding no bag each click would
    // pay two full object-array walks. That is the coin gun's defect exactly.
    if (!call.object || (!g_foldCls && !g_rollCls)) return sg::Verdict::Run;
    void* const cls = R::ClassOf(call.object);
    const bool isFold = g_foldCls && cls == g_foldCls;
    const bool isRoll = !isFold && g_rollCls && cls == g_rollCls;
    if (!isFold && !isRoll) return sg::Verdict::Run;

    static void*   sFn = nullptr;
    static int32_t sPlayerOff = -1;
    if (call.function != sFn) {
        sFn = call.function;
        sPlayerOff = R::FindParamOffset(call.function, L"player");
    }
    if (sPlayerOff < 0) return sg::Verdict::Run;
    void* player = *reinterpret_cast<void**>(call.locals + sPlayerOff);
    if (!player) return sg::Verdict::Run;

    // What the tool's own body would bag: the RAW interaction trace it breaks itself, not the
    // look-at actor derived from it later in the tick. The derived one is skipped while a grab is
    // open or the trace did not block, so a press whose trace named the pile could read as naming
    // nothing here -- and this body running locally is exactly the divergence the lane exists to
    // prevent: a bag on this screen and a pile still standing on every other.
    void* aimed = E::ReadMainPlayerHitActor(player);
    const bool isPile = ue_wrap::prop::IsChipPile(aimed);
    const bool isClump = isFold && ue_wrap::prop::IsGarbageClump(aimed);
    if (!isPile && !isClump) return sg::Verdict::Run;  // the tool's other uses stay the game's

    coop::element::ElementId eid = coop::remote_prop::ResolveMirrorEidByActor(aimed);
    if (eid == 0u || eid == coop::element::kInvalidId)
        eid = coop::element::Registry::Get().EidForActor(aimed);
    if (eid == 0u || eid == coop::element::kInvalidId) {
        // An identity-less pile: packing it locally would author a bag the host never sees and
        // destroy a pile the host still has. Refuse the press instead.
        ++g_refusedGhost;
        if (g_refusedGhost <= 3 || g_refusedGhost % 20 == 0)
            UE_LOGW("[PACK-TRASH] CLIENT press on an identity-less target %p -- refused (#%llu)",
                    aimed, static_cast<unsigned long long>(g_refusedGhost));
        return sg::Verdict::Cancel;
    }

    coop::net::PackTrashIntentPayload p{};
    p.eid = eid;
    p.targetKind = isClump ? 1u : 0u;
    p.toolKind = isRoll ? 1u : 0u;
    if (!s->SendReliable(coop::net::ReliableKind::PackTrashIntent, &p, sizeof(p)))
        return sg::Verdict::Cancel;  // the press is spent either way; a bag the host refused is none
    ++g_sent;
    // The tool is this peer's own state with no lane, so this peer spends it -- next tick, out of
    // the body's frame.
    g_spent.push_back(SpentTool{call.object, R::InternalIndexOf(call.object), isRoll});
    if (g_sent <= 3 || g_sent % 50 == 0)
        UE_LOGI("[PACK-TRASH] CLIENT SENT eid=%u kind=%u tool=%s (#%llu)", eid,
                static_cast<unsigned>(p.targetKind), isRoll ? "roll" : "fold",
                static_cast<unsigned long long>(g_sent));
    return sg::Verdict::Cancel;
}

void ConsumeSpentTools() {
    for (const SpentTool& t : g_spent) {
        if (!R::IsLiveByIndex(t.actor, t.idx)) continue;
        if (!t.roll) {
            E::DestroyActor(t.actor);
            continue;
        }
        int32_t bags = 0;
        if (!ue_wrap::garbage_bag::ReadRollCount(t.actor, bags)) continue;
        bags -= 1;
        ue_wrap::garbage_bag::WriteRollCount(t.actor, bags > 0 ? bags : 0);
        if (bags <= 0) E::DestroyActor(t.actor);
    }
    g_spent.clear();
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (!g_watchInstalled)
        g_watchInstalled = sg::WatchName(kHandUseVerb, kTagPackTrash, &OnHandUsePre, nullptr);
    // Install is the per-tick retry pump and a FindClass miss is not memoised, so the resolve of a
    // class the world may not hold is bound to about 1 Hz, the coin gun's shape.
    if (g_foldCls && g_rollCls) return;
    static uint32_t sResolveN = 0;
    if ((sResolveN++ % 125u) != 0u) return;
    if (!g_foldCls) g_foldCls = ue_wrap::garbage_bag::FoldClass();
    if (!g_rollCls) g_rollCls = ue_wrap::garbage_bag::RollClass();
}

void Tick(coop::net::Session& session) {
    sg::ResolvePendingNames();
    if (!g_watchInstalled) {
        g_watchInstalled = sg::WatchName(kHandUseVerb, kTagPackTrash, &OnHandUsePre, nullptr);
        return;
    }
    if (!g_watchLive && sg::NameWatchLive(kHandUseVerb, kTagPackTrash)) {
        g_watchLive = true;
        UE_LOGI("[PACK-TRASH] the hand-use gate is live");
    }
    if (!session.running()) return;
    sg::SetEnabled(true);  // each lane asserts its own enable

    if (!g_spent.empty()) ConsumeSpentTools();
    if (session.role() != coop::net::Role::Host) return;
    for (uint8_t slot = 1; slot < coop::net::kMaxPeers; ++slot) {
        if (g_pending[slot].empty()) continue;
        if (!TakeToken(slot)) continue;
        const coop::net::PackTrashIntentPayload p = g_pending[slot].front();
        g_pending[slot].pop_front();
        Execute(session, p, slot);
    }
}

void OnPackTrashIntent(coop::net::Session& session,
                       const coop::net::PackTrashIntentPayload& payload, uint8_t senderSlot) {
    if (session.role() != coop::net::Role::Host) return;
    if (senderSlot < 1 || senderSlot >= coop::net::kMaxPeers) return;
    auto& q = g_pending[senderSlot];
    if (q.size() >= kMaxPending) {
        Bucket& b = g_rate[senderSlot];
        const uint64_t now = coop::active_drive::NowMs();
        if (now >= b.nextSayMs) {
            b.nextSayMs = now + kRefusalSayMs;
            UE_LOGW("[PACK-TRASH] slot %u queue full (%zu) -- refusing presses until it drains",
                    static_cast<unsigned>(senderSlot), q.size());
        }
        ++g_denied;
        return;
    }
    q.push_back(payload);
}

void OnPeerLeft(uint8_t slot) {
    if (slot >= coop::net::kMaxPeers) return;
    g_pending[slot].clear();
    g_rate[slot] = Bucket{};
}

void OnDisconnect() {
    if (g_sent || g_packed || g_denied || g_refusedGhost)
        UE_LOGI("[PACK-TRASH] session end -- sent=%llu packed=%llu denied=%llu ghost-refused=%llu",
                static_cast<unsigned long long>(g_sent), static_cast<unsigned long long>(g_packed),
                static_cast<unsigned long long>(g_denied),
                static_cast<unsigned long long>(g_refusedGhost));
    for (uint8_t slot = 0; slot < coop::net::kMaxPeers; ++slot) {
        g_pending[slot].clear();
        g_rate[slot] = Bucket{};
    }
    g_spent.clear();
    g_remedyAtMs.clear();
    g_sent = g_packed = g_denied = g_refusedGhost = 0;
}

}  // namespace coop::pack_trash_intent
