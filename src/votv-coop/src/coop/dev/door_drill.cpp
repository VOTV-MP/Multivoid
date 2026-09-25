// coop/dev/door_drill.cpp -- see coop/dev/door_drill.h.

#include "coop/dev/door_drill.h"

#include "coop/config/config.h"
#include "coop/dev/director/director.h"
#include "coop/interactables/door_verb_intent.h"  // SentCount: an aimed press went to the host
#include "coop/net/session.h"
#include "coop/player/hand_item.h"        // the local hand, for the crowbar the hit phase takes
#include "coop/player/players_registry.h"
#include "coop/player/roster.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/actors/inventory.h"          // the hold slot the crowbar is written to
#include "ue_wrap/devices/door.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"  // the game's updateHold
#include "ue_wrap/world/weapon_catalog.h"      // what a crowbar's swing deals

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
namespace sg = ue_wrap::script_gate;

constexpr auto  kSampleEvery = std::chrono::milliseconds(250);
constexpr float kReachCm     = 60.f;    // at the approach point, not merely near it
constexpr float kApproachCm  = 90.f;    // in front of or behind the leaf: a closed door's own origin
                                        // sits in its nav modifier, where no route ends
constexpr float kRouteEndCm  = 100.f;   // a route that ends farther from its point never gets there
constexpr int   kCandidates  = 12;      // the nearest doors by straight distance; each costs two routes
constexpr float kAwayCm      = 1500.f;  // the walk away: this far back along the route, out of any sensor
constexpr float kInSensorCm  = 30.f;    // at the sensor box's centre: where the autoclose counts a player
constexpr float kOutsideCm   = 170.f;   // straight out from the door: past the sensor's half-depth, a capsule and room
constexpr float kClearCm     = 60.f;    // a start this far past the sensor box's larger half-extent: a capsule and room
constexpr float kSameFloorCm = 120.f;   // a route point this near the door's height stands on the door's floor
constexpr int   kOpenWaitMs  = 5000;    // the host's answer and the swing's start; a door that has not
                                        // started opening by then was refused
// A stay ends on the door's own next check, five seconds after the swing ends: re-armed (its list held
// something, or the grid is off: an empty list closes the door only under gamemode's usesp_light) or
// the door closing; a stay with neither by this bound says so, and the list itself is read then.
constexpr int   kCheckWaitMs = 12000;
// After the walk away the host's door closes at its first five-second check that finds the sensor
// empty, then this copy follows its broadcast. Three checks' worth is the bound: a door still open
// by then was not closed by its autoclose.
constexpr int   kCloseWaitMs = 15000;
// The HIT phase: a held weapon's damage, a hit a player's swing apart, until the host's pry opens
// the door or the hits run out. A hit moves the leaves toward open at an interpolation speed of
// (damage / 25)^1.5 * 5 over a 0.01 s step, and the door opens once the right leaf is more than 90
// from its open offset of 70 (door_C::addDamage): a 10-damage swing (the crowbar, a
// mop, a broom; a player with nothing held does not swing) needs about 27 hits, 50 needs three.
// The host's walker hits at 50; a client's hits are bounded by its held item's swing, so it swings a
// crowbar at the most a crowbar's swing deals.
constexpr float kHitDamage   = 50.f;
constexpr int   kHitMax      = 40;
constexpr DWORD kHitEveryMs  = 700;
constexpr int   kReadMax     = 16;

const char* Side() { return coop::roster::LocalIsHost() ? "host" : "client"; }
const std::string& OnlyDoor();  // the one door the walker may walk to, when set; below

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
    if (coop::roster::LocalIsHost()) return s.AnyWorldReadyPeer();
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

