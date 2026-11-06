// coop/creatures/ambient_creature_sync.cpp -- see coop/creatures/ambient_creature_sync.h.

#include "coop/creatures/ambient_creature_sync.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/creatures/npc_sync.h"
#include "coop/creatures/served_player.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/props/prop_drive_host.h"
#include "coop/world/ambient_births.h"

#include "ue_wrap/actors/ambient_creature.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_nav.h"
#include "ue_wrap/engine/engine_pawn.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace coop::ambient_creature_sync {
namespace {

namespace AB = coop::ambient_births;
namespace AC = ue_wrap::ambient_creature;
namespace E = ue_wrap::engine;
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

// The walkers this tick, read once by whichever part of it asks first.
std::vector<void*> g_tickWalkers;
bool g_tickWalkersRead = false;
const std::vector<void*>& TickWalkers() {
    if (!g_tickWalkersRead) {
        g_tickWalkersRead = true;
        g_tickWalkers.clear();
        AC::ListWalkers(g_tickWalkers);
    }
    return g_tickWalkers;
}

// The walkers' poll runs a quarter of a second apart, as the view test and the steering run every tick.
constexpr auto  kWalkerPoll = std::chrono::milliseconds(250);
Clock::time_point g_nextWalkerPoll{};

// CLIENT: where each walker was at the last poll and when it last moved, so a copy's pose shows whether it walks. The
// host's walker jitters a few cm at a path's end, and stands only once it has been still for a moment.
struct WalkerSeen {
    ue_wrap::FVector at{};
    Clock::time_point moved{};
    bool angry = false;
};
std::unordered_map<void*, WalkerSeen> g_walkers;
constexpr float kWalkingCm = 3.f;
constexpr auto  kStandAfter = std::chrono::milliseconds(1500);

// A walker's view test runs more often than the poll: it walks 250 cm in a second.
constexpr auto kWatchPoll = std::chrono::milliseconds(50);
constexpr auto kWatchBeat = std::chrono::seconds(1);      // a client's word while it keeps looking
constexpr auto kWatchExpire = std::chrono::milliseconds(2500);  // a client that left says nothing more
constexpr auto kWatchLinger = std::chrono::seconds(15);    // as long as the walker's own timer stays frozen after a look
Clock::time_point g_nextWatch{};

// HOST: who has each walker on screen, by its key, and when they last said so.
std::unordered_map<std::wstring, std::unordered_map<int, Clock::time_point>> g_watchers;

// HOST: when a walker no client watches any more is let go. Its own timer cannot be relied on: one that fires while the flag
// is held clears it, the hold sets it again before the tick sees the change, and with no new look no timer follows.
std::unordered_map<std::wstring, Clock::time_point> g_lingering;

// CLIENT: the walkers this player has on screen, and when it last said so.
std::unordered_map<std::wstring, Clock::time_point> g_watching;

// HOST: the walkers whose anger went out. CLIENT: the walkers the host said are angry, kept until a copy with the key is here.
std::unordered_set<std::wstring> g_angerSent;
std::unordered_set<std::wstring> g_angryKeys;

// HOST: whom each walker goes for, when that is not the host. Its Blueprint moves toward player 0's camera and judges its
// range by it, and nothing in it names another player; what its tick walks is its own `path`, so a walker nearest a client
// walks a route to that client's body, written over the route to the host. The range is the Blueprint's own.
struct WalkerTarget {
    uint8_t slot = 0;
    std::vector<ue_wrap::FVector> route;
    Clock::time_point nextRoute{};
};
std::unordered_map<void*, WalkerTarget> g_targets;
constexpr float kWalkerRangeCm = 20000.f;
constexpr float kRetargetMarginCm = 300.f;  // two players nearly level do not trade the walker back and forth
constexpr auto  kRouteEvery = std::chrono::milliseconds(500);

float Distance(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

void* PlayerBody(uint8_t slot) {
    auto& reg = coop::players::Registry::Get();
    if (slot == coop::players::kPeerIdHost) return reg.Local();
    coop::RemotePlayer* rp = reg.Puppet(slot);
    return (rp && rp->valid() && rp->HasPose()) ? rp->GetActor() : nullptr;
}

// The nearest player in range, staying with `current` unless another is nearer by the margin.
uint8_t NearestPlayer(const ue_wrap::FVector& at, uint8_t current) {
    uint8_t best = coop::players::kPeerIdHost;
    float bestCm = kWalkerRangeCm;
    float currentCm = -1.f;
    for (int slot = 0; slot < coop::players::kMaxPeers; ++slot) {
        void* body = PlayerBody(static_cast<uint8_t>(slot));
        ue_wrap::FVector p{};
        if (!body || !E::TryGetActorLocation(body, p)) continue;
        const float cm = Distance(at, p);
        if (slot == current) currentCm = cm;
        if (cm < bestCm) {
            bestCm = cm;
            best = static_cast<uint8_t>(slot);
        }
    }
    return currentCm >= 0.f && currentCm <= bestCm + kRetargetMarginCm && currentCm <= kWalkerRangeCm ? current : best;
}

// Every frame while a walker goes for a client, as its own path update can write the route to the host at any moment.
void HostSteerWalkers(Clock::time_point now) {
    const std::vector<void*>& walkers = TickWalkers();
    if (walkers.empty()) {
        g_targets.clear();
        return;
    }
    std::unordered_map<void*, WalkerTarget> kept;
    for (void* w : walkers) {
        ue_wrap::FVector at{};
        if (!E::TryGetActorLocation(w, at)) continue;
        auto was = g_targets.find(w);
        WalkerTarget t = was != g_targets.end() ? std::move(was->second) : WalkerTarget{};
        const uint8_t slot = NearestPlayer(at, t.slot);
        const bool changed = slot != t.slot;
        if (changed) {
            UE_LOGI("ambient_creature_sync: a walker now goes for player slot %u", static_cast<unsigned>(slot));
            t.slot = slot;
            t.nextRoute = now;
        }
        // Its tick stands it while "the player" lies in a ragdoll, which is the player it goes for.
        if (coop::served_player::ServedSlot(w) != slot) {
            if (slot == coop::players::kPeerIdHost) coop::served_player::Unserve(w);
            else coop::served_player::Serve(w, slot, coop::served_player::kMainPlayer);
        }
        // The host is the Blueprint's own target: one route on the way back from a client, and then it is left alone.
        if (slot == coop::players::kPeerIdHost && !changed) {
            kept[w] = std::move(t);
            continue;
        }
        void* body = PlayerBody(slot);
        ue_wrap::FVector goal{};
        if (!body || !E::TryGetActorLocation(body, goal)) continue;
        bool routed = false;
        if (now >= t.nextRoute) {
            t.nextRoute = now + kRouteEvery;
            std::vector<ue_wrap::FVector> route;
            // The ground under the body, by a short box first: a tall one takes a roof above a player indoors.
            ue_wrap::FVector onNav = goal;
            if (!E::ProjectToNav(w, {goal.X, goal.Y, goal.Z - 90.f}, {150.f, 150.f, 150.f}, &onNav))
                E::ProjectToNav(w, goal, {200.f, 200.f, 500.f}, &onNav);
            if (E::FindNavPath(w, at, onNav, route) && !route.empty()) {
                t.route = std::move(route);
                routed = true;
            }
        }
        if (!t.route.empty()) AC::WriteWalkerPath(w, t.route, routed);
        if (slot == coop::players::kPeerIdHost) t.route.clear();
        else if (routed) AC::WalkerEnsureMove(w, goal);
        kept[w] = std::move(t);
    }
    g_targets.swap(kept);
}

// Every frame, as the walker's timer can clear its flag and its tick would walk it on the next.
void HostHoldWatched(Clock::time_point now) {
    if (g_watchers.empty() && g_lingering.empty()) return;
    for (auto it = g_watchers.begin(); it != g_watchers.end();) {
        for (auto w = it->second.begin(); w != it->second.end();)
            w = now - w->second >= kWatchExpire ? it->second.erase(w) : std::next(w);
        if (it->second.empty()) {
            g_lingering[it->first] = now + kWatchLinger;
            it = g_watchers.erase(it);
        } else {
            ++it;
        }
    }
    for (void* w : TickWalkers()) {
        const std::wstring key = ue_wrap::prop::GetKeyString(w);
        if (g_watchers.count(key)) {
            AC::HoldWalkerSeen(w);
            continue;
        }
        auto linger = g_lingering.find(key);
        if (linger == g_lingering.end() || now < linger->second) continue;
        // A host that has it on screen sets the flag again on its next tick.
        AC::ReleaseWalkerSeen(w);
        g_lingering.erase(linger);
    }
}

void ClientWatch(coop::net::Session& s, Clock::time_point now) {
    const std::vector<void*>& walkers = TickWalkers();
    if (walkers.empty() && g_watching.empty()) return;
    void* pawn = coop::players::Registry::Get().Local();
    void* controller = pawn ? E::GetController(pawn) : nullptr;
    std::unordered_map<std::wstring, Clock::time_point> still;
    auto send = [&s](const std::wstring& key, bool watching) {
        coop::net::WalkerWatchPayload p{};
        coop::net::WireKeyFromString(key, p.key);
        p.watching = watching ? 1 : 0;
        return s.SendReliable(coop::net::ReliableKind::WalkerWatch, &p, sizeof(p));
    };
    for (void* w : walkers) {
        if (!AC::WalkerInView(w, controller)) continue;
        const std::wstring key = ue_wrap::prop::GetKeyString(w);
        if (key.empty()) continue;
        auto was = g_watching.find(key);
        if (was != g_watching.end() && now - was->second < kWatchBeat) {
            still[key] = was->second;
        } else if (send(key, true)) {
            still[key] = now;
            if (was == g_watching.end()) UE_LOGI("ambient_creature_sync: this player sees walker '%ls'", key.c_str());
        }
        // The pose the host's walker takes in a moment, without its round trip. An angry one does not stop.
        if (!AC::ReadWalkerAngry(w)) AC::WriteWalkerStanding(w, true);
    }
    for (const auto& entry : g_watching)
        if (!still.count(entry.first) && send(entry.first, false)) UE_LOGI("ambient_creature_sync: this player no longer sees walker '%ls'", entry.first.c_str());
    g_watching.swap(still);
}

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


// HOST: the walkers' share of the 250 ms poll.
void HostWalkerPoll(coop::net::Session& s) {
    // A walker's brain moves it with no hand and no verb, so the host's drive stream holds it for as long as it stands.
    for (void* w : TickWalkers()) {
        coop::prop_drive_host::Claim(w, "walker");
        // An anger is the game's alone to start, by a fire, and nothing in the game undoes it.
        if (!AC::ReadWalkerAngry(w)) continue;
        const std::wstring key = ue_wrap::prop::GetKeyString(w);
        if (key.empty() || g_angerSent.count(key)) continue;
        coop::net::WalkerWatchPayload p{};
        coop::net::WireKeyFromString(key, p.key);
        p.angered = 1;
        if (!s.SendReliable(coop::net::ReliableKind::WalkerAngry, &p, sizeof(p))) continue;
        g_angerSent.insert(key);
        UE_LOGI("ambient_creature_sync: walker '%ls' is angry -- told the clients", key.c_str());
    }
}

// CLIENT: the walkers' share of the 250 ms poll.
void ClientWalkerPoll(coop::net::Session& s, Clock::time_point now) {
    std::unordered_map<void*, WalkerSeen> walkersNow;
    for (void* w : TickWalkers()) {
        auto was = g_walkers.find(w);
        ue_wrap::FVector at{};
        if (!E::TryGetActorLocation(w, at)) continue;
        WalkerSeen seen{at, now};
        // A fire on this copy angers only this copy; the walker that hits is the host's, so the anger goes there. One the host
        // told of goes nowhere.
        const std::wstring walkerKey = ue_wrap::prop::GetKeyString(w);
        const bool hostSaid = !walkerKey.empty() && g_angryKeys.count(walkerKey) != 0;
        if (hostSaid && !AC::ReadWalkerAngry(w)) {
            AC::WriteWalkerAngry(w);
            UE_LOGI("ambient_creature_sync: the host's walker '%ls' is angry here", walkerKey.c_str());
        }
        const bool angry = AC::ReadWalkerAngry(w);
        bool told = hostSaid || (was != g_walkers.end() && was->second.angry);
        if (angry && !told && !walkerKey.empty()) {
            coop::net::WalkerWatchPayload p{};
            coop::net::WireKeyFromString(walkerKey, p.key);
            p.watching = g_watching.count(walkerKey) ? 1 : 0;
            p.angered = 1;
            if (s.SendReliable(coop::net::ReliableKind::WalkerWatch, &p, sizeof(p))) {
                told = true;
                UE_LOGI("ambient_creature_sync: this player angered walker '%ls'", walkerKey.c_str());
            }
        }
        if (was != g_walkers.end()) {
            const float dx = at.X - was->second.at.X, dy = at.Y - was->second.at.Y;
            if (std::sqrt(dx * dx + dy * dy) < kWalkingCm) seen = was->second;
            seen.angry = told;
            if (!g_watching.count(ue_wrap::prop::GetKeyString(w))) AC::WriteWalkerStanding(w, now - seen.moved >= kStandAfter);
        }
        seen.angry = told;
        walkersNow[w] = seen;
    }
    g_walkers.swap(walkersNow);
}

// The walking mannequins' part of the tick: the host holds and steers them every tick, a client tests its view every 50 ms, and
// both walk the walkers' list at the 250 ms poll.
void WalkerTick(coop::net::Session& s) {
    const Clock::time_point now = Clock::now();
    g_tickWalkersRead = false;
    if (s.role() == coop::net::Role::Host) {
        HostHoldWatched(now);
        HostSteerWalkers(now);
    } else if (s.role() == coop::net::Role::Client && now >= g_nextWatch) {
        g_nextWatch = now + kWatchPoll;
        ClientWatch(s, now);
    }
    if (now < g_nextWalkerPoll) return;
    g_nextWalkerPoll = now + kWalkerPoll;
    if (s.role() == coop::net::Role::Host) HostWalkerPoll(s);
    else if (s.role() == coop::net::Role::Client) ClientWalkerPoll(s, now);
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
    WalkerTick(*s);
}

void OnTreeState(const coop::net::AmbientTreeStatePayload& p) {
    Want& w = g_want[static_cast<coop::element::ElementId>(p.treeEid)];
    if (w.scale != p.scale || w.landed != (p.landed != 0)) w.applied = nullptr;
    w.scale = p.scale;
    w.landed = p.landed != 0;
    g_recheck = true;
}

void OnWalkerWatch(const coop::net::WalkerWatchPayload& p, int senderSlot) {
    const std::wstring key = coop::net::StringFromWireKey(p.key);
    if (p.angered) {
        std::vector<void*> walkers;
        AC::ListWalkers(walkers);
        for (void* w : walkers) {
            if (ue_wrap::prop::GetKeyString(w) != key) continue;
            AC::WriteWalkerAngry(w);
            UE_LOGI("ambient_creature_sync: slot %d angered walker '%ls'", senderSlot, key.c_str());
        }
    }
    if (p.watching) {
        g_watchers[key][senderSlot] = Clock::now();
        g_lingering.erase(key);
        return;
    }
    auto it = g_watchers.find(key);
    if (it == g_watchers.end()) return;
    it->second.erase(senderSlot);
    if (!it->second.empty()) return;
    g_lingering[key] = Clock::now() + kWatchLinger;
    g_watchers.erase(it);
}

void OnWalkerAngry(const coop::net::WalkerWatchPayload& p) {
    const std::wstring key = coop::net::StringFromWireKey(p.key);
    if (!key.empty() && g_angryKeys.insert(key).second) UE_LOGI("ambient_creature_sync: the host says walker '%ls' is angry", key.c_str());
}

void QueueConnectBroadcastForSlot(int slot) {
    if (slot <= 0 || slot >= coop::players::kMaxPeers) return;
    g_angerSent.clear();
    for (auto& kv : g_trees) {
        kv.second.pendingSlots |= 1u << slot;
        kv.second.dirty = true;
    }
    g_anyDirty = !g_trees.empty();
}

Counters Read() { return g_counters; }

void OnDisconnect() {
    g_walkers.clear();
    g_watchers.clear();
    g_lingering.clear();
    g_watching.clear();
    g_angerSent.clear();
    g_angryKeys.clear();
    g_targets.clear();
    g_nextWalkerPoll = Clock::time_point{};
    g_nextWatch = Clock::time_point{};
    g_counters = Counters{};
    g_trees.clear();
    g_anyDirty = false;
    g_want.clear();
    g_recheck = false;
    g_events.clear();
    g_session.store(nullptr);
}

}  // namespace coop::ambient_creature_sync
