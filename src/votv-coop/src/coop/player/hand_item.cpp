// coop/player/hand_item.cpp -- see coop/player/hand_item.h for the design.

#include "coop/player/hand_item.h"

#include "coop/net/latest_wins.h"
#include "coop/net/net_clock.h"
#include "coop/net/protocol.h"
#include "coop/player/hand_history.h"
#include "coop/player/hand_item_mirror.h"
#include "coop/player/hand_item_pose.h"  // the owner's measure of its hold
#include "coop/player/hand_item_wire.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/player/roster_ledger.h"
#include "coop/props/prop_echo_suppress.h"
#include "coop/props/trash_collect_sync.h"  // EnsureHeldItemBroadcast (hand-edge world release)
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/hot_path_guard.h"  // UE_ASSERT_GAME_THREAD
#include "ue_wrap/actors/inventory.h"   // the hands' item names
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"

#include <cmath>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace coop::hand_item {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;
namespace W = coop::hand_item_wire;

static_assert(W::kMaxRecordBytes <= static_cast<size_t>(coop::net::kMaxReliablePayload),
              "a HandItem at its string caps must fit one reliable datagram");

SlotHand g_hands[coop::players::kMaxPeers];     // GT-only: the main hands
SlotHand g_offHands[coop::players::kMaxPeers];  // GT-only: the off hands

}  // namespace

SlotHand& HandOf(Hand hand, uint8_t slot) { return hand == Hand::Main ? g_hands[slot] : g_offHands[slot]; }

