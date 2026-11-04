// coop/creatures/ambient_creature_sync.cpp -- see coop/creatures/ambient_creature_sync.h.

#include "coop/creatures/ambient_creature_sync.h"

#include "coop/creatures/npc_sync.h"
#include "coop/element/mirror_managers.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/session/poll_timer.h"

#include "ue_wrap/actors/ambient_creature.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace coop::ambient_creature_sync {
namespace {

namespace AC = ue_wrap::ambient_creature;
namespace E = ue_wrap::engine;
namespace R = ue_wrap::reflection;
using Clock = std::chrono::steady_clock;

constexpr auto  kHeartbeat = std::chrono::seconds(5);  // a mirror that came after the last send
constexpr float kDissolveCm = 5000.f;                  // deer_C's ReceiveTick compares the camera with this

std::atomic<coop::net::Session*> g_session{nullptr};
coop::session::PollTimer g_poll{std::chrono::milliseconds(250)};

bool g_parkedAny = false;

// HOST: what went out last, by the tree's element.
struct Sent {
    float scale = 0.f;
    bool landed = false;
    Clock::time_point at{};
};
std::unordered_map<coop::element::ElementId, Sent> g_sent;

// CLIENT: the host's tree, kept until its mirror exists, and whether it has been written.
struct Want {
    float scale = 0.f;
    bool landed = false;
    bool applied = false;
};
std::unordered_map<coop::element::ElementId, Want> g_want;
std::unordered_set<void*> g_dissolving;

void HostPoll(coop::net::Session& s, Clock::time_point now) {
    std::vector<void*> trees;
    AC::ListTrees(trees);
    std::unordered_set<coop::element::ElementId> alive;
    for (void* tree : trees) {
        const coop::element::ElementId eid = coop::npc_sync::GetNpcIdForActor(tree);
        if (eid == 0) continue;
        alive.insert(eid);
        float scale = 0.f;
        bool landed = false;
        if (!AC::ReadTree(tree, scale, landed)) continue;
        auto it = g_sent.find(eid);
        const bool changed = it == g_sent.end() || it->second.scale != scale || it->second.landed != landed;
        if (!changed && now - it->second.at < kHeartbeat) continue;
        coop::net::AmbientTreeStatePayload p{};
        p.treeEid = static_cast<uint32_t>(eid);
        p.scale = scale;
        p.landed = landed ? 1 : 0;
        if (!s.SendReliable(coop::net::ReliableKind::AmbientTreeState, &p, sizeof(p))) continue;
        if (changed) UE_LOGI("ambient_creature_sync: tree %u scale=%.2f landed=%d", p.treeEid, scale, landed ? 1 : 0);
        g_sent[eid] = Sent{scale, landed, now};
    }
    for (auto it = g_sent.begin(); it != g_sent.end();)
        it = alive.count(it->first) ? std::next(it) : g_sent.erase(it);
}

void ClientPoll(coop::net::Session& /*s*/, Clock::time_point now) {
    std::vector<void*> spawners;
    AC::ListSpawners(spawners);
    for (void* spawner : spawners) {
        if (!E::IsActorTickEnabled(spawner)) continue;
        E::SetActorTickEnabled(spawner, false);
        g_parkedAny = true;
        UE_LOGI("ambient_creature_sync: parked a spawner of '%ls'", R::ToString(R::NameOf(R::ClassOf(spawner))).c_str());
    }

    std::vector<coop::element::Npc*> mirrors;
    coop::element::NpcMirrors().Snapshot(mirrors);
    std::unordered_set<void*> tracked;
    for (coop::element::Npc* m : mirrors)
        if (m && m->GetActor()) tracked.insert(m->GetActor());

    std::vector<void*> deer, trees;
    AC::ListDeer(deer);
    AC::ListTrees(trees);
    // One this client rolled before its spawner was parked, or that a load brought. Only these classes: the npc lane's own
    // sweep would take a save's kerfur still waiting for its adoption.
    auto sweep = [&tracked](void* a) {
        if (tracked.count(a)) return false;
        const bool gone = AC::Destroy(a);
        UE_LOGI("ambient_creature_sync: swept this client's own '%ls' (%d)", R::ToString(R::NameOf(R::ClassOf(a))).c_str(), gone ? 1 : 0);
        return true;
    };
    for (void* a : trees) sweep(a);

    const ue_wrap::FVector camera = E::GetCameraLocation();
    std::unordered_set<void*> stillDissolving;
    for (void* a : deer) {
        if (sweep(a)) continue;
        if (g_dissolving.count(a)) {
            stillDissolving.insert(a);
            continue;
        }
        ue_wrap::FVector at{};
        if (!E::TryGetActorLocation(a, at)) continue;
        const float dx = at.X - camera.X, dy = at.Y - camera.Y, dz = at.Z - camera.Z;
        if (std::sqrt(dx * dx + dy * dy + dz * dz) >= kDissolveCm) continue;
        if (AC::Dissolve(a)) {
            stillDissolving.insert(a);
            UE_LOGI("ambient_creature_sync: a deer's mirror dissolves for this viewer");
        }
    }
    g_dissolving.swap(stillDissolving);

    for (auto it = g_want.begin(); it != g_want.end(); ++it) {
        if (it->second.applied) continue;
        void* tree = coop::element::LiveActorOfType(it->first, coop::element::ElementType::Npc);
        if (!tree) continue;
        AC::WriteTree(tree, it->second.scale, it->second.landed);
        it->second.applied = true;
        UE_LOGI("ambient_creature_sync: tree %u here scale=%.2f landed=%d", it->first, it->second.scale, it->second.landed ? 1 : 0);
    }
}

}  // namespace

void Install(coop::net::Session* session) { g_session.store(session); }

void Tick() {
    coop::net::Session* s = g_session.load();
    if (!s) return;
    const Clock::time_point now = Clock::now();
    if (!g_poll.Due(now)) return;
    if (s->role() == coop::net::Role::Host) HostPoll(*s, now);
    else if (s->role() == coop::net::Role::Client) ClientPoll(*s, now);
}

void OnTreeState(const coop::net::AmbientTreeStatePayload& p) {
    Want& w = g_want[static_cast<coop::element::ElementId>(p.treeEid)];
    if (w.scale != p.scale || w.landed != (p.landed != 0)) w.applied = false;
    w.scale = p.scale;
    w.landed = p.landed != 0;
}

void QueueConnectBroadcastForSlot(int /*slot*/) {
    g_sent.clear();
}

void OnDisconnect() {
    if (g_parkedAny) {
        std::vector<void*> spawners;
        AC::ListSpawners(spawners);
        for (void* spawner : spawners) E::SetActorTickEnabled(spawner, true);
    }
    g_parkedAny = false;
    g_sent.clear();
    g_want.clear();
    g_dissolving.clear();
    g_session.store(nullptr);
}

}  // namespace coop::ambient_creature_sync