// The door this peer's navmesh reaches by the shortest walk, standing at one of its two approach
// points (in front of the leaf or behind it): the route has to end at that point, since a door
// behind a wall has a route that stops short or winds far around. Says why when there is none. A
// door named by door_drill_door is walked to even when its route ends short: from far off the
// route search stops at its node limit, and the director's re-path finishes it on the way.
bool PickReachableDoor(void* player, const ue_wrap::FVector& at, DR::DirectorGoal& goal,
                       std::wstring& nameOut, std::vector<ue_wrap::FVector>& routeOut, float& lenOut) {
    struct Cand { const Door* door; ue_wrap::FVector pos; float dist; };
    std::vector<Cand> cands;
    const std::wstring only(OnlyDoor().begin(), OnlyDoor().end());
    for (const Door& d : g_doors) {
        if (!R::IsLiveByIndex(d.actor, d.idx)) continue;
        if (!only.empty() && d.name != only) continue;
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
            if (HorizDist(path.back(), target) > kRouteEndCm) {
                ++endsFar;
                if (only.empty()) continue;
            }
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
        UE_LOGW("[DOOR-DRILL] %s at (%.0f,%.0f,%.0f): %zu nearest doors, the nearest %.0fcm off; "
                "%d approach points had no route, %d a route ending over %.0fcm short", Side(),
                at.X, at.Y, at.Z, cands.size(), cands.empty() ? -1.f : cands.front().dist, noRoute,
                endsFar, kRouteEndCm);
    return goal.targetActor != nullptr;
}

// The local player's location, or the origin when it could not be read.
ue_wrap::FVector LocalAt() {
    auto at = std::make_shared<ue_wrap::FVector>();
    GT::RunAndWait([at](std::atomic<int>& done) {
        void* p = coop::players::Registry::Get().Local();
        if (p && R::IsLive(p)) E::TryGetActorLocation(p, *at);
        done.store(1);
    });
    return *at;
}

float DistToGoal(const ue_wrap::FVector& target) {
    auto dist = std::make_shared<float>(-1.f);
    GT::RunAndWait([dist, target](std::atomic<int>& done) {
        void* p = coop::players::Registry::Get().Local();
        ue_wrap::FVector at{};
        if (p && R::IsLive(p) && E::TryGetActorLocation(p, at)) *dist = HorizDist(at, target);
        done.store(1);
    });
    return *dist;
}

// This copy's open state: the swing's intent, or with `settled` the flag the swing's end sets. -1 when
// the read failed.
int ReadOpen(void* door, bool settled) {
    auto open = std::make_shared<int>(-1);
    GT::RunAndWait([door, open, settled](std::atomic<int>& done) {
        bool o = false;
        if (settled ? D::TryReadOpen(door, o) : D::TryReadOpenIntent(door, o)) *open = o ? 1 : 0;
        done.store(1);
    });
    return *open;
}
int ReadOpenIntent(void* door) { return ReadOpen(door, false); }

// Whether the local hand holds an item of class `cls` (nullptr: holds nothing), within `boundMs`.
bool WaitForHand(const wchar_t* cls, int boundMs) {
    for (int waited = 0; waited <= boundMs; waited += 100) {
        const int held = GT::RunAndWait([cls](std::atomic<int>& done) {
            void* a = coop::hand_item::LocalHandActor();
            done.store(cls ? (a && R::ClassNameOf(a) == cls ? 1 : 2) : (a ? 2 : 1));
        });
        if (held == 1) return true;
        ::Sleep(100);
    }
    return false;
}

// Whether the local hold slot, what TakeIntoHand writes over, holds nothing.
bool HoldSlotEmpty() {
    return GT::RunAndWait([](std::atomic<int>& done) {
        done.store(ue_wrap::inventory::ReadHeldItemName().empty() ? 1 : 2);
    }) == 1;
}

// The local hand takes `item` of class `cls` (list_props' row name and its class), or empties with
// "None" and no class: the hold slot written, then the game's own updateHold.
bool TakeIntoHand(const wchar_t* item, const wchar_t* cls) {
    return GT::RunAndWait([item, cls](std::atomic<int>& done) {
        void* p = coop::players::Registry::Get().Local();
        done.store(p && ue_wrap::inventory::WriteHeldItem(item, cls) && ue_wrap::engine::CallMainPlayerUpdateHold(p)
                       ? 1 : 2);
    }) == 1;
}

// Waits until this copy reads `want`, up to `boundMs`; the milliseconds it took, or -1 at the bound.
int WaitForOpen(void* door, int want, int boundMs, bool settled = false) {
    for (int waited = 0; waited <= boundMs; waited += 100) {
        if (ReadOpen(door, settled) == want) return waited;
        ::Sleep(100);
    }
    return -1;
}

// Whether this peer's own sensor list for `door` holds its own player: 1 or 0, -1 when unread.
int WalkerListed(void* door) {
    auto listed = std::make_shared<int>(-1);
    GT::RunAndWait([door, listed](std::atomic<int>& done) {
        void* held[kReadMax];
        const int n = D::ReadSensorOverlaps(door, held, kReadMax);
        void* const me = coop::players::Registry::Get().Local();
        if (n >= 0) {
            *listed = 0;
            for (int i = 0; i < n && i < kReadMax; ++i)
                if (held[i] == me) *listed = 1;
        }
        done.store(1);
    });
    return *listed;
}

// The door's own sensor checks, counted on the drill's door: checkSensor runs when a swing ends open and
// again at each five-second check that finds its list holding something.
std::atomic<void*> g_checkDoor{nullptr};
std::atomic<int>   g_checks{0};
constexpr int kTagCheck = 0x44444353;  // 'DDCS'
bool g_checkWatched = false;
void OnCheckSensorPost(const sg::Call& call) {
    if (call.object && call.object == g_checkDoor.load(std::memory_order_relaxed))
        g_checks.fetch_add(1, std::memory_order_relaxed);
}

// The sensor's own two events on the drill's door, read twice. Their stubs are watched by name; the
// handlers are also read where the stubs lead, the door's ubergraph at the two entry points the
// stubs call (door_C: 7956 the begin, 8242 the end), with the event's arguments in its frame. The
// begin handler adds the actor once per component that begins; the end handler removes every
// entry of the actor. Each line names the component, its actor and the list the handler left.
struct SensorEvent { const wchar_t* name; int tag; const char* what; };
constexpr SensorEvent kSensorEvents[] = {
    { L"BndEvt__door_sensor_ComponentBoundEvent_2_ComponentBeginOverlapSignature__DelegateSignature",
      0x44444242 /*'DDBB'*/, "BEGIN" },
    { L"BndEvt__door_sensor_ComponentBoundEvent_3_ComponentEndOverlapSignature__DelegateSignature",
      0x44444245 /*'DDBE'*/, "END" },
};
constexpr int kTagUbergraph = 0x44445547;  // 'DDUG'
constexpr int32_t kBeginEntry = 7956, kEndEntry = 8242;
bool g_sensorWatched[2] = {};
bool g_ubergraphWatched = false;
std::atomic<int> g_stubCalls{0};   // the two stubs, on any door
int32_t g_otherActorOff[2] = {-2, -2}, g_otherCompOff[2] = {-2, -2};
int32_t g_entryOff = -2, g_frameActorOff[2] = {-2, -2}, g_frameCompOff[2] = {-2, -2};
std::wstring ClassNameOf(void* o) {
    void* cls = o ? R::ClassOf(o) : nullptr;
    return cls ? R::ToString(R::NameOf(cls)) : L"?";
}
void LogSensorEvent(void* door, int i, const char* via, void* actor, void* comp) {
    auto& reg = coop::players::Registry::Get();
    const char* who = !actor ? "no actor" : reg.IsLocal(actor) ? "the local player" : reg.IsPuppet(actor) ? "a puppet" : "another actor";
    void* held[kReadMax];
    const int n = D::ReadSensorOverlaps(door, held, kReadMax);
    UE_LOGI("[DOOR-DRILL] %s SENSOR-%s (%s) on the drill's door: component %ls (%ls) of %s (%ls); the list holds %d",
            Side(), kSensorEvents[i].what, via, comp ? R::ToString(R::NameOf(comp)).c_str() : L"?",
            ClassNameOf(comp).c_str(), who, ClassNameOf(actor).c_str(), n);
}
void OnSensorEventPost(const sg::Call& call) {
    g_stubCalls.fetch_add(1, std::memory_order_relaxed);
    if (!call.object || call.object != g_checkDoor.load(std::memory_order_relaxed)) return;
    if (!call.function || !call.locals) return;
    const int i = call.tag == kSensorEvents[0].tag ? 0 : 1;
    if (g_otherActorOff[i] == -2) {
        g_otherActorOff[i] = R::FindParamOffset(call.function, L"OtherActor");
        g_otherCompOff[i] = R::FindParamOffset(call.function, L"OtherComp");
    }
    void* actor = g_otherActorOff[i] >= 0 ? *reinterpret_cast<void* const*>(call.locals + g_otherActorOff[i]) : nullptr;
    void* comp = g_otherCompOff[i] >= 0 ? *reinterpret_cast<void* const*>(call.locals + g_otherCompOff[i]) : nullptr;
    LogSensorEvent(call.object, i, "stub", actor, comp);
}
void OnUbergraphPost(const sg::Call& call) {
    if (!call.object || call.object != g_checkDoor.load(std::memory_order_relaxed)) return;
    if (!call.function || !call.locals) return;
    if (g_entryOff == -2) {
        g_entryOff = R::FindParamOffset(call.function, L"EntryPoint");
        g_frameActorOff[0] = R::FindPropertyOffset(call.function, L"K2Node_ComponentBoundEvent_OtherActor_2");
        g_frameCompOff[0] = R::FindPropertyOffset(call.function, L"K2Node_ComponentBoundEvent_OtherComp_2");
        g_frameActorOff[1] = R::FindPropertyOffset(call.function, L"K2Node_ComponentBoundEvent_OtherActor_1");
        g_frameCompOff[1] = R::FindPropertyOffset(call.function, L"K2Node_ComponentBoundEvent_OtherComp_1");
        UE_LOGI("[DOOR-DRILL] %s ubergraph frame: EntryPoint@%d begin actor@%d comp@%d, end actor@%d comp@%d",
                Side(), g_entryOff, g_frameActorOff[0], g_frameCompOff[0], g_frameActorOff[1], g_frameCompOff[1]);
    }
    if (g_entryOff < 0) return;
    const int32_t entry = *reinterpret_cast<const int32_t*>(call.locals + g_entryOff);
    const int i = entry == kBeginEntry ? 0 : entry == kEndEntry ? 1 : -1;
    if (i < 0) return;
    void* actor = g_frameActorOff[i] >= 0 ? *reinterpret_cast<void* const*>(call.locals + g_frameActorOff[i]) : nullptr;
    void* comp = g_frameCompOff[i] >= 0 ? *reinterpret_cast<void* const*>(call.locals + g_frameCompOff[i]) : nullptr;
    LogSensorEvent(call.object, i, "ubergraph", actor, comp);
}

// The one door the walker may walk to, when set.
const std::string& OnlyDoor() {
    static const std::string s = coop::config::ResolveString(::coop::config_registry::rows::door_drill_door);
    return s;
}

// Which peer walks: the client, or the host for single player's reading.
bool WalkerIsHost() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::door_drill_host);
    return s;
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