namespace {

// The session, cached by TickOwner for the connect-replay entry that only receives a slot.
// Game thread only.
coop::net::Session* g_session = nullptr;

// Owner-side change detection.
uint32_t     g_ownLookSeq = 0;  // owner ticks that read the hand; the latch below is its class at the last one
bool         g_ownHas = false;
std::wstring g_ownCls;
std::wstring g_ownName;
std::wstring g_ownItem;
// The last-seen holding actor, pointer and captured index, since the game recycles addresses.
// While unchanged, TickOwner skips the class and name renders, which allocate engine-side per
// call.
void*        g_ownHeldPtr = nullptr;
int32_t      g_ownHeldIdx = -1;

// The off hand's owner latch: what was announced, and the actor it was read from.
struct OffLatch {
    bool         has = false;
    std::wstring cls, name, item;
    void*        ptr = nullptr;
    int32_t      idx = -1;
};
OffLatch g_ownOff;

// The hand-edge world release. A drop or quick-slot place is not a spawn: the game releases
// the ex-hand actor into the world (the same actor, with physics and collision re-enabled),
// so no spawn seam fires and the census guard is blind. But the previous hand actor's identity
// is held here, so at the hand edge an ex-hand actor that survived the edge (a stow or switch
// destroys it) is expressed immediately through the canonical untracked-prop broadcast, on
// both roles.
void ExpressReleasedHandActor(coop::net::Session& session, void* prev, int32_t prevIdx) {
    if (!prev || !R::IsLiveByIndex(prev, prevIdx)) {
        // Diagnostic: the previous actor dead at the hand edge means it was destroyed in hand (a
        // stow or switch, or a drop path that destroys the hand actor and spawns a separate world
        // actor), so the same-actor release does not run. The pointer is logged, never
        // dereferenced.
        if (prev) {
            UE_LOGI("[ROCK-DROP] hand-edge: ex-hand actor %p DEAD (destroyed in-hand) -- the "
                    "'release surviving actor' express does NOT run; a fresh world spawn (if any) "
                    "is authored elsewhere (client Aprop spawns are host-auth-skipped)", prev);
        }
        return;
    }
    // The previous actor survived the hand edge: a genuine same-actor release into the world.
    if (coop::trash_collect_sync::EnsureHeldItemBroadcast(prev, &session)) {
        UE_LOGI("hand_item: released ex-hand actor %p expressed as a world prop "
                "(R-drop/place -- peers see it this tick)", prev);
    } else {
        UE_LOGI("[ROCK-DROP] hand-edge: ex-hand actor %p SURVIVED but EnsureHeldItemBroadcast "
                "DECLINED it (see the decline reason above) -- not expressed", prev);
    }
}

// HandItems this peer dropped or refused, per sender, for the log: the first and then each power of two,
// so a sender at the edge budget's rate cannot flood it. The count when this drop is one to log, else 0.
uint32_t g_droppedBySender[coop::players::kMaxPeers] = {};
uint32_t DropToLog(int sender) {
    if (sender < 0 || sender >= static_cast<int>(coop::players::kMaxPeers)) return 1;
    const uint32_t n = ++g_droppedBySender[sender];
    return (n & (n - 1)) == 0 ? n : 0;
}
std::wstring ReadItemName(void* actor) {
    const int32_t off = PropNameOffset();
    if (off < 0 || !actor) return {};
    R::FName n{};
    std::memcpy(&n, reinterpret_cast<const uint8_t*>(actor) + off, sizeof(n));
    std::wstring s = R::ToString(n);
    if (s == L"None") s.clear();
    return s;
}

// The payload of `slot`'s `hand` (coop/player/hand_item_wire.h).
std::vector<uint8_t> BuildPayload(uint8_t slot, Hand hand, const SlotHand& h) {
    W::Record r;
    r.slot = slot;
    r.hand = hand;
    r.has = h.has;
    r.cls = h.cls;
    r.name = h.name;
    r.item = h.item;
    std::memcpy(r.relPos, h.relPos, sizeof(r.relPos));
    std::memcpy(r.relRot, h.relRot, sizeof(r.relRot));
    return W::Build(r);
}

void SendState(coop::net::Session& session, uint8_t slot, Hand hand, const SlotHand& h) {
    const std::vector<uint8_t> p = BuildPayload(slot, hand, h);
    if (session.role() == coop::net::Role::Host) {
        for (int x = 1; x < coop::net::kMaxPeers; ++x) {
            if (x == slot) continue;                 // never echo the originator
            if (!session.IsSlotReady(x)) continue;
            session.SendReliableToSlot(x, coop::net::ReliableKind::HandItem,
                                       p.data(), static_cast<int>(p.size()));
        }
    } else {
        session.SendReliableToSlot(0, coop::net::ReliableKind::HandItem,
                                   p.data(), static_cast<int>(p.size()));
    }
}

// Host: the hand states applied for a slot's occupant (StatesApplied).
coop::roster_ledger::PerSlotState<uint32_t> g_statesApplied;

// Host: one client's hand state applied, field by field: `lastCls` moves only while the hand holds
// something, since the pry gate reads it once the hand is empty (LastHeldClassIs). Then re-sent to
// every other ready client. The hand history is the main hand's, the one the verbs it judges use.
void ApplyHandState(coop::net::Session& session, uint8_t slot, Hand hand, const SlotHand& in) {
    SlotHand& h = HandOf(hand, slot);
    h.has = in.has; h.cls = in.cls; h.name = in.name; h.item = in.item;
    if (in.has) h.lastCls = in.cls;
    std::memcpy(h.relPos, in.relPos, sizeof(h.relPos));
    std::memcpy(h.relRot, in.relRot, sizeof(h.relRot));
    h.haveRel = in.has && hand == Hand::Main;
    if (hand == Hand::Main) coop::hand_history::Note(slot, in.has, in.cls, coop::net::NowMs());
    SendState(session, slot, hand, h);
    ++g_statesApplied[slot];
}

// Host: per slot, the newest changed hand state and its pace (coop/net/latest_wins.h); one struct, so both clear with the
// occupant.
struct PendingHand { SlotHand value; coop::net::LatestWins pace; };
coop::roster_ledger::PerSlotState<PendingHand> g_pendingHand;
coop::roster_ledger::PerSlotState<PendingHand> g_pendingOffHand;

PendingHand& PendingOf(Hand hand, uint8_t slot) { return hand == Hand::Main ? g_pendingHand[slot] : g_pendingOffHand[slot]; }

// Whether two states hold the same thing: emptiness, the class without case, the name and the item. The hold
// transform is not compared: the pose stream overwrites the applied one each sample.
bool SameHeld(const SlotHand& a, const SlotHand& b) {
    if (a.has != b.has) return false;
    return !a.has || (::_wcsicmp(a.cls.c_str(), b.cls.c_str()) == 0 && a.name == b.name && a.item == b.item);
}

// The local off hand's actor: holdingActor_offhand when it is a live prop, the routing predicate of the main hand.
void* LocalOffHandActor() {
    void* player = coop::players::Registry::Get().Local();
    ue_wrap::engine::MainPlayerGrabState gs{};
    if (!player || !ue_wrap::engine::ReadMainPlayerGrabState(player, gs)) return nullptr;
    void* const oh = gs.offHand;
    return oh && R::IsLive(oh) && ue_wrap::prop::IsDescendantOfProp(oh) ? oh : nullptr;
}

// Owner side of the off hand: announce a change of what it holds. The same live actor skips the renders but for a
// re-check every 30 ticks (a held prop renamed in place); no transform rides, the mirror sits at weapon_off.
void TickOwnOffHand(coop::net::Session& session, uint8_t self, void* local) {
    ue_wrap::engine::MainPlayerGrabState gs{};
    void* held = nullptr;
    if (local && ue_wrap::engine::ReadMainPlayerGrabState(local, gs) && gs.offHand && R::IsLive(gs.offHand) &&
        ue_wrap::prop::IsDescendantOfProp(gs.offHand))
        held = gs.offHand;
    SlotHand& h = g_offHands[self];
    if (!held) {
        if (!g_ownOff.has) return;
        g_ownOff = OffLatch{};
        const std::wstring last = h.lastCls;
        h = SlotHand{};
        h.lastCls = last;
        SendState(session, self, Hand::Off, h);
        UE_LOGI("hand_item: local off hand -> EMPTY (announced)");
        return;
    }
    static uint32_t sSameStreak = 0;
    const int32_t idx = R::InternalIndexOf(held);
    if (held == g_ownOff.ptr && idx == g_ownOff.idx && (++sSameStreak % 30) != 0) return;
    g_ownOff.ptr = held;
    g_ownOff.idx = idx;
    const std::wstring cls = R::ClassNameOf(held);
    const std::wstring name = ReadItemName(held);
    const std::wstring item = ue_wrap::inventory::ReadHeldItemName(ue_wrap::inventory::Hand::Off);
    if (g_ownOff.has && cls == g_ownOff.cls && name == g_ownOff.name && item == g_ownOff.item) return;
    g_ownOff.has = true;
    g_ownOff.cls = cls;
    g_ownOff.name = name;
    g_ownOff.item = item;
    h = SlotHand{};
    h.has = true; h.cls = cls; h.name = name; h.item = item; h.lastCls = cls;
    SendState(session, self, Hand::Off, h);
    UE_LOGI("hand_item: local off hand -> cls='%ls' name='%ls' item='%ls' (announced)", cls.c_str(), name.c_str(),
            item.c_str());
}

}  // namespace

