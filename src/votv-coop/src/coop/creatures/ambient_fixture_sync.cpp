// coop/creatures/ambient_fixture_sync.cpp -- see coop/creatures/ambient_fixture_sync.h.

#include "coop/creatures/ambient_fixture_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"
#include "coop/session/poll_timer.h"

#include "ue_wrap/actors/ambient_creature.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/world_identity.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace coop::ambient_fixture_sync {
namespace {

namespace AC = ue_wrap::ambient_creature;
namespace E = ue_wrap::engine;
namespace R = ue_wrap::reflection;
using Clock = std::chrono::steady_clock;
using Payload = coop::net::AmbientFixtureStatePayload;

constexpr int   kSendsPerPoll = 64;     // a hexahive's field arrives whole in one poll
constexpr float kAdoptCm = 50.f;        // a loaded branch at the host's place is that branch

std::atomic<coop::net::Session*> g_session{nullptr};
coop::session::PollTimer g_poll{std::chrono::milliseconds(250)};
uint32_t g_beehiveWorld = 0;   // the world whose beehive timer this client has paused; 0 until it holds

// HOST: each fixture's number and place, and whether it still has to go out.
struct Known {
    Payload wire{};
    bool pending = true;
};
std::unordered_map<void*, Known> g_known;
uint32_t g_nextId = 1;

// HOST: when each client's next sus hole roll is due, as the spawner's tick sets its own interval: 300 to 600 s.
std::unordered_map<uint8_t, Clock::time_point> g_nextHole;
std::mt19937 g_rng{std::random_device{}()};
uint32_t g_holesMade = 0, g_holesMissed = 0;

// HOST: the holes rolled around a client. A hole ends 10 minutes on unless player 0 is within 50 m or has it on screen, and on
// the host that is the host, so this copy's end is not sent: each copy ends by its own viewer, the client's by the client.
std::unordered_set<void*> g_clientHoles;

float Uniform(float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(g_rng); }

// The spawner's roll, around `around` instead of player 0's camera: 100 to 500 m off at any angle, on grass or nothing.
bool RollHole(const ue_wrap::FVector& around, uint8_t slot) {
    const float angle = Uniform(0.f, 360.f) / 57.29578f, dist = Uniform(10000.f, 50000.f);
    const ue_wrap::FVector point{around.X + dist * std::cos(angle), around.Y + dist * std::sin(angle), around.Z};
    ue_wrap::FVector at{};
    ue_wrap::FRotator rot{};
    if (!AC::HoleSpot(point, at, rot)) {
        ++g_holesMissed;
        return false;
    }
    void* hole = AC::SpawnFixture(false, at, rot);
    if (!hole) return false;
    g_clientHoles.insert(hole);
    ++g_holesMade;
    UE_LOGI("ambient_fixture_sync: a sus hole rolled around slot %u, %.0f m off (made %u, missed %u)", static_cast<unsigned>(slot),
            dist / 100.f, g_holesMade, g_holesMissed);
    return true;
}

bool ClientBody(uint8_t slot, ue_wrap::FVector& at) {
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(slot);
    void* body = rp && rp->valid() && rp->HasPose() ? rp->GetActor() : nullptr;
    return body && E::TryGetActorLocation(body, at);
}

void HostRollHoles(Clock::time_point now) {
    std::unordered_map<uint8_t, Clock::time_point> next;
    for (uint8_t slot = 1; slot < coop::players::kMaxPeers; ++slot) {
        ue_wrap::FVector at{};
        if (!ClientBody(slot, at)) continue;
        auto it = g_nextHole.find(slot);
        // The spawner's first tick is at its begin play, so a player's first roll is when it is first here.
        Clock::time_point due = it == g_nextHole.end() ? now : it->second;
        if (now >= due) {
            if (AC::HolesRoll()) RollHole(at, slot);
            due = now + std::chrono::milliseconds(static_cast<int64_t>(Uniform(300.f, 600.f) * 1000.f));
        }
        next[slot] = due;
    }
    g_nextHole.swap(next);
}

// CLIENT.
std::vector<Payload> g_inbox;
std::unordered_map<uint32_t, void*> g_copies;     // by the host's number; adopted branches too
std::unordered_set<void*> g_spawned;              // the copies this lane made
std::unordered_set<void*> g_loaded;               // the branches the load brought
bool g_loadedRead = false;

void List(std::vector<void*>& holes, std::vector<void*>& branches) {
    AC::ListHoles(holes);
    AC::ListBranches(branches);
}

void HostPoll(coop::net::Session& s, Clock::time_point now) {
    HostRollHoles(now);
    std::vector<void*> holes, branches;
    List(holes, branches);
    std::unordered_set<void*> alive;
    auto note = [&](void* a, uint8_t family) {
        alive.insert(a);
        if (g_known.count(a)) return;
        ue_wrap::FVector at{};
        ue_wrap::FRotator rot{};
        if (!E::TryGetActorLocation(a, at) || !E::TryGetActorRotation(a, rot)) return;
        Known k;
        k.wire.id = g_nextId++;
        k.wire.x = at.X;
        k.wire.y = at.Y;
        k.wire.z = at.Z;
        k.wire.pitch = rot.Pitch;
        k.wire.yaw = rot.Yaw;
        k.wire.roll = rot.Roll;
        k.wire.family = family;
        k.wire.alive = 1;
        g_known[a] = k;
    };
    for (void* a : holes) note(a, coop::net::ambient_fixture::kHole);
    for (void* a : branches) note(a, coop::net::ambient_fixture::kBranch);

    int sends = 0;
    for (auto it = g_known.begin(); it != g_known.end();) {
        if (alive.count(it->first)) {
            if (it->second.pending && sends < kSendsPerPoll &&
                s.SendReliable(coop::net::ReliableKind::AmbientFixtureState, &it->second.wire, sizeof(Payload))) {
                it->second.pending = false;
                ++sends;
            }
            ++it;
            continue;
        }
        // Never sent means no client has it, and a join sends only the living.
        Payload end = it->second.wire;
        end.alive = 0;
        const bool clientHole = g_clientHoles.erase(it->first) != 0;
        if (!it->second.pending && !clientHole && !s.SendReliable(coop::net::ReliableKind::AmbientFixtureState, &end, sizeof(end))) {
            ++it;
            continue;
        }
        UE_LOGI("ambient_fixture_sync: fixture %u family=%u ended%s", end.id, end.family, clientHole ? " here alone (a client's hole)" : "");
        it = g_known.erase(it);
    }
    if (sends) UE_LOGI("ambient_fixture_sync: sent %d fixtures, %zu stand", sends, g_known.size());
}

void* AdoptLoaded(const Payload& p) {
    for (void* a : g_loaded) {
        ue_wrap::FVector at{};
        if (!R::IsLive(a) || !E::TryGetActorLocation(a, at)) continue;
        const float dx = at.X - p.x, dy = at.Y - p.y, dz = at.Z - p.z;
        if (std::sqrt(dx * dx + dy * dy + dz * dz) < kAdoptCm) return a;
    }
    return nullptr;
}

void ApplyInbox() {
    std::vector<Payload> inbox;
    inbox.swap(g_inbox);
    for (const Payload& p : inbox) {
        auto it = g_copies.find(p.id);
        if (!p.alive) {
            if (it == g_copies.end()) continue;
            if (it->second && R::IsLive(it->second)) AC::Destroy(it->second);
            g_spawned.erase(it->second);
            g_loaded.erase(it->second);
            g_copies.erase(it);
            UE_LOGI("ambient_fixture_sync: fixture %u ended here", p.id);
            continue;
        }
        if (it != g_copies.end()) continue;
        void* adopted = p.family == coop::net::ambient_fixture::kBranch ? AdoptLoaded(p) : nullptr;
        if (adopted) {
            g_copies[p.id] = adopted;
            continue;
        }
        void* copy = AC::SpawnFixture(p.family == coop::net::ambient_fixture::kBranch, {p.x, p.y, p.z}, {p.pitch, p.yaw, p.roll});
        if (!copy) {
            UE_LOGW("ambient_fixture_sync: fixture %u family=%u did not spawn", p.id, p.family);
            continue;
        }
        g_copies[p.id] = copy;
        g_spawned.insert(copy);
    }
}

void ClientPoll() {
    const uint32_t world = ue_wrap::world_identity::Generation();
    if (g_beehiveWorld != world && AC::PauseBeehiveTimers(true)) g_beehiveWorld = world;
    // The save's branches come with its objects, late in the load: one read before then finds none, and a loaded branch swept
    // as this client's own takes its nest down with it. The host's arrive after, onto what the load made.
    if (!coop::net_pump::HasAnnouncedWorldReady() || coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle) return;
    std::vector<void*> holes, branches;
    List(holes, branches);
    if (!g_loadedRead) {
        g_loadedRead = true;
        g_loaded.insert(branches.begin(), branches.end());
        UE_LOGI("ambient_fixture_sync: the load brought %zu branches", g_loaded.size());
    }
    ApplyInbox();
    // After the inbox, so what it just made is a copy. The list was read before it, and a new copy is not on it.
    auto sweep = [](void* a) {
        if (g_spawned.count(a) || g_loaded.count(a)) return;
        const bool gone = AC::Destroy(a);
        UE_LOGI("ambient_fixture_sync: swept this client's own '%ls' (%d)", R::ToString(R::NameOf(R::ClassOf(a))).c_str(), gone ? 1 : 0);
    };
    for (void* a : holes) sweep(a);
    for (void* a : branches) sweep(a);
}

}  // namespace