// The camera turned at a part of the door through a fan of headings around it, each held until the
// player's own interaction trace answers (it runs every frame), until the trace strikes that part of
// that door: what the trace takes is what a player's E is sent to. `other` names an actor it took
// instead, the first one met.
struct Aim { bool took = false; std::wstring other; };
Aim AimAtPart(void* door, const wchar_t* part) {
    static constexpr float kYaw[] = {0.f, -5.f, 5.f, -10.f, 10.f, -16.f, 16.f};
    static constexpr float kPitch[] = {0.f, 8.f, -8.f, 16.f, -16.f};
    Aim a;
    for (float dp : kPitch) {
        for (float dy : kYaw) {
            const bool aimed = GT::RunAndWait([door, part, dp, dy](std::atomic<int>& done) {
                void* p = coop::players::Registry::Get().Local();
                void* c = p ? E::GetController(p) : nullptr;
                void* comp = D::PartOf(door, part);
                if (!c || !comp) { done.store(2); return; }
                const ue_wrap::FVector cam = E::GetCameraLocation();
                const ue_wrap::FVector at = E::GetComponentLocation(comp);
                const float dx = at.X - cam.X, dyv = at.Y - cam.Y, dz = at.Z - cam.Z;
                const float yaw = std::atan2(dyv, dx) * 57.29578f;
                const float pitch = std::atan2(dz, std::sqrt(dx * dx + dyv * dyv)) * 57.29578f;
                E::SetControlRotation(c, ue_wrap::FRotator{pitch + dp, yaw + dy, 0.f});
                done.store(1);
            }) == 1;
            if (!aimed) return a;
            for (int waited = 0; waited <= 300; waited += 50) {
                ::Sleep(50);
                auto other = std::make_shared<std::wstring>();
                const int hit = GT::RunAndWait([door, part, other](std::atomic<int>& done) {
                    void* p = coop::players::Registry::Get().Local();
                    void* actor = p ? E::ReadMainPlayerHitActor(p) : nullptr;
                    void* comp = p ? E::ReadMainPlayerHitComponent(p) : nullptr;
                    if (actor == door && comp && comp == D::PartOf(door, part)) { done.store(1); return; }
                    if (actor && actor != door) *other = R::ClassNameOf(actor);
                    done.store(2);
                });
                if (hit == 1) { a.took = true; return a; }
                if (!other->empty() && a.other.empty()) a.other = *other;
            }
        }
    }
    return a;
}