void TickOwner(coop::net::Session& session, void* local, void* holdingProp) {
    g_session = &session;
    if (!session.connected()) return;
    const uint8_t self = coop::players::Registry::Get().LocalPeerId();
    if (self >= coop::players::kMaxPeers) return;
    ++g_ownLookSeq;  // every path below leaves the latch naming the class the hand holds now
    TickOwnOffHand(session, self, local);

    if (holdingProp) {
        // The live view-relative hold transform is measured and latched every tick while holding;
        // the net thread streams it, so a melee swing reads as continuous motion on the mirror.
        // Before the identity-latch early return: motion must flow even with the held identity
        // unchanged.
        {
            float sp[3], sr[3];
            if (coop::hand_item_pose::MeasureLocalHoldRelative(local, holdingProp, sp, sr)) {
                coop::net::HandPoseSnapshot hp{};
                std::memcpy(hp.relPos, sp, sizeof(sp));
                std::memcpy(hp.relRot, sr, sizeof(sr));
                session.SetLocalHandPose(true, hp);
            }
        }
        // O(1) steady state: the same live actor as last tick skips the renders, except a re-check
        // every 30 ticks, since the game renames a held prop in place (arming a grenade), which a
        // pointer latch would never see.
        static uint32_t sSameStreak = 0;
        const int32_t idx = R::InternalIndexOf(holdingProp);
        if (holdingProp == g_ownHeldPtr && idx == g_ownHeldIdx &&
            (++sSameStreak % 30) != 0) {
            return;
        }
        if (holdingProp != g_ownHeldPtr && g_ownHeldPtr) {
            // The identity changed with a previous actor latched: if it survived, it was released
            // to the world (place, then the next slot).
            ExpressReleasedHandActor(session, g_ownHeldPtr, g_ownHeldIdx);
        }
        g_ownHeldPtr = holdingProp;
        g_ownHeldIdx = idx;
        const std::wstring cls = R::ClassNameOf(holdingProp);
        const std::wstring name = ReadItemName(holdingProp);
        const std::wstring item = ue_wrap::inventory::ReadHeldItemName(ue_wrap::inventory::Hand::Main);
        if (!g_ownHas || cls != g_ownCls || name != g_ownName || item != g_ownItem) {
            g_ownHas = true;
            g_ownCls = cls;
            g_ownName = name;
            g_ownItem = item;
            SlotHand& h = g_hands[self];
            h.has = true; h.cls = cls; h.name = name; h.item = item; h.lastCls = cls;
            // The first measure of the weld's view-relative transform, which may land a tick early;
            // the periodic re-check corrects it. Measured into scratch and reset on failure: on a
            // quick-slot switch a failed measure would otherwise ship the previous item's transform
            // under the new item's name, plausible values the receiver cannot reject.
            float p[3], rr[3];
            if (coop::hand_item_pose::MeasureLocalHoldRelative(local, holdingProp, p, rr)) {
                std::memcpy(h.relPos, p, sizeof(p));
                std::memcpy(h.relRot, rr, sizeof(rr));
                h.haveRel = true;
            } else {
                h.relPos[0] = kFallbackRelPos[0];
                h.relPos[1] = kFallbackRelPos[1];
                h.relPos[2] = kFallbackRelPos[2];
                h.relRot[0] = h.relRot[1] = h.relRot[2] = 0.f;
                h.haveRel = false;
            }
            SendState(session, self, Hand::Main, h);
            UE_LOGI("hand_item: local hand -> cls='%ls' name='%ls' item='%ls' (announced, rel %s)",
                    cls.c_str(), name.c_str(), item.c_str(), h.haveRel ? "measured" : "fallback");
            // No reconcile request here: the pickup's ground-actor death is caught at the destroy
            // seam, and the stream carries the live transform.
        }
    } else if (g_ownHas) {
        session.SetLocalHandPose(false, {});  // stop the stream on the empty edge
        // The hand is empty this tick: an ex-hand actor that survived the edge was released into
        // the world, so express it now, then drop the identity latch so a re-appearance re-renders
        // and re-compares.
        ExpressReleasedHandActor(session, g_ownHeldPtr, g_ownHeldIdx);
        g_ownHeldPtr = nullptr;
        g_ownHeldIdx = -1;
        // The stow announce is edge-instant: a quick-slot switch is one synchronous better_holdItem_mainhand call
        // (destroy, spawn, then the holding name), so a poll never observes a mid-switch null and
        // no debounce is needed.
        g_ownHas = false;
        g_ownCls.clear();
        g_ownName.clear();
        g_ownItem.clear();
        SlotHand& h = g_hands[self];
        const std::wstring last = h.lastCls;
        h = SlotHand{};
        h.lastCls = last;
        SendState(session, self, Hand::Main, h);
        UE_LOGI("hand_item: local hand -> EMPTY (announced)");
        // No reconcile request: the released actor was expressed above, and inventory spawns ride
        // the finish-spawn seam.
    }
}