void Install(coop::net::Session* session) { g_session.store(session); }

void Tick() {
    coop::net::Session* s = g_session.load();
    if (!s) return;
    const Clock::time_point now = Clock::now();
    if (!g_poll.Due(now)) return;
    if (s->role() == coop::net::Role::Host) HostPoll(*s, now);
    else if (s->role() == coop::net::Role::Client) ClientPoll();
}

void OnState(const coop::net::AmbientFixtureStatePayload& p) { g_inbox.push_back(p); }

void QueueConnectBroadcastForSlot(int /*slot*/) {
    for (auto& entry : g_known) entry.second.pending = true;
}

void OnDisconnect() {
    if (g_session.load() && g_session.load()->role() == coop::net::Role::Client) {
        AC::PauseBeehiveTimers(false);
        for (void* a : g_spawned)
            if (R::IsLive(a)) AC::Destroy(a);
    }
    g_known.clear();
    g_nextId = 1;
    g_nextHole.clear();
    g_clientHoles.clear();
    g_inbox.clear();
    g_copies.clear();
    g_spawned.clear();
    g_loaded.clear();
    g_loadedRead = false;
    g_beehiveWorld = 0;
    g_session.store(nullptr);
}

bool DebugRollHoleNear(int slot) {
    ue_wrap::FVector at{};
    if (slot <= 0 || slot >= coop::players::kMaxPeers || !ClientBody(static_cast<uint8_t>(slot), at)) return false;
    return RollHole(at, static_cast<uint8_t>(slot));
}

}  // namespace coop::ambient_fixture_sync
