// coop/dev/door_drill.cpp -- see coop/dev/door_drill.h.

#include "coop/dev/door_drill.h"

#include "coop/config/config.h"
#include "coop/dev/director/director.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/roster.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/devices/door.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace coop::dev::door_drill {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace D  = ue_wrap::door;
namespace GT = ue_wrap::game_thread;
namespace DR = coop::director;

constexpr auto  kSampleEvery = std::chrono::milliseconds(250);
constexpr float kReachCm     = 60.f;    // at the approach point, not merely near it
constexpr float kApproachCm  = 90.f;    // in front of or behind the leaf: a closed door's own origin
                                        // sits in its nav modifier, where no route ends
constexpr float kRouteEndCm  = 100.f;   // a route that ends farther from its point never gets there
constexpr int   kCandidates  = 12;      // the nearest doors by straight distance; each costs two routes
constexpr float kAwayCm      = 1500.f;  // the walk away: this far back along the route, out of any sensor
constexpr float kInSensorCm  = 30.f;    // at the sensor box's centre: where the autoclose counts a player
constexpr int   kOpenWaitMs  = 5000;    // the host's answer and the swing's start; a door that has not
                                        // started opening by then was refused
// The stay in the doorway: longer than the door's own five-second sensor check, so the reading
// covers what the game does with the sensor while a player stands in it.
constexpr DWORD kDwellMs     = 8000;
// After the walk away the host's door closes at its first five-second check that finds the sensor
// empty, then this copy follows its broadcast. Three checks' worth is the bound: a door still open
// by then was not closed by its autoclose.
constexpr int   kCloseWaitMs = 15000;
// The HIT phase: a held weapon's damage, a hit a player's swing apart, until the host's pry opens
// the door or the hits run out. A hit moves the leaves toward open at an interpolation speed of
// (damage / 25)^1.5 * 5 over a 0.01 s step, and the door opens once the right leaf is more than 90
// from its open offset of 70 (door_C::addDamage, tools/bp_cpp.py): a 10-damage swing (the crowbar, a
// mop, a broom; a player with nothing held does not swing) needs about 27 hits, 50 needs three.
constexpr float kHitDamage   = 50.f;
constexpr int   kHitMax      = 8;
constexpr DWORD kHitEveryMs  = 700;
constexpr int   kReadMax     = 16;

const char* Side() { return coop::roster::LocalIsHost() ? "host" : "client"; }

float HorizDist(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y;
    return std::sqrt(dx * dx + dy * dy);
}

// The reading: every door of this world and what its sensor held when last logged.
struct Door {
    void*        actor;
    int32_t      idx;
    std::wstring name;
    int          overlaps = -2;   // -2: not read yet
    int          puppets = 0;
    int          locals = 0;
    int          open = -1;       // the settled open flag at the last reading; -1: unread
};
std::vector<Door> g_doors;
bool g_listed = false;
std::chrono::steady_clock::time_point g_nextSample{};
int g_puppetEntries = 0;   // readings in which a puppet was newly inside a door's sensor

std::atomic<bool> g_walkerStarted{false};

// The milestone the rig itself waits on, so the drill starts on the world it measures: a client
// once the host's snapshot is applied, the host once a client is in.
bool RoleIsReady(coop::net::Session& s) {
    if (coop::roster::LocalIsHost()) return s.connectedPeerCount() > 0;
    return coop::net_pump::HasAnnouncedWorldReady() &&
           coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle;
}

// One walk of the object array per world, for the drill only.
void ListDoors() {
    g_doors.clear();
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* o = R::ObjectAt(i);
        if (!o || !R::IsLive(o) || !D::IsDoor(o)) continue;
        if (R::NameStartsWith(R::NameOf(o), L"Default__")) continue;
        g_doors.push_back(Door{o, R::InternalIndexOf(o), R::ToString(R::NameOf(o))});
    }
    g_listed = true;
    UE_LOGI("[DOOR-DRILL] %s reads the sensors of %zu doors", Side(), g_doors.size());
}

