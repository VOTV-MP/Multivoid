// coop/creatures/ambient_creature_sync.cpp -- see coop/creatures/ambient_creature_sync.h.

#include "coop/creatures/ambient_creature_sync.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/creatures/npc_sync.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/world/ambient_births.h"

#include "ue_wrap/actors/ambient_creature.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace coop::ambient_creature_sync {
namespace {

namespace AB = coop::ambient_births;
namespace AC = ue_wrap::ambient_creature;
namespace R = ue_wrap::reflection;
namespace SG = ue_wrap::script_gate;
using Clock = std::chrono::steady_clock;

constexpr auto kHeldSaid = std::chrono::seconds(10);

// The landing's entry on the host: walkingTree_C's OnLanded waits a second and then shows the mesh.
UE_FACT_FN(kRowOnLandedWalkingTree, "ambient_creature_sync", L"walkingTree_C", L"OnLanded", Declared);
constexpr int kTag = 0x41435452;  // 'ACTR'

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_watchTaken = false;
bool g_namesSettled = false;

// HOST: each tree this lane knows, by the actor; its element, what it last sent, and whether it must send.
struct Tree {
    int32_t                  index = -1;
    coop::element::ElementId eid = coop::element::kInvalidId;
    float                    sentScale = 0.f;
    bool                     sent = false;
    bool                     sentLanded = false;
    bool                     landed = false;  // the landing's entry was seen, or the mesh reads shown
    bool                     dirty = false;
    uint32_t                 pendingSlots = 0;  // joiners owed the current state alone (bit = slot)
    Clock::time_point        heldSince{};
    bool                     heldSaid = false;
};
std::unordered_map<void*, Tree> g_trees;
bool g_anyDirty = false;

// CLIENT: the host's tree, kept until its mirror exists, and the actor it was written to. MTA writes an object's scale into its
// add packet (reference/mtasa-blue/Server/mods/deathmatch/logic/packets/CEntityAddPacket.cpp:268-283) and sends a later
// change as an RPC (CStaticFunctionDefinitions.cpp:8395-8402), so its receiver never holds a state for an element it lacks;
// here the NPC spawn payload serves every NPC class, so a tree's size is a kind of its own and waits for its mirror.
struct Want {
    float   scale = 0.f;
    bool    landed = false;
    void*   applied = nullptr;
    int32_t appliedIndex = -1;
    bool    ended = false;  // the actor it was written to is gone
};
std::unordered_map<coop::element::ElementId, Want> g_want;
bool g_recheck = false;
std::vector<AB::Event> g_events;  // reused by every drain
Counters g_counters;              // the drill's read

// The landing's entry, on the game thread: the lane sends it at its next tick.
void OnLandedPost(const SG::Call& call) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host || !call.object) return;
    const int32_t index = R::InternalIndexOf(call.object);
    if (index < 0) return;
    Tree& t = g_trees[call.object];
    t.index = index;
    t.landed = true;
    t.dirty = true;
    g_anyDirty = true;
}

void DrainHost() {
    for (const AB::Event& e : g_events) {
        if (e.kind == AB::Kind::Death) {
            g_trees.erase(e.obj);
            continue;
        }
        if (!R::IsLiveByIndex(e.obj, e.index)) continue;
        Tree& t = g_trees[e.obj];
        t.index = e.index;
        t.dirty = true;
        g_anyDirty = true;
    }
}

// True when the tree is dealt with: sent, or nothing to send. False keeps it dirty for the next tick.
bool SendTree(coop::net::Session& s, void* obj, Tree& t) {
    // An NPC is registered when the host's catch or its world enumeration makes its element, and nothing announces
    // that, so a tree met before then is held and asked again each tick.
    if (t.eid == coop::element::kInvalidId) t.eid = coop::npc_sync::GetNpcIdForActor(obj);
    if (t.eid == coop::element::kInvalidId) {
        const Clock::time_point now = Clock::now();
        if (t.heldSince == Clock::time_point{}) t.heldSince = now;
        if (!t.heldSaid && now - t.heldSince >= kHeldSaid) {
            t.heldSaid = true;
            UE_LOGW("ambient_creature_sync: a walking tree %p has no NPC element after %lld s -- its size is not sent",
                    obj, static_cast<long long>(kHeldSaid.count()));
        }
        return false;
    }
    float scale = 0.f;
    bool shown = false;
    if (!AC::ReadTree(obj, scale, shown)) return false;
    const bool landed = t.landed || shown;
    const bool changed = !t.sent || t.sentScale != scale || t.sentLanded != landed;
    if (!changed && !t.pendingSlots) {
        t.dirty = false;
        return true;
    }
    // The creature drill's red arm: the host's tree state goes nowhere, so a client's tree keeps what its own begin play rolled.
    static const bool s_redNoTreeState =
        coop::config::ResolveEnum(coop::config_registry::rows::ambient_creature_red) == "no_tree_state";
    if (s_redNoTreeState) {
        t.dirty = false;
        t.pendingSlots = 0;
        return true;
    }
    coop::net::AmbientTreeStatePayload p{};
    p.treeEid = static_cast<uint32_t>(t.eid);
    p.scale = scale;
    p.landed = landed ? 1 : 0;
    if (changed) {
        // A change goes to every peer, a joiner waiting for its resend among them.
        if (!s.SendReliable(coop::net::ReliableKind::AmbientTreeState, &p, sizeof(p))) return false;
        t.landed = landed;
        t.sentScale = scale;
        t.sentLanded = landed;
        t.sent = true;
        t.pendingSlots = 0;
        ++g_counters.sent;
    } else {
        // Nothing changed: the joiner alone is told what the others already have.
        for (int slot = 1; slot < coop::players::kMaxPeers; ++slot) {
            if (!(t.pendingSlots & (1u << slot))) continue;
            if (!s.SendReliableToSlot(slot, coop::net::ReliableKind::AmbientTreeState, &p, sizeof(p))) return false;
            t.pendingSlots &= ~(1u << slot);
            ++g_counters.sent;
        }
    }
    t.dirty = false;
    UE_LOGI("ambient_creature_sync: tree %u scale=%.2f landed=%d", p.treeEid, scale, landed ? 1 : 0);
    return true;
}