void* LocalHandActor() {
    // The registry's player, not one TickOwner cached: a client's streams wait for its load tail, and the hand is
    // asked about before that (the join sweep spares it).
    void* player = coop::players::Registry::Get().Local();
    ue_wrap::engine::MainPlayerGrabState gs{};
    if (!player || !ue_wrap::engine::ReadMainPlayerGrabState(player, gs)) return nullptr;
    void* ha = gs.mainHand;
    if (!ha || !R::IsLive(ha)) return nullptr;
    // The same routing predicate as local_streams: only a prop descendant in the holding actor is
    // the hotbar hand; the clump or pile carry is a world entity and must stay adoptable.
    if (!ue_wrap::prop::IsDescendantOfProp(ha)) return nullptr;
    return ha;
}

bool IsLocalHandViewActor(void* actor) {
    if (!actor) return false;
    void* player = coop::players::Registry::Get().Local();
    ue_wrap::engine::MainPlayerGrabState gs{};
    if (!player || !ue_wrap::engine::ReadMainPlayerGrabState(player, gs)) return false;
    // Pointer identity with no liveness check on the actor: it is asked at the entry of the
    // actor's own destroy, and R::IsLive would refuse an actor already pending kill.
    return ue_wrap::engine::InEitherHand(gs, actor) && ue_wrap::prop::IsDescendantOfProp(actor);
}