void Sample() {
    auto& reg = coop::players::Registry::Get();
    int firstNonEmpty = 0, firstUnread = 0;
    bool first = false;
    for (Door& d : g_doors) {
        if (!R::IsLiveByIndex(d.actor, d.idx)) continue;
        void* held[kReadMax];
        const int n = D::ReadSensorOverlaps(d.actor, held, kReadMax);
        int puppets = 0, locals = 0;
        for (int i = 0; i < n && i < kReadMax; ++i) {
            if (reg.IsPuppet(held[i])) ++puppets;
            else if (reg.IsLocal(held[i])) ++locals;
        }
        // The door's own state too: an autoclose after its sensor empties is a change of this flag
        // alone, and it is the reading the door lane's fix is judged on.
        bool openNow = false;
        const int open = D::TryReadOpen(d.actor, openNow) ? (openNow ? 1 : 0) : -1;
        if (n == d.overlaps && puppets == d.puppets && locals == d.locals && open == d.open) continue;
        const bool firstReading = d.overlaps == -2;
        const int prevPuppets = d.puppets;
        first = first || firstReading;
        d.overlaps = n;
        d.puppets = puppets;
        d.locals = locals;
        d.open = open;
        // The first reading of an empty sensor says nothing; the summary below counts it.
        if (firstReading && n == 0) continue;
        if (firstReading && n < 0) { ++firstUnread; continue; }
        if (firstReading) ++firstNonEmpty;
        else if (puppets > prevPuppets) ++g_puppetEntries;
        UE_LOGI("[DOOR-DRILL] %s door=%ls sensor holds %d (puppets %d, local players %d) open=%d",
                Side(), d.name.c_str(), n, puppets, locals, open);
    }
    if (first)
        UE_LOGI("[DOOR-DRILL] %s first reading: %d of %zu doors hold something, %d unreadable",
                Side(), firstNonEmpty, g_doors.size(), firstUnread);
}

// A game-thread body run from the walker's thread, bounded so a stalled game thread ends the
// walker instead of hanging it.
template <class Fn>
int RunGT(Fn&& body) {
    auto done = std::make_shared<std::atomic<int>>(0);
    GT::Post([done, body]() mutable { body(*done); });
    for (int waited = 0; done->load() == 0; waited += 5) {
        if (waited >= 4000) return 0;
        ::Sleep(5);
    }
    return done->load();
}

// The door this peer's navmesh reaches by the shortest walk, standing at one of its two approach
// points (in front of the leaf or behind it): the route has to end at that point, since a door
// behind a wall has a route that stops short or winds far around. Says why when there is none.
bool PickReachableDoor(void* player, const ue_wrap::FVector& at, DR::DirectorGoal& goal,
                       std::wstring& nameOut, std::vector<ue_wrap::FVector>& routeOut, float& lenOut) {
    struct Cand { const Door* door; ue_wrap::FVector pos; float dist; };
    std::vector<Cand> cands;
    for (const Door& d : g_doors) {
        if (!R::IsLiveByIndex(d.actor, d.idx)) continue;
        ue_wrap::FVector p{};
        if (!E::TryGetActorLocation(d.actor, p)) continue;
        cands.push_back({&d, p, HorizDist(p, at)});
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.dist < b.dist; });
    if (cands.size() > kCandidates) cands.resize(kCandidates);
    float best = 1e30f;
    int noRoute = 0, endsFar = 0;
    for (const Cand& c : cands) {
        const ue_wrap::FVector fwd = E::GetActorForwardVector(c.door->actor);
        for (const float side : {1.f, -1.f}) {
            const ue_wrap::FVector target{c.pos.X + fwd.X * kApproachCm * side,
                                          c.pos.Y + fwd.Y * kApproachCm * side, c.pos.Z};
            std::vector<ue_wrap::FVector> path;
            if (!E::FindNavPath(player, at, target, path) || path.empty()) { ++noRoute; continue; }
            if (HorizDist(path.back(), target) > kRouteEndCm) { ++endsFar; continue; }
            float len = 0.f;
            for (size_t i = 1; i < path.size(); ++i) len += HorizDist(path[i - 1], path[i]);
            if (len >= best) continue;
            best = len;
            goal.targetActor = c.door->actor;
            goal.targetPos = target;
            nameOut = c.door->name;
            routeOut = path;
            lenOut = len;
        }
    }
    if (!goal.targetActor)
        UE_LOGW("[DOOR-DRILL] client at (%.0f,%.0f,%.0f): %zu nearest doors, the nearest %.0fcm off; "
                "%d approach points had no route, %d a route ending over %.0fcm short",
                at.X, at.Y, at.Z, cands.size(), cands.empty() ? -1.f : cands.front().dist, noRoute,
                endsFar, kRouteEndCm);
    return goal.targetActor != nullptr;
}

