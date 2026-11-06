// coop/creatures/walker_sync.cpp -- see coop/creatures/walker_sync.h.

#include "coop/creatures/walker_sync.h"

#include "coop/creatures/served_player.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/props/prop_drive_host.h"
#include "coop/session/poll_timer.h"

#include "ue_wrap/actors/ambient_creature.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_nav.h"
#include "ue_wrap/engine/engine_pawn.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace coop::walker_sync {
namespace {

namespace AC = ue_wrap::ambient_creature;
namespace E = ue_wrap::engine;
namespace R = ue_wrap::reflection;
using Clock = std::chrono::steady_clock;

std::atomic<coop::net::Session*> g_session{nullptr};
coop::session::PollTimer g_poll{std::chrono::milliseconds(250)};

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
bool g_parkedAny = false;

// CLIENT: where each walker was at the last poll and when it last moved, so a parked copy's pose shows whether it walks. The
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
            UE_LOGI("walker_sync: a walker now goes for player slot %u", static_cast<unsigned>(slot));
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
            if (was == g_watching.end()) UE_LOGI("walker_sync: this player sees walker '%ls'", key.c_str());
        }
        // The pose the host's walker takes in a moment, without its round trip. An angry one does not stop.
        if (!AC::ReadWalkerAngry(w)) AC::WriteWalkerStanding(w, true);
    }
    for (const auto& entry : g_watching)
        if (!still.count(entry.first) && send(entry.first, false)) UE_LOGI("walker_sync: this player no longer sees walker '%ls'", entry.first.c_str());
    g_watching.swap(still);
}

void HostPoll(coop::net::Session& s) {
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
        UE_LOGI("walker_sync: walker '%ls' is angry -- told the clients", key.c_str());
    }
}

void ClientPoll(coop::net::Session& s, Clock::time_point now) {
    std::vector<void*> spawners;
    AC::ListSpawners(spawners);
    for (void* spawner : spawners) {
        if (!E::IsActorTickEnabled(spawner)) continue;
        E::SetActorTickEnabled(spawner, false);
        g_parkedAny = true;
        UE_LOGI("walker_sync: parked a spawner of '%ls'", R::ToString(R::NameOf(R::ClassOf(spawner))).c_str());
    }

    std::unordered_map<void*, WalkerSeen> walkersNow;
    for (void* w : TickWalkers()) {
        if (E::IsActorTickEnabled(w)) {
            AC::ParkWalker(w);
            g_parkedAny = true;
            UE_LOGI("walker_sync: parked a walker's brain");
        }
        ue_wrap::FVector at{};
        if (!E::TryGetActorLocation(w, at)) continue;
        WalkerSeen seen{at, now};
        auto was = g_walkers.find(w);
        // A fire on this copy angers only this copy; the walker that hits is the host's, so the anger goes there. One the host
        // told of goes nowhere.
        const std::wstring walkerKey = ue_wrap::prop::GetKeyString(w);
        const bool hostSaid = !walkerKey.empty() && g_angryKeys.count(walkerKey) != 0;
        if (hostSaid && !AC::ReadWalkerAngry(w)) {
            AC::WriteWalkerAngry(w);
            UE_LOGI("walker_sync: the host's walker '%ls' is angry here", walkerKey.c_str());
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
                UE_LOGI("walker_sync: this player angered walker '%ls'", walkerKey.c_str());
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

}  // namespace

void Install(coop::net::Session* session) { g_session.store(session); }

void Tick() {
    coop::net::Session* s = g_session.load();
    if (!s) return;
    const Clock::time_point now = Clock::now();
    g_tickWalkersRead = false;
    if (s->role() == coop::net::Role::Host) {
        HostHoldWatched(now);
        HostSteerWalkers(now);
    } else if (s->role() == coop::net::Role::Client && now >= g_nextWatch) {
        g_nextWatch = now + kWatchPoll;
        ClientWatch(*s, now);
    }
    if (!g_poll.Due(now)) return;
    if (s->role() == coop::net::Role::Host) HostPoll(*s);
    else if (s->role() == coop::net::Role::Client) ClientPoll(*s, now);
}

void OnWalkerWatch(const coop::net::WalkerWatchPayload& p, int senderSlot) {
    const std::wstring key = coop::net::StringFromWireKey(p.key);
    if (p.angered) {
        std::vector<void*> walkers;
        AC::ListWalkers(walkers);
        for (void* w : walkers) {
            if (ue_wrap::prop::GetKeyString(w) != key) continue;
            AC::WriteWalkerAngry(w);
            UE_LOGI("walker_sync: slot %d angered walker '%ls'", senderSlot, key.c_str());
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
    if (!key.empty() && g_angryKeys.insert(key).second) UE_LOGI("walker_sync: the host says walker '%ls' is angry", key.c_str());
}

void QueueConnectBroadcastForSlot(int /*slot*/) { g_angerSent.clear(); }

void OnDisconnect() {
    if (g_parkedAny) {
        std::vector<void*> spawners;
        AC::ListSpawners(spawners);
        for (void* spawner : spawners) E::SetActorTickEnabled(spawner, true);
        std::vector<void*> walkers;
        AC::ListWalkers(walkers);
        for (void* w : walkers) AC::UnparkWalker(w);
    }
    g_parkedAny = false;
    g_walkers.clear();
    g_watchers.clear();
    g_lingering.clear();
    g_watching.clear();
    g_angerSent.clear();
    g_angryKeys.clear();
    g_targets.clear();
    g_session.store(nullptr);
}

}  // namespace coop::walker_sync