void* MirrorActorForSlot(uint8_t slot) {
    return slot < coop::players::kMaxPeers ? mirror::Live(Hand::Main, slot) : nullptr;
}

size_t CollectHandAxisActors(void* out[], size_t cap) {
    size_t n = 0;
    if (void* lh = LocalHandActor(); lh && n < cap) out[n++] = lh;
    if (void* oh = LocalOffHandActor(); oh && n < cap) out[n++] = oh;
    for (uint8_t slot = 0; slot < coop::players::kMaxPeers; ++slot)
        for (const Hand hand : {Hand::Main, Hand::Off})
            if (void* m = mirror::Live(hand, slot); m && n < cap) out[n++] = m;
    return n;
}

bool IsHandAxisActor(void* actor) {
    if (!actor) return false;
    void* axis[kHandAxisCap];
    const size_t n = CollectHandAxisActors(axis, kHandAxisCap);
    for (size_t i = 0; i < n; ++i)
        if (axis[i] == actor) return true;
    return false;
}

void TickMirrors() {
    auto& reg = coop::players::Registry::Get();
    const uint8_t self = reg.LocalPeerId();
    for (uint8_t slot = 0; slot < coop::players::kMaxPeers; ++slot) {
        if (slot == self) continue;  // never mirror our own hand
        coop::RemotePlayer* pup = reg.Puppet(slot);
        // valid() is IsLiveByIndex on the puppet's captured index, never bare IsLive on the cached
        // actor.
        void* puppetActor = (pup && pup->valid()) ? pup->GetActor() : nullptr;
        // The freshest streamed hold transform overrides the announce-time one; the announce only
        // covers the pre-stream frame.
        if (puppetActor && g_hands[slot].has && g_session) {
            coop::net::HandPoseSnapshot hp;
            bool isNew = false;
            if (g_session->TryGetRemoteHandPose(slot, hp, &isNew) && isNew) {
                SlotHand& wh = g_hands[slot];
                std::memcpy(wh.relPos, hp.relPos, sizeof(hp.relPos));
                std::memcpy(wh.relRot, hp.relRot, sizeof(hp.relRot));
                wh.haveRel = true;
            }
        }
        mirror::Tick(Hand::Main, slot, pup, puppetActor);
        mirror::Tick(Hand::Off, slot, pup, puppetActor);
    }
}