float DistToGoal(const ue_wrap::FVector& target) {
    auto dist = std::make_shared<float>(-1.f);
    RunGT([dist, target](std::atomic<int>& done) {
        void* p = coop::players::Registry::Get().Local();
        ue_wrap::FVector at{};
        if (p && R::IsLive(p) && E::TryGetActorLocation(p, at)) *dist = HorizDist(at, target);
        done.store(1);
    });
    return *dist;
}

// This copy's intent: open or opening. -1 when the read failed.
int ReadOpenIntent(void* door) {
    auto open = std::make_shared<int>(-1);
    RunGT([door, open](std::atomic<int>& done) {
        bool o = false;
        if (D::TryReadOpenIntent(door, o)) *open = o ? 1 : 0;
        done.store(1);
    });
    return *open;
}

// Waits until this copy reads `want`, up to `boundMs`; the milliseconds it took, or -1 at the bound.
int WaitForOpen(void* door, int want, int boundMs) {
    for (int waited = 0; waited <= boundMs; waited += 100) {
        if (ReadOpenIntent(door) == want) return waited;
        ::Sleep(100);
    }
    return -1;
}

// The point this far back along the route from its end, out of the door's sensor on the ground the
// route already walked; the start when the route is shorter.
ue_wrap::FVector PointBackAlong(const std::vector<ue_wrap::FVector>& route, float backCm) {
    float walked = 0.f;
    for (size_t i = route.size() - 1; i > 0; --i) {
        walked += HorizDist(route[i], route[i - 1]);
        if (walked >= backCm) return route[i - 1];
    }
    return route.front();
}

// A walk's budget from its own length at a slow walk, so a far door is not a timeout and a hang is.
int WalkSeconds(float routeCm) {
    return std::clamp(static_cast<int>(routeCm / 100.f) + 60, 90, 900);
}