// The door verbs this client has sent the host, read on the game thread.
uint64_t SentVerbs() {
    auto n = std::make_shared<uint64_t>(0);
    GT::RunAndWait([n](std::atomic<int>& done) {
        *n = coop::door_verb_intent::SentCount();
        done.store(1);
    });
    return *n;
}

// AIMED (a client, the door shut): the player's own E, aimed through the game's trace at a leaf and
// then at the frame. The press goes through the player's use handler, which sends the selected action
// to the actor the trace hit: it reached the door's entry verb when that verb, refused on this copy,
// went to the host as a press. Whether the door moves is the door's own say (it will not close on a
// player in its sensor), so its open is reported beside the verdict, not judged.
void AimedLegs(void* door, const std::wstring& name) {
    for (const wchar_t* part : {L"door_L", L"frame"}) {
        const int before = ReadOpenIntent(door);
        const Aim a = AimAtPart(door, part);
        if (!a.took) {
            UE_LOGW("[DOOR-DRILL] client AIMED door=%ls part=%ls: no aim of the fan put the trace on it (it took %ls) "
                    "-- INCONCLUSIVE", name.c_str(), part, a.other.empty() ? L"nothing else" : a.other.c_str());
            continue;
        }
        const uint64_t sent0 = SentVerbs();
        const bool pressed = GT::RunAndWait([](std::atomic<int>& done) {
            void* p = coop::players::Registry::Get().Local();
            done.store(p && E::CallMainPlayerUseSelectedAction(p) ? 1 : 2);
        }) == 1;
        const bool went = SentVerbs() > sent0;
        WaitForOpen(door, before == 1 ? 0 : 1, 3000);
        UE_LOGI("[DOOR-DRILL] client AIMED door=%ls part=%ls: the trace took the door through it; E through "
                "useSelectedAction dispatched=%d, %s; the door's open %d -> %d on this copy", name.c_str(), part,
                pressed ? 1 : 0, went ? "the press went to the host -- PASS" : "no press went to the host -- FAIL",
                before, ReadOpenIntent(door));
    }
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
    const int picked = GT::RunAndWait([pick, toDoor](std::atomic<int>& done) {
        void* p = coop::players::Registry::Get().Local();
        if (!p || !R::IsLive(p) || !E::GetController(p) || !E::TryGetActorLocation(p, pick->start)) {
            done.store(2);
            return;
        }
        done.store(PickReachableDoor(p, pick->start, *toDoor, pick->door, pick->route, pick->len) ? 1 : 2);
    });
    if (picked != 1) {
        UE_LOGW("[DOOR-DRILL] %s: no door this navmesh reaches from here -- INCONCLUSIVE", Side());
        UE_LOGI("[DOOR-DRILL] %s DONE at=0 -- no door to walk", Side());
        return 0;
    }
    g_checkDoor.store(toDoor->targetActor, std::memory_order_relaxed);
    UE_LOGI("[DOOR-DRILL] %s walks to door=%ls at (%.0f,%.0f,%.0f), a %.0fcm route, %d s budget", Side(),
            pick->door.c_str(), toDoor->targetPos.X, toDoor->targetPos.Y, toDoor->targetPos.Z,
            pick->len, WalkSeconds(pick->len));
    DR::ControlManager toMgr;
    DR::AddWalkToProcesses(toMgr, *toDoor);
    const bool at = toMgr.Run(*toDoor, WalkSeconds(pick->len)) && toDoor->reached;
    const ue_wrap::FVector arrived = LocalAt();
    UE_LOGI("[DOOR-DRILL] %s AT the approach of door=%ls: reached=%d, %.0fcm off, %.0fcm above it", Side(),
            pick->door.c_str(), at ? 1 : 0, DistToGoal(toDoor->targetPos), arrived.Z - toDoor->targetPos.Z);
    // Reached is on the door's own level (the director's reach): a walker the route left on another
    // floor stands in no sensor of this door, and every pass would measure nothing.
    if (!at) {
        UE_LOGW("[DOOR-DRILL] %s did not reach the approach of door=%ls on its level -- INCONCLUSIVE", Side(),
                pick->door.c_str());
        UE_LOGI("[DOOR-DRILL] %s DONE at=0 -- did not reach its door", Side());
        return 0;
    }

    // The stay is at the centre of the door's sensor box, read from this copy; each stay reads whether
    // the list the autoclose counts holds the walker there.
    void* const door = toDoor->targetActor;
    struct Box { bool ok = false; ue_wrap::FVector centre{}, half{}, origin{}, fwd{}; };
    auto box = std::make_shared<Box>();
    GT::RunAndWait([door, box](std::atomic<int>& done) {
        box->ok = D::ReadSensorBox(door, box->centre, box->half) && E::TryGetActorLocation(door, box->origin);
        box->fwd = E::GetActorForwardVector(door);
        done.store(1);
    });
    if (box->ok) {
        const float dx = box->centre.X - box->origin.X, dy = box->centre.Y - box->origin.Y;
        UE_LOGI("[DOOR-DRILL] %s SENSOR of door=%ls: centre %.0fcm along the door's forward and %.0fcm "
                "across, half-extent (%.0f,%.0f,%.0f); the approach point is %.0fcm along", Side(),
                pick->door.c_str(), dx * box->fwd.X + dy * box->fwd.Y, dx * -box->fwd.Y + dy * box->fwd.X,
                box->half.X, box->half.Y, box->half.Z, (toDoor->targetPos.X - box->origin.X) * box->fwd.X +
                (toDoor->targetPos.Y - box->origin.Y) * box->fwd.Y);
    } else {
        UE_LOGW("[DOOR-DRILL] %s SENSOR of door=%ls did not read -- PRESENCE stands at the door's origin",
                Side(), pick->door.c_str());
    }
    const ue_wrap::FVector stand = box->ok ? ue_wrap::FVector{box->centre.X, box->centre.Y, box->origin.Z}
                                           : box->origin;

    // One pass: from a start -- outside the sensor, read empty of the walker, or the approach point,
    // where the walker is often already in it -- PRESS the closed door, step into the sensor at once
    // (mid-swing) or once the swing has ended, stay and read whether the walker's own list keeps it,
    // then walk AWAY and wait for the CLOSE. The press is the door's own verb, dispatched as a player's
    // E dispatches it, so on a client the script gate is what turns it into the host's.
    const float side = box->ok ? ((toDoor->targetPos.X - box->origin.X) * box->fwd.X +
                                  (toDoor->targetPos.Y - box->origin.Y) * box->fwd.Y >= 0.f ? 1.f : -1.f)
                               : 1.f;
    // A start outside the sensor on ground the walker has already crossed: back along the route that
    // brought it, the first point clear of the sensor box on the door's floor. Straight out from the door
    // is the fallback when the route never left the box; on the dish doors that point stands past a
    // railing, and a pass that cannot reach its start says so.
    const ue_wrap::FVector centre = box->ok ? box->centre : box->origin;
    const float clearCm = (box->ok ? (std::max)(box->half.X, box->half.Y) : 0.f) + kClearCm;
    ue_wrap::FVector outside{box->origin.X + box->fwd.X * kOutsideCm * side,
                             box->origin.Y + box->fwd.Y * kOutsideCm * side, box->origin.Z};
    bool outsideOnRoute = false;
    for (size_t i = toDoor->route.size(); i-- > 0 && !outsideOnRoute;) {
        const ue_wrap::FVector& p = toDoor->route[i];
        if (HorizDist(p, centre) >= clearCm && std::fabs(p.Z - toDoor->targetPos.Z) <= kSameFloorCm) {
            outside = p;
            outsideOnRoute = true;
        }
    }
    UE_LOGI("[DOOR-DRILL] %s OUTSIDE start of door=%ls: %.0fcm from the sensor's centre, %s", Side(),
            pick->door.c_str(), HorizDist(outside, centre),
            outsideOnRoute ? "on the route that brought the walker" : "straight out from the door (no route point clears the box)");
    struct Pass { bool opened = false, inside = false, away = false; int listedBefore = -1, kept = -1, closedMs = -1; };
    auto walkTo = [&](const ue_wrap::FVector& at, bool straight) {
        auto goal = std::make_shared<DR::DirectorGoal>();
        goal->targetPos = at;
        goal->reachCm = kReachCm;
        goal->straight = straight;
        DR::ControlManager mgr;
        DR::AddWalkToProcesses(mgr, *goal);
        return mgr.Run(*goal, WalkSeconds(kAwayCm)) && goal->reached;
    };
    auto runPass = [&](const char* name, bool fromOutside, bool afterSwing) {
        Pass r;
        // Back to the approach point by the navmesh first: the last pass's walk away may have left the
        // walker on another floor, which a straight line never climbs. An outside pass then walks to its
        // start, a point of the route it came by.
        if (!walkTo(toDoor->targetPos, /*straight*/ false) || (fromOutside && !walkTo(outside, /*straight*/ false))) {
            UE_LOGW("[DOOR-DRILL] %s PASS (%s) door=%ls ENDS: the walker did not reach its start on the door's "
                    "level", Side(), name, pick->door.c_str());
            return r;
        }
        // A pass starts from a closed door: an open or opening one is pressed shut first, and its swing
        // waited out; one already closing is only waited out.
        if (ReadOpenIntent(door) == 1) {
            GT::RunAndWait([door](std::atomic<int>& done) {
                void* p = coop::players::Registry::Get().Local();
                done.store(p && D::CallPress(door, p, D::kUseAction) ? 1 : 2);
            });
            const int shutMs = WaitForOpen(door, 0, kOpenWaitMs, /*settled*/ true);
            UE_LOGI("[DOOR-DRILL] %s PASS (%s) door=%ls was open: pressed shut, closed=%d after %d ms", Side(),
                    name, pick->door.c_str(), shutMs >= 0 ? 1 : 0, shutMs);
        } else if (ReadOpen(door, /*settled*/ true) == 1) {
            WaitForOpen(door, 0, kOpenWaitMs, /*settled*/ true);
        }
        r.listedBefore = WalkerListed(door);
        const int openBefore = ReadOpenIntent(door);
        const int pressed = GT::RunAndWait([door](std::atomic<int>& done) {
            void* p = coop::players::Registry::Get().Local();
            done.store(p && D::CallPress(door, p, D::kUseAction) ? 1 : 2);
        });
        const int openedMs = pressed == 1 ? WaitForOpen(door, 1, kOpenWaitMs) : -1;
        r.opened = openedMs >= 0;
        const int settledMs = (r.opened && afterSwing) ? WaitForOpen(door, 1, kOpenWaitMs, /*settled*/ true) : -1;
        UE_LOGI("[DOOR-DRILL] %s PRESS (%s) door=%ls: in its own list before the press=%d, open before=%d, "
                "dispatched=%d, opening=%d after %d ms, swing ended after %d ms", Side(), name, pick->door.c_str(),
                r.listedBefore, openBefore, pressed == 1 ? 1 : 0, r.opened ? 1 : 0, openedMs, settledMs);
        if (!r.opened) {
            UE_LOGW("[DOOR-DRILL] %s PASS (%s) door=%ls ENDS: the door did not start opening within %d ms of the "
                    "press", Side(), name, pick->door.c_str(), kOpenWaitMs);
            return r;
        }
        auto inDoor = std::make_shared<DR::DirectorGoal>();
        inDoor->targetPos = stand;
        inDoor->reachCm = kInSensorCm;
        inDoor->straight = true;  // a step in plain view, before the door's own five seconds run out
        DR::ControlManager inMgr;
        DR::AddWalkToProcesses(inMgr, *inDoor);
        r.inside = r.opened && inMgr.Run(*inDoor, WalkSeconds(kAwayCm)) && inDoor->reached;
        const ue_wrap::FVector me = LocalAt();
        UE_LOGI("[DOOR-DRILL] %s IN the sensor (%s) of door=%ls: reached=%d, %.0fcm from its centre, %.0fcm above "
                "it", Side(), name, pick->door.c_str(), r.inside ? 1 : 0, DistToGoal(inDoor->targetPos),
                box->ok ? me.Z - box->centre.Z : 0.f);
        // The stay ends on the door's own next check, counted from once the swing has ended (that end
        // calls checkSensor itself, to arm the check); whether the walker is in the list is read from
        // the list, since the check also re-arms on an empty one with the grid off.
        WaitForOpen(door, 1, kOpenWaitMs, /*settled*/ true);
        const int checks0 = g_checks.load(std::memory_order_relaxed);
        int checkMs = -1;
        bool closedByCheck = false;
        for (int waited = 0; waited <= kCheckWaitMs && checkMs < 0; waited += 100) {
            if (g_checks.load(std::memory_order_relaxed) > checks0) checkMs = waited;
            else if (ReadOpenIntent(door) == 0) { checkMs = waited; closedByCheck = true; }
            else ::Sleep(100);
        }
        r.kept = WalkerListed(door);
        // Where the sensor box is with the door open, against the closed-door reading.
        auto now = std::make_shared<Box>();
        GT::RunAndWait([door, now](std::atomic<int>& done) {
            now->ok = D::ReadSensorBox(door, now->centre, now->half);
            done.store(1);
        });
        const float mx = now->centre.X - box->centre.X, my = now->centre.Y - box->centre.Y;
        UE_LOGI("[DOOR-DRILL] %s KEPT (%s) door=%ls: the door's check %s after %d ms; in its own list then=%d; "
                "the sensor box has moved %.0fcm along and %.0fcm across since the closed-door reading",
                Side(), name, pick->door.c_str(), checkMs < 0 ? "did not run" : closedByCheck ? "closed it" : "re-armed",
                checkMs, r.kept, mx * box->fwd.X + my * box->fwd.Y, mx * -box->fwd.Y + my * box->fwd.X);
        r.away = walkTo(PointBackAlong(pick->route, kAwayCm), /*straight*/ false);
        // 0 ms means the door had already closed by the time the walk ended.
        r.closedMs = WaitForOpen(door, 0, kCloseWaitMs);
        UE_LOGI("[DOOR-DRILL] %s CLOSE (%s) door=%ls: away=%d, %.0fcm from the door; closed=%d, %d ms after the "
                "walk away ended", Side(), name, pick->door.c_str(), r.away ? 1 : 0,
                DistToGoal(toDoor->targetPos), r.closedMs >= 0 ? 1 : 0, r.closedMs);
        return r;
    };
    // Entering from outside the list, during the swing and after it, then the approach point as the
    // control where the walker is usually listed before its press.
    const Pass outSwing = runPass("outside, mid-swing", /*fromOutside*/ true, /*afterSwing*/ false);
    const Pass outSettled = outSwing.closedMs >= 0 ? runPass("outside, after the swing", true, true) : Pass{};
    const Pass inSettled = outSettled.closedMs >= 0 ? runPass("approach, after the swing", false, true) : Pass{};

    // HIT: back at the approach point, a weapon's hits until the door's pry opens it. A client's copy
    // must not move under its own verbs: each one is refused there and run on the host, which holds a
    // pry to a crowbar last held and a hit to what the held item's swing deals (door_verb_intent).
    const bool back2 = inSettled.closedMs >= 0 && walkTo(toDoor->targetPos, /*straight*/ false);
    const bool client = !coop::roster::LocalIsHost();
    auto hitOnce = [door](float damage) {
        GT::RunAndWait([door, damage](std::atomic<int>& done) {
            void* p = coop::players::Registry::Get().Local();
            done.store(p && D::CallHit(door, p, damage) ? 1 : 2);
        });
    };
    auto pryOnce = [door]() {
        GT::RunAndWait([door](std::atomic<int>& done) { done.store(D::CallCrowbarOpen(door) ? 1 : 2); });
    };
    float hitDamage = kHitDamage;
    bool armed = !client;
    // A client already holding something is no clean test of a hand with nothing that swings, and its
    // item would be written over: the unarmed and armed legs are skipped, and with them the hits.
    const bool emptyHand = back2 && client && HoldSlotEmpty();
    if (back2 && client && !emptyHand)
        UE_LOGW("[DOOR-DRILL] client door=%ls: the hand holds an item already -- the pry and hit checks are "
                "INCONCLUSIVE", pick->door.c_str());
    if (back2 && client && emptyHand) {
        pryOnce();
        hitOnce(kHitDamage);
        const bool stayed = WaitForOpen(door, 1, 1500) < 0;
        UE_LOGI("[DOOR-DRILL] client UNARMED door=%ls: a pry and a hit of %.0f with nothing held, the door %s",
                pick->door.c_str(), kHitDamage, stayed ? "stayed shut, as it should" : "OPENED -- FAIL");
        ue_wrap::weapon_catalog::Swing swing;
        armed = TakeIntoHand(L"crowbar", L"prop_crowbar_C") && WaitForHand(L"prop_crowbar_C", 3000) &&
                GT::RunAndWait([&swing](std::atomic<int>& done) {
                    done.store(ue_wrap::weapon_catalog::Lookup(L"crowbar", swing) && swing.canSwing ? 1 : 2);
                }) == 1;
        if (armed) hitDamage = swing.maxDamage;
        UE_LOGI("[DOOR-DRILL] client ARMED=%d door=%ls: a crowbar in hand, a swing of at most %.1f; one hit of "
                "%.1f the host must cut", armed ? 1 : 0, pick->door.c_str(), hitDamage, hitDamage * 10.f);
        if (armed) hitOnce(hitDamage * 10.f);
    }
    int hitOpenedAt = -1;
    if (back2 && armed) {
        for (int hit = 1; hit <= kHitMax && hitOpenedAt < 0; ++hit) {
            hitOnce(hitDamage);
            if (WaitForOpen(door, 1, static_cast<int>(kHitEveryMs)) >= 0) hitOpenedAt = hit;
        }
    }
    UE_LOGI("[DOOR-DRILL] %s HIT door=%ls: back at the approach=%d, opened at hit %d (damage %.1f each, %d at "
            "most)", Side(), pick->door.c_str(), back2 ? 1 : 0, hitOpenedAt, hitDamage, kHitMax);
    if (back2 && client && armed && emptyHand) {
        // The crowbar goes from the hand, as a pry takes it: the hand empty, a crowbar the last it held.
        const bool gone = TakeIntoHand(L"None", nullptr) && WaitForHand(nullptr, 3000);
        pryOnce();
        UE_LOGI("[DOOR-DRILL] client PRY door=%ls: the crowbar gone from the hand=%d, a pry sent; the host runs it",
                pick->door.c_str(), gone ? 1 : 0);
    }
    // The aimed legs want the door shut: away until it closes, then back to the approach.
    if (client && back2) {
        const bool shut = walkTo(PointBackAlong(pick->route, kAwayCm), /*straight*/ false) &&
                          WaitForOpen(door, 0, kCloseWaitMs) >= 0 && walkTo(toDoor->targetPos, /*straight*/ false);
        if (shut) AimedLegs(door, pick->door);
        else UE_LOGW("[DOOR-DRILL] client AIMED door=%ls: the door did not shut behind the walk away -- INCONCLUSIVE",
                     pick->door.c_str());
    }
    UE_LOGI("[DOOR-DRILL] %s sensor stubs reached the gate %d time(s), on any door", Side(),
            g_stubCalls.load(std::memory_order_relaxed));
    UE_LOGI("[DOOR-DRILL] %s DONE at=%d outsideMidSwing(before=%d kept=%d closedMs=%d) "
            "outsideAfterSwing(before=%d kept=%d closedMs=%d) approachAfterSwing(before=%d kept=%d closedMs=%d) "
            "hitOpenedAt=%d", Side(), at ? 1 : 0, outSwing.listedBefore, outSwing.kept, outSwing.closedMs,
            outSettled.listedBefore, outSettled.kept, outSettled.closedMs, inSettled.listedBefore, inSettled.kept,
            inSettled.closedMs, hitOpenedAt);
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
    if (!g_checkWatched) g_checkWatched = sg::WatchName(L"checkSensor", kTagCheck, nullptr, &OnCheckSensorPost);
    for (int i = 0; i < 2; ++i)
        if (!g_sensorWatched[i])
            g_sensorWatched[i] = sg::WatchName(kSensorEvents[i].name, kSensorEvents[i].tag, nullptr, &OnSensorEventPost);
    if (!g_ubergraphWatched)
        g_ubergraphWatched = sg::WatchName(L"ExecuteUbergraph_door", kTagUbergraph, nullptr, &OnUbergraphPost);
    sg::ResolvePendingNames();
    const auto now = std::chrono::steady_clock::now();
    if (now < g_nextSample) return;
    g_nextSample = now + kSampleEvery;
    if (!g_listed) ListDoors();
    Sample();
    if (coop::roster::LocalIsHost() == WalkerIsHost() && !g_walkerStarted.exchange(true)) {
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