bool HandleHandItem(coop::net::Session& session,
                    const coop::net::Session::ReliableMessage& msg) {
    UE_ASSERT_GAME_THREAD("HandleHandItem");
    // Each string within the cap Build writes to, so what the host keeps is what it relays, and free of
    // control characters (coop/player/hand_item_wire.h).
    W::Record rec;
    const std::string why = W::Parse(msg.payload, static_cast<size_t>(msg.payloadLen), rec);
    const char* malformed = !why.empty()                         ? why.c_str()
                          : rec.slot >= coop::net::kMaxPeers     ? "a slot out of range"
                                                                 : nullptr;
    if (malformed) {
        if (const uint32_t n = DropToLog(msg.senderPeerSlot))
            UE_LOGW("hand_item: %s -- dropping (%u from slot %d)", malformed, n, msg.senderPeerSlot);
        return true;
    }
    const uint8_t describedSlot = rec.slot;
    const Hand hand = rec.hand;
    // A main-hand transform that is not sane (non-finite, out of arm's reach) keeps the fallback placement.
    SlotHand in;
    in.has = rec.has; in.cls = std::move(rec.cls); in.name = std::move(rec.name); in.item = std::move(rec.item);
    if (rec.haveRel) {
        std::memcpy(in.relPos, rec.relPos, sizeof(in.relPos));
        std::memcpy(in.relRot, rec.relRot, sizeof(in.relRot));
    }

    if (session.role() == coop::net::Role::Host) {
        // The forgery guard: a client may only describe its own hand.
        if (msg.senderPeerSlot != describedSlot || describedSlot == 0) {
            if (const uint32_t n = DropToLog(msg.senderPeerSlot))
                UE_LOGW("hand_item: slot=%u from senderSlot=%d -- forged, dropping (%u from that slot)",
                        static_cast<unsigned>(describedSlot), msg.senderPeerSlot, n);
            return true;
        }
        // A class that is not a hand item -- one of the two families by name, or a class this host resolves
        // to something else -- is not kept and not relayed, and the slot keeps the state it had; the link
        // stays. Any other class this host cannot resolve yet is kept and relayed, and
        // judged where it would be built (SpawnMirror): the owner announces only on a change, so an
        // honest state refused here would never come again. MTA's server drops a weapon id outside the
        // set it knows and keeps the link
        // (reference/mtasa-blue/Server/mods/deathmatch/logic/packets/CBulletsyncPacket.cpp:78-82). It
        // can also hold a weapon against its own record of the player; this host's only copy of a
        // client's hands is the profile that client streams itself, so it checks the lane's type. A
        // hand item is kept and relayed under its class's own spelling, and every peer compares the class
        // without case.
        // The newest state kept, applied at the budget's pace (coop/net/latest_wins.h). An unchanged one costs the
        // parse and a compare, ahead of the class gate: the applied state passed it, and the compare reads the
        // class without case. The host's verbs and its mirror read the applied state, at most a refill behind.
        PendingHand& pend = PendingOf(hand, describedSlot);
        if (SameHeld(in, HandOf(hand, describedSlot))) {
            pend.pace.Withdraw();
            return true;
        }
        std::wstring own;
        const HandClass kind = in.has ? ResolveHandClass(in.cls, nullptr, &own) : HandClass::Unresolved;
        if (kind == HandClass::NotHand) {
            if (const uint32_t n = DropToLog(msg.senderPeerSlot))
                UE_LOGW("hand_item: slot %u names class '%ls', which is not a hand item -- not kept or relayed "
                        "(%u from this slot)", static_cast<unsigned>(describedSlot), in.cls.c_str(), n);
            return true;
        }
        if (kind == HandClass::Hand) in.cls = own;
        pend.value = in;
        if (pend.pace.Offer(kHandBudget, coop::net::NowMs())) ApplyHandState(session, describedSlot, hand, in);
        return true;
    }

    // Client: only the host relays hand states.
    if (msg.senderPeerSlot != 0) {
        if (const uint32_t n = DropToLog(msg.senderPeerSlot))
            UE_LOGW("hand_item: HandItem from non-host senderPeerSlot=%d -- dropping (%u from that slot)",
                    msg.senderPeerSlot, n);
        return true;
    }
    if (describedSlot == coop::players::Registry::Get().LocalPeerId()) return true;  // own echo
    SlotHand& h = HandOf(hand, describedSlot);
    h.has = in.has; h.cls = in.cls; h.name = in.name; h.item = in.item;
    if (in.has) h.lastCls = in.cls;
    std::memcpy(h.relPos, in.relPos, sizeof(h.relPos));
    std::memcpy(h.relRot, in.relRot, sizeof(h.relRot));
    h.haveRel = in.has && hand == Hand::Main;
    return true;
}