DWORD WINAPI WalkerThread(LPVOID) {
    struct Pick {
        ue_wrap::FVector start{};
        std::wstring door;
        std::vector<ue_wrap::FVector> route;
        float len = 0.f;
    };
    auto pick = std::make_shared<Pick>();
    auto toDoor = std::make_shared<DR::DirectorGoal>();
    toDoor->reachCm = kReachCm;
    const int picked = RunGT([pick, toDoor](std::atomic<int>& done) {
        void* p = coop::players::Registry::Get().Local();
        if (!p || !R::IsLive(p) || !E::GetController(p) || !E::TryGetActorLocation(p, pick->start)) {
            done.store(2);
            return;
        }
        done.store(PickReachableDoor(p, pick->start, *toDoor, pick->door, pick->route, pick->len) ? 1 : 2);
    });
    if (picked != 1) {
        UE_LOGW("[DOOR-DRILL] client: no door this navmesh reaches from here -- INCONCLUSIVE");
        UE_LOGI("[DOOR-DRILL] client DONE at=0 opened=0 inside=0 away=0 closedMs=-1 hitOpenedAt=-1");
        return 0;
    }
    UE_LOGI("[DOOR-DRILL] client walks to door=%ls at (%.0f,%.0f,%.0f), a %.0fcm route, %d s budget",
            pick->door.c_str(), toDoor->targetPos.X, toDoor->targetPos.Y, toDoor->targetPos.Z,
            pick->len, WalkSeconds(pick->len));
    DR::ControlManager toMgr;
    DR::AddWalkToProcesses(toMgr, *toDoor);
    const bool at = toMgr.Run(*toDoor, WalkSeconds(pick->len)) && toDoor->reached;
    UE_LOGI("[DOOR-DRILL] client AT the approach of door=%ls: reached=%d, %.0fcm off", pick->door.c_str(),
            at ? 1 : 0, DistToGoal(toDoor->targetPos));

    // PRESS, then PRESENCE: wait until this copy opens, then stand in the door's sensor, the box its
    // autoclose counts players in. The box is not the doorway -- a player in the middle of an open
    // doorway can be outside it -- so the stand is at the box's centre, read from this copy. The press
    // is the door's own verb, dispatched here as a player's E dispatches it, so the script gate is
    // what turns it into the host's.
    void* const door = toDoor->targetActor;
    struct Box { bool ok = false; ue_wrap::FVector centre{}, half{}, origin{}, fwd{}; };
    auto box = std::make_shared<Box>();
    RunGT([door, box](std::atomic<int>& done) {
        box->ok = D::ReadSensorBox(door, box->centre, box->half) && E::TryGetActorLocation(door, box->origin);
        box->fwd = E::GetActorForwardVector(door);
        done.store(1);
    });
    if (box->ok) {
        const float dx = box->centre.X - box->origin.X, dy = box->centre.Y - box->origin.Y;
        UE_LOGI("[DOOR-DRILL] client SENSOR of door=%ls: centre %.0fcm along the door's forward and %.0fcm "
                "across, half-extent (%.0f,%.0f,%.0f); the approach point is %.0fcm along", pick->door.c_str(),
                dx * box->fwd.X + dy * box->fwd.Y, dx * -box->fwd.Y + dy * box->fwd.X, box->half.X,
                box->half.Y, box->half.Z, (toDoor->targetPos.X - box->origin.X) * box->fwd.X +
                (toDoor->targetPos.Y - box->origin.Y) * box->fwd.Y);
    } else {
        UE_LOGW("[DOOR-DRILL] client SENSOR of door=%ls did not read -- PRESENCE stands at the door's origin",
                pick->door.c_str());
    }
    const int openBefore = ReadOpenIntent(door);
    const int pressed = RunGT([door](std::atomic<int>& done) {
        void* p = coop::players::Registry::Get().Local();
        done.store(p && D::CallPress(door, p, D::kUseAction) ? 1 : 2);
    });
    const int openedMs = pressed == 1 ? WaitForOpen(door, 1, kOpenWaitMs) : -1;
    const bool opened = openedMs >= 0;
    UE_LOGI("[DOOR-DRILL] client PRESS door=%ls: open before=%d, dispatched=%d, this copy opening=%d "
            "after %d ms", pick->door.c_str(), openBefore, pressed == 1 ? 1 : 0, opened ? 1 : 0, openedMs);
    auto inDoor = std::make_shared<DR::DirectorGoal>();
    inDoor->targetPos = box->ok ? ue_wrap::FVector{box->centre.X, box->centre.Y, box->origin.Z} : box->origin;
    inDoor->reachCm = kInSensorCm;
    inDoor->straight = true;  // a step in plain view, before the door's own five seconds run out
    DR::ControlManager inMgr;
    DR::AddWalkToProcesses(inMgr, *inDoor);
    const bool inside = opened && inMgr.Run(*inDoor, WalkSeconds(kAwayCm)) && inDoor->reached;
    UE_LOGI("[DOOR-DRILL] client IN the sensor of door=%ls: reached=%d, %.0fcm from its centre; staying %lu ms",
            pick->door.c_str(), inside ? 1 : 0, DistToGoal(inDoor->targetPos), kDwellMs);
    ::Sleep(kDwellMs);

    auto back = std::make_shared<DR::DirectorGoal>();
    back->targetPos = PointBackAlong(pick->route, kAwayCm);
    back->reachCm = kReachCm;
    DR::ControlManager backMgr;
    DR::AddWalkToProcesses(backMgr, *back);
    const bool away = backMgr.Run(*back, WalkSeconds(kAwayCm)) && back->reached;
    UE_LOGI("[DOOR-DRILL] client AWAY from door=%ls: reached=%d, %.0fcm from the door", pick->door.c_str(),
            away ? 1 : 0, DistToGoal(toDoor->targetPos));
    // CLOSE: 0 ms means the copy had already closed by the time the walk ended.
    const int closedMs = WaitForOpen(door, 0, kCloseWaitMs);
    UE_LOGI("[DOOR-DRILL] client CLOSE door=%ls: this copy closed=%d, %d ms after the walk away ended",
            pick->door.c_str(), closedMs >= 0 ? 1 : 0, closedMs);

    // HIT: back at the approach point, a weapon's hits until the host's pry opens the door. This copy
    // must not move under its own hits: each one is refused here and run on the host.
    auto again = std::make_shared<DR::DirectorGoal>();
    again->targetPos = toDoor->targetPos;
    again->reachCm = kReachCm;
    DR::ControlManager againMgr;
    DR::AddWalkToProcesses(againMgr, *again);
    const bool back2 = againMgr.Run(*again, WalkSeconds(kAwayCm)) && again->reached;
    int hitOpenedAt = -1;
    if (back2 && closedMs >= 0) {
        for (int hit = 1; hit <= kHitMax && hitOpenedAt < 0; ++hit) {
            RunGT([door](std::atomic<int>& done) {
                void* p = coop::players::Registry::Get().Local();
                done.store(p && D::CallHit(door, p, kHitDamage) ? 1 : 2);
            });
            if (WaitForOpen(door, 1, static_cast<int>(kHitEveryMs)) >= 0) hitOpenedAt = hit;
        }
    }
    UE_LOGI("[DOOR-DRILL] client HIT door=%ls: back at the approach=%d, this copy opened at hit %d "
            "(damage %.0f each, %d at most)", pick->door.c_str(), back2 ? 1 : 0, hitOpenedAt,
            kHitDamage, kHitMax);
    UE_LOGI("[DOOR-DRILL] client DONE at=%d opened=%d inside=%d away=%d closedMs=%d hitOpenedAt=%d",
            at ? 1 : 0, opened ? 1 : 0, inside ? 1 : 0, away ? 1 : 0, closedMs, hitOpenedAt);
    return 0;
}

}  // namespace

bool IsEnabled() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::door_drill);
    return s;
}

void Tick(coop::net::Session* session) {
    if (!IsEnabled() || !session || !session->connected() || !RoleIsReady(*session)) return;
    if (!D::EnsureResolved()) return;
    const auto now = std::chrono::steady_clock::now();
    if (now < g_nextSample) return;
    g_nextSample = now + kSampleEvery;
    if (!g_listed) ListDoors();
    Sample();
    if (!coop::roster::LocalIsHost() && !g_walkerStarted.exchange(true)) {
        if (HANDLE h = ::CreateThread(nullptr, 0, &WalkerThread, nullptr, 0, nullptr))
            ::CloseHandle(h);
    }
}

void OnDisconnect() {
    if (!IsEnabled()) return;
    if (g_listed)
        UE_LOGI("[DOOR-DRILL] %s summary: a puppet entered a door's sensor %d time(s) in this world",
                Side(), g_puppetEntries);
    g_doors.clear();
    g_listed = false;
    g_puppetEntries = 0;
}

}  // namespace coop::dev::door_drill