void HostTick(coop::net::Session& s) {
    DrainHost();
    // A send reaches only a world-ready peer; the trees stay dirty until one is, and the connect replay marks them all for it.
    if (!g_anyDirty || !s.AnyWorldReadyPeer()) return;
    uint32_t connected = 0;
    for (int slot = 1; slot < coop::players::kMaxPeers; ++slot)
        if (s.IsSlotConnected(slot)) connected |= 1u << slot;
    bool held = false;
    for (auto it = g_trees.begin(); it != g_trees.end();) {
        Tree& t = it->second;
        t.pendingSlots &= connected;  // a joiner that left is owed nothing
        if (t.dirty && !SendTree(s, it->first, t)) held = true;
        ++it;
    }
    g_anyDirty = held;
}

void DrainClient() {
    for (const AB::Event& e : g_events) {
        if (e.kind == AB::Kind::Birth) {
            g_recheck = true;
            continue;
        }
        for (auto& kv : g_want) {
            Want& w = kv.second;
            if (w.applied != e.obj || w.appliedIndex != e.index) continue;
            w.applied = nullptr;
            w.ended = true;
            g_recheck = true;
        }
    }
}

// A mirror born for an element is written again, whatever it rolled in its own begin play. An element whose mirror is gone
// is the tree's end, and its want goes.
void ApplyWants() {
    for (auto it = g_want.begin(); it != g_want.end();) {
        Want& w = it->second;
        void* tree = coop::element::LiveActorOfType(it->first, coop::element::ElementType::Npc);
        if (!tree) {
            it = w.ended ? g_want.erase(it) : std::next(it);
            continue;
        }
        if (tree != w.applied) {
            AC::WriteTree(tree, w.scale, w.landed);
            w.applied = tree;
            w.appliedIndex = R::InternalIndexOf(tree);
            w.ended = false;
            ++g_counters.applied;
            UE_LOGI("ambient_creature_sync: tree %u here scale=%.2f landed=%d", it->first, w.scale, w.landed ? 1 : 0);
        }
        ++it;
    }
}

void ClientTick() {
    DrainClient();
    if (g_recheck) {
        g_recheck = false;
        ApplyWants();
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session);
    if (!g_watchTaken) {
        g_watchTaken = SG::WatchName(kRowOnLandedWalkingTree, kTag, nullptr, &OnLandedPost);
        static bool sSaid = false;
        if (!g_watchTaken && !sSaid)
            UE_LOGE("ambient_creature_sync: the watch on a walking tree's landing was refused -- retried each pump");
        sSaid = !g_watchTaken;
    }
}

void Tick() {
    coop::net::Session* s = g_session.load();
    if (!s) return;
    if (g_watchTaken && !g_namesSettled) {
        SG::ResolvePendingNames();
        g_namesSettled = SG::NameWatchSettled(kRowOnLandedWalkingTree, kTag);
        if (g_namesSettled && !SG::NameWatchLive(kRowOnLandedWalkingTree, kTag))
            UE_LOGE("ambient_creature_sync: the watch on a walking tree's landing is dead -- a landing reaches no mirror");
    }
    g_events.clear();
    AB::Drain(AB::Bit(AB::Class::Tree), g_events);
    if (s->role() == coop::net::Role::Host) HostTick(*s);
    else if (s->role() == coop::net::Role::Client) ClientTick();
}

void OnTreeState(const coop::net::AmbientTreeStatePayload& p) {
    Want& w = g_want[static_cast<coop::element::ElementId>(p.treeEid)];
    if (w.scale != p.scale || w.landed != (p.landed != 0)) w.applied = nullptr;
    w.scale = p.scale;
    w.landed = p.landed != 0;
    g_recheck = true;
}

void QueueConnectBroadcastForSlot(int slot) {
    if (slot <= 0 || slot >= coop::players::kMaxPeers) return;
    for (auto& kv : g_trees) {
        kv.second.pendingSlots |= 1u << slot;
        kv.second.dirty = true;
    }
    g_anyDirty = !g_trees.empty();
}

Counters Read() { return g_counters; }

void OnDisconnect() {
    g_counters = Counters{};
    g_trees.clear();
    g_anyDirty = false;
    g_want.clear();
    g_recheck = false;
    g_events.clear();
    g_session.store(nullptr);
}

}  // namespace coop::ambient_creature_sync