void ReplayPeerStatesToSlot(int slot) {
    coop::net::Session* session = g_session;
    if (!session || session->role() != coop::net::Role::Host) return;
    if (slot < 1 || slot >= static_cast<int>(coop::net::kMaxPeers)) return;
    for (uint8_t s = 0; s < coop::players::kMaxPeers; ++s) {
        if (s == slot) continue;
        for (const Hand hand : {Hand::Main, Hand::Off}) {
            const SlotHand& h = HandOf(hand, s);
            if (!h.has) continue;  // empty is the joiner's default -- nothing to send
            const std::vector<uint8_t> p = BuildPayload(s, hand, h);
            session->SendReliableToSlot(slot, coop::net::ReliableKind::HandItem,
                                        p.data(), static_cast<int>(p.size()));
            UE_LOGI("hand_item: connect-replay -- slot %u %s hand holds '%ls' -> joiner slot %d",
                    static_cast<unsigned>(s), HandName(hand), h.cls.c_str(), slot);
        }
    }
}

void Reset() {
    // Stop the hand-pose stream before the session cache drops, or a stale latch would stream the
    // dead session's transform into the next one.
    if (g_session) g_session->SetLocalHandPose(false, {});
    for (uint8_t s = 0; s < coop::players::kMaxPeers; ++s) {
        mirror::Destroy(Hand::Main, s, "session reset");
        mirror::Destroy(Hand::Off, s, "session reset");
        g_hands[s] = SlotHand{};
        g_offHands[s] = SlotHand{};
        g_droppedBySender[s] = 0;
        mirror::ForgetSlot(s);
        coop::hand_history::Clear(s);
    }
    g_ownHas = false;
    g_ownCls.clear();
    g_ownName.clear();
    g_ownItem.clear();
    g_ownHeldPtr = nullptr;
    g_ownHeldIdx = -1;
    g_ownOff = OffLatch{};
    g_session        = nullptr;
}

bool HeldClassIs(uint8_t slot, const wchar_t* cls) {
    if (slot >= coop::players::kMaxPeers || !cls) return false;
    const SlotHand& h = g_hands[slot];
    return h.has && ::_wcsicmp(h.cls.c_str(), cls) == 0;
}

bool OwnAnnounced(const wchar_t* cls) {
    return cls && g_ownHas && ::_wcsicmp(g_ownCls.c_str(), cls) == 0;
}

uint32_t OwnHandLookSeq() { return g_ownLookSeq; }

bool LastHeldClassIs(uint8_t slot, const wchar_t* cls) {
    if (slot >= coop::players::kMaxPeers || !cls) return false;
    const SlotHand& h = g_hands[slot];
    return !h.lastCls.empty() && ::_wcsicmp(h.lastCls.c_str(), cls) == 0;
}

void PayOwed(coop::net::Session& session) {
    if (session.role() != coop::net::Role::Host) return;
    for (int slot = 1; slot < static_cast<int>(coop::players::kMaxPeers); ++slot) {
        for (const Hand hand : {Hand::Main, Hand::Off}) {
            PendingHand& pend = PendingOf(hand, static_cast<uint8_t>(slot));
            if (pend.pace.Owed() && pend.pace.Due(kHandBudget, coop::net::NowMs()))
                ApplyHandState(session, static_cast<uint8_t>(slot), hand, pend.value);
        }
    }
}

bool HoldsItem(uint8_t slot) {
    return slot < coop::players::kMaxPeers && g_hands[slot].has;
}

bool HandStateOwed(uint8_t slot) {
    return slot < coop::players::kMaxPeers && g_pendingHand[slot].pace.Owed();
}

uint32_t StatesApplied(int slot) {
    return (slot >= 0 && slot < static_cast<int>(coop::players::kMaxPeers)) ? g_statesApplied[slot] : 0;
}

std::wstring HeldItem(uint8_t slot) {
    if (slot >= coop::players::kMaxPeers) return {};
    const SlotHand& h = g_hands[slot];
    return h.has ? h.item : std::wstring();
}

void OnSlotDisconnected(uint8_t slot) {
    if (slot >= coop::players::kMaxPeers) return;
    mirror::Destroy(Hand::Main, slot, "peer disconnected");
    mirror::Destroy(Hand::Off, slot, "peer disconnected");
    g_hands[slot] = SlotHand{};
    g_offHands[slot] = SlotHand{};
    g_droppedBySender[slot] = 0;
    mirror::ForgetSlot(slot);
    coop::hand_history::Clear(slot);
}

}  // namespace coop::hand_item
