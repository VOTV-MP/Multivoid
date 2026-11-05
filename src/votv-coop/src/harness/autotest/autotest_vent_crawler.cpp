// harness/autotest/autotest_vent_crawler.cpp -- TEST-ONLY: the day 5 vent crawler event, host and client side by side.
//
// The host fires `ventCrawler` through the eventer as the scheduler does. The client meets the host, then has the
// crawler's mirror; its player walks into the crawler's box with the director, which starts the crawl on the host (the
// host's own player walks in, if that one does not). The crawl closes the signal room door (opened first) and takes
// its power, plays its steps, breaks and swings the hanging vent down, gives the power back, and at its end opens the
// balcony door's keypads and the door, and the crawler goes. The client's door closes and loses its power, and the
// crawl's sounds play there; each peer then reads the two doors, the keypads and the vent, and the client's must be the
// host's. The client begins once its join's catch-up window has closed. Then life 1 leaves, and the rejoined life
// (VOTVCOOP_CRAWLER_REJOIN=1, mp.py smoke --rejoin --rejoin-env) reads the same row against the host's, the crawl being
// over. The host's verdict counts its own fails, the client's and the rejoined life's. With VOTVCOOP_CRAWLER_HOST_FIRST=1
// on both peers the host's player walks into the box before any client has joined, and the client that joins after the
// crawl must be told of it (a state heard) and find the vent and doors as the host's are. Gated by
// VOTVCOOP_RUN_VENT_CRAWLER; the folder is VOTVCOOP_CAN_DIR.

#include "harness/autotest.h"
#include "harness/autotest/meet.h"
#include "harness/autotest/peer_signal.h"
#include "harness/session_runtime.h"

#include "coop/config/config.h"

#include "coop/dev/director/director.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/props/join_membership_sweep.h"
#include "coop/session/host_world_settle.h"
#include "coop/session/net_pump.h"
#include "coop/world/event_fire_sync.h"
#include "coop/world/vent_crawler_sync.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/devices/door.h"
#include "ue_wrap/devices/passwordlock.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_component.h"
#include "ue_wrap/world/keyed_objects.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace harness::autotest {
namespace {

namespace D = ue_wrap::door;
namespace DIR = coop::director;
namespace E = ue_wrap::engine;
namespace PL = ue_wrap::passwordlock;
namespace R = ue_wrap::reflection;

namespace PS = harness::autotest::peer_signal;
using PS::Await;
using PS::AwaitText;
using PS::Mark;

int g_fails = 0;

template <class Fn>
void OnGameThread(Fn&& fn) {
    ue_wrap::game_thread::RunAndWait([&fn](std::atomic<int>& d) { fn(); d.store(1); });
}

// A peer's fails count as it left it in its done signal; text that is no count is a fail.
int ParseFails(const std::string& text) {
    char* end = nullptr;
    const long n = std::strtol(text.c_str(), &end, 10);
    return end != text.c_str() && n >= 0 ? static_cast<int>(n) : 1;
}

void Check(bool ok, const char* what) {
    UE_LOGI("crawler-drill: %hs %hs", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fails;
}

bool WaitFor(const std::function<bool()>& ok, int seconds) {
    for (int i = 0; i < seconds * 10; ++i) {
        bool v = false;
        OnGameThread([&] { v = ok(); });
        if (v) return true;
        ::Sleep(100);
    }
    return false;
}

std::vector<std::string> Split(const std::string& s, char sep = '|') {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string one;
    while (std::getline(ss, one, sep)) out.push_back(one);
    return out;
}

void* Me() { return coop::players::Registry::Get().Local(); }

bool WorldReady() {
    bool ready = false;
    for (int i = 0; i < 480 && !ready; ++i) {
        OnGameThread([&] { ready = Me() && coop::net_pump::HasAnnouncedWorldReady(); });
        if (!ready) ::Sleep(500);
    }
    return ready;
}

// The box's reach: its smaller scaled half-extent in the plane, so a walker within it stands inside the box whatever
// way the box is turned. Zero when the box or its call does not resolve. Game thread.
float BoxReach(void* box) {
    void* fn = box ? R::FindDispatchFunctionCached(R::ClassOf(box), L"GetScaledBoxExtent") : nullptr;
    ue_wrap::ParamFrame f(fn);
    if (!fn || !f.valid() || !ue_wrap::Call(box, f)) return 0.f;
    const ue_wrap::FVector e = f.Get<ue_wrap::FVector>(L"ReturnValue");
    return (std::min)(e.X, e.Y);
}

constexpr float kPaceCmPerSec = 300.f;   // a walk's pace, as harness/autotest/meet.cpp budgets it

// Walks the local player into the crawler's box ("x|y|z|reach") with the director: a navmesh route walked through
// AddMovementInput, never a teleport. Called from the drill's thread. False, with the reason logged, when the walk
// could not be made or did not arrive.
bool WalkInto(const std::string& box) {
    const auto b = Split(box);
    if (b.size() != 4) {
        UE_LOGW("crawler-drill: no box to walk into ('%hs')", box.c_str());
        return false;
    }
    DIR::DirectorGoal goal;
    goal.targetPos = {static_cast<float>(std::atof(b[0].c_str())), static_cast<float>(std::atof(b[1].c_str())),
                      static_cast<float>(std::atof(b[2].c_str()))};
    goal.reachCm = static_cast<float>(std::atof(b[3].c_str()));
    float line = 0.f;
    bool placed = false;
    OnGameThread([&] {
        ue_wrap::FVector mine{};
        if (!Me() || !E::TryGetActorLocation(Me(), mine)) return;
        const float dx = mine.X - goal.targetPos.X, dy = mine.Y - goal.targetPos.Y, dz = mine.Z - goal.targetPos.Z;
        line = std::sqrt(dx * dx + dy * dy + dz * dz);
        goal.epoch = DIR::WalkEpoch();
        placed = true;
    });
    if (!placed) {
        UE_LOGW("crawler-drill: the local player's place is not readable to walk from");
        return false;
    }
    const int budget = static_cast<int>(line / kPaceCmPerSec * 2.f) + 30;
    UE_LOGI("crawler-drill: walking into the box, %.0fcm away, reach %.0fcm, %d s budget", line, goal.reachCm, budget);
    DIR::ControlManager mgr;
    DIR::AddWalkToProcesses(mgr, goal);
    mgr.Run(goal, budget);
    if (!goal.reached) {
        UE_LOGW("crawler-drill: could not walk into the box (%hs)", goal.failReason);
        return false;
    }
    return true;
}

// The world is up and the host's puppet exists here.
bool HostPuppetUp() {
    WorldReady();
    return PS::WaitFor("the host's puppet", 60, [&] {
        bool have = false;
        OnGameThread([&] {
            coop::RemotePlayer* p = coop::players::Registry::Get().Puppet(0);
            have = p && p->valid();
        });
        return have;
    });
}

// The live crawler, or null; the class's default object is not one.
void* Crawler() {
    void* cls = ue_wrap::object_index::ClassByName(L"ventCrawler_C");
    void* found = nullptr;
    if (cls)
        ue_wrap::object_index::ForEachInstance(cls, [](void* ctx, void* obj, int32_t) {
            if (R::IsLive(obj) && R::ToString(R::NameOf(obj)).rfind(L"Default__", 0) != 0) *static_cast<void**>(ctx) = obj;
        }, &found);
    return found;
}

void* ObjField(void* obj, const wchar_t* name) {
    const int32_t off = obj ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    if (off < 0) return nullptr;
    void* v = nullptr;
    std::memcpy(&v, static_cast<const uint8_t*>(obj) + off, sizeof(v));
    return v;
}

// The door's passLocks, as its own TArray holds them.
std::vector<void*> PassLocks(void* door) {
    std::vector<void*> out;
    const int32_t off = door ? R::FindPropertyOffset(R::ClassOf(door), L"passLocks") : -1;
    if (off < 0) return out;
    struct Arr { void** data; int32_t num, max; } a{};
    std::memcpy(&a, static_cast<const uint8_t*>(door) + off, sizeof(a));
    for (int32_t i = 0; a.data && i < a.num && i < 16; ++i) out.push_back(a.data[i]);
    return out;
}

std::string DoorRow(const wchar_t* key) {
    void* door = ue_wrap::keyed_objects::Resolve(key);
    bool open = false, active = false;
    if (!door || !D::TryReadOpen(door, open) || !D::TryReadActive(door, active)) return "?,?";
    return std::string(open ? "open" : "shut") + "," + (active ? "on" : "off");
}

// The two doors, the balcony door's keypads and the hanging vent, as this peer has them.
std::string StateRow() {
    std::string row = DoorRow(L"basedoor_signalroom") + "|" + DoorRow(L"basedoor_balcony") + "|";
    void* balcony = ue_wrap::keyed_objects::Resolve(L"basedoor_balcony");
    PL::EnsureResolved();
    for (void* lock : PassLocks(balcony)) {
        PL::State st;
        row += (lock && PL::ReadState(lock, st)) ? (st.active ? "1" : "0") : "?";
    }
    row += "|";
    void* vent = ue_wrap::keyed_objects::Resolve(L"ventHanging");
    ue_wrap::FVector at{};
    ue_wrap::FRotator rot{};
    char buf[160] = "?";
    if (vent && E::TryGetActorLocation(vent, at) && E::TryGetActorRotation(vent, rot))
        std::snprintf(buf, sizeof(buf), "%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%d", at.X, at.Y, at.Z, rot.Pitch, rot.Yaw,
                      rot.Roll, ue_wrap::prop::IsFrozen(vent) ? 1 : 0);
    return row + buf;
}

// Within 5 cm and 5 degrees, field by field, the vent's frozen flag exact.
bool SameVent(const std::string& a, const std::string& b) {
    const auto x = Split(a, ','), y = Split(b, ',');
    if (x.size() != 7 || y.size() != 7) return false;
    for (int i = 0; i < 6; ++i)
        if (std::fabs(std::atof(x[i].c_str()) - std::atof(y[i].c_str())) > 5.f) return false;
    return x[6] == y[6];
}

// Every field of the row the host's: the two doors, the keypads and the vent.
bool Agrees(const std::string& mine, const std::string& host) {
    const auto m = Split(mine), h = Split(host);
    return m.size() == 4 && h.size() == 4 && m[0] == h[0] && m[1] == h[1] && m[2] == h[2] && SameVent(m[3], h[3]);
}

void Compare(const std::string& mine, const std::string& host, const char* when) {
    const auto m = Split(mine), h = Split(host);
    UE_LOGI("crawler-drill: %hs -- here %hs, the host %hs", when, mine.c_str(), host.c_str());
    char what[160];
    std::snprintf(what, sizeof(what), "%s: the signal room door is the host's", when);
    Check(m.size() == 4 && h.size() == 4 && m[0] == h[0], what);
    std::snprintf(what, sizeof(what), "%s: the balcony door is the host's", when);
    Check(m.size() == 4 && h.size() == 4 && m[1] == h[1], what);
    std::snprintf(what, sizeof(what), "%s: the balcony door's keypads are the host's", when);
    Check(m.size() == 4 && h.size() == 4 && m[2] == h[2], what);
    std::snprintf(what, sizeof(what), "%s: the hanging vent is where the host's is", when);
    Check(m.size() == 4 && h.size() == 4 && SameVent(m[3], h[3]), what);
}

std::string Where(void* actor) {
    ue_wrap::FVector at{};
    char buf[96] = "";
    if (actor && E::TryGetActorLocation(actor, at)) std::snprintf(buf, sizeof(buf), "%.0f|%.0f|%.0f", at.X, at.Y, at.Z);
    return buf;
}

// ---- HOST ----------------------------------------------------------------------------------------------------------

// The host's crawler placed in the world (its spawn is deferred, and it stands at the origin until it finishes), with its
// box ("x|y|z|reach") and its place. Drill thread.
bool WaitCrawlerPlaced(void*& crawler, std::string& box, std::string& start) {
    return WaitFor([&] {
        crawler = Crawler();
        void* b = ObjField(crawler, L"Box");
        ue_wrap::FVector at{}, me{};
        const float reach = BoxReach(b);
        if (!b || reach <= 0.f || !E::TryGetComponentLocation(b, at) || !E::TryGetActorLocation(crawler, me) ||
            std::fabs(me.X) + std::fabs(me.Y) < 1.f)
            return false;
        char buf[112];
        std::snprintf(buf, sizeof(buf), "%.0f|%.0f|%.0f|%.0f", at.X, at.Y, at.Z, reach);
        box = buf;
        start = Where(crawler);
        return true;
    }, 10);
}

// The signal room door open, so the crawl's close has something to move. isOpened is set where the swing ends, so this
// waits for the door to have finished opening.
void OpenSignalRoomDoor() {
    OnGameThread([&] {
        void* door = ue_wrap::keyed_objects::Resolve(L"basedoor_signalroom");
        if (door) D::CallDoorOpen(door, true);
    });
    Check(WaitFor([&] {
              void* door = ue_wrap::keyed_objects::Resolve(L"basedoor_signalroom");
              bool open = false;
              return door && D::TryReadOpen(door, open) && open;
          }, 15),
          "the signal room door is open before the crawl");
}

// The crawl's last act opens the balcony door (its keypads first), and isOpened is set where that swing ends.
void AwaitBalconyOpen() {
    Check(WaitFor([&] {
              void* door = ue_wrap::keyed_objects::Resolve(L"basedoor_balcony");
              bool open = false;
              return door && D::TryReadOpen(door, open) && open;
          }, 20),
          "the balcony door opened at the crawl's end");
}

void RunHost() {
    if (!Await("client_ready", 400)) return;
    // Life 1's occupancy of slot 1: the rejoin step waits for it to change, never for the slot to read closed -- a fast
    // rejoin closes and refills the slot between two reads.
    const uint32_t life1Gen = harness::session_runtime::Session().peerGenerationForSlot(1);
    OpenSignalRoomDoor();
    std::string row;
    OnGameThread([&] { row = StateRow(); });
    UE_LOGI("crawler-drill: host before %hs", row.c_str());
    Mark("h_before", row);
    if (!Await("c_before")) return;
    Check(coop::event_fire_sync::HostFire(coop::event_fire_sync::FireKind::RunEvent, L"ventCrawler", L"None"),
          "the host fired ventCrawler");
    void* crawler = nullptr;
    std::string box, start;
    Check(WaitCrawlerPlaced(crawler, box, start), "the host's crawler is in the world");
    UE_LOGI("crawler-drill: host crawler at %hs, its box at %hs", start.c_str(), box.c_str());
    Mark("h_spawn", box);
    if (!Await("c_in")) return;
    const auto s = Split(start);
    auto moved = [&] {
        ue_wrap::FVector at{};
        return s.size() == 3 && R::IsLive(crawler) && E::TryGetActorLocation(crawler, at) &&
               std::fabs(at.Y - std::atof(s[1].c_str())) > 50.f;
    };
    // The crawl starts 5 s after the box is touched: two vent steps, then the timeline.
    const bool byClient = WaitFor(moved, 15);
    Check(byClient, "the client's player in the box started the crawl on the host");
    if (!byClient) {
        Check(WalkInto(box), "the host's player walked into the box");
        Check(WaitFor(moved, 15), "the host's own player started it");
    }
    Mark("h_crawl", byClient ? "client" : "host");
    for (int i = 0; i < 600; ++i) {
        bool live = false;
        OnGameThread([&] {
            void* c = Crawler();
            live = c != nullptr;
            if (i % 10 == 0) UE_LOGI("crawler-drill: host mid %hs at %hs", StateRow().c_str(), Where(c).c_str());
        });
        if (!live) break;
        ::Sleep(100);
    }
    Check(WaitFor([&] { return Crawler() == nullptr; }, 60), "the host's crawler went at the crawl's end");
    AwaitBalconyOpen();
    OnGameThread([&] { row = StateRow(); });
    UE_LOGI("crawler-drill: host after %hs", row.c_str());
    Mark("h_after", row);
    std::string clientText;
    if (!AwaitText("c_done", clientText, 240)) return;
    const int clientFails = ParseFails(clientText);
    // The rejoin: life 1 leaves once the host watches its slot, the crawl is over, and the rejoined life must find the
    // vent and the doors as they are. The host marks what it must read only after that life reports its world ready.
    Mark("host_ack");
    if (!PS::WaitFor("the client's life 1 gone from slot 1", 300, [life1Gen] {
            auto& s = harness::session_runtime::Session();
            return !s.IsSlotConnected(1) || s.peerGenerationForSlot(1) != life1Gen;
        }))
        return;
    if (!Await("rejoin_ready", 400)) return;
    OnGameThread([&] { row = StateRow(); });
    UE_LOGI("crawler-drill: host for the rejoin %hs", row.c_str());
    Mark("e_row", row);
    std::string rejoinText;
    const int rejoinFails = AwaitText("rejoin_done", rejoinText, 400) ? ParseFails(rejoinText) : 1;   // no word is no pass
    if (g_fails == 0 && clientFails == 0 && rejoinFails == 0) UE_LOGI("crawler-drill: VERDICT PASS");
    else UE_LOGW("crawler-drill: VERDICT FAIL host=%d client=%d rejoin=%d", g_fails, clientFails, rejoinFails);
    UE_LOGI("crawler-drill: DONE on host fails=%d", g_fails);
}

// The host-first arm (VOTVCOOP_CRAWLER_HOST_FIRST=1): the host's player walks into the box right after the host's world
// settles, before any client has joined, so the crawl is recorded with nobody to hear it. The client that joins after it
// must be told where it stands, and find the vent and doors as the host's are.
void RunHostFirst() {
    if (!PS::WaitFor("the host's world to settle", 300, [&] {
            bool settled = false;
            OnGameThread([&] { settled = coop::host_world_settle::Settled() && Me(); });
            return settled;
        }))
        return;
    OpenSignalRoomDoor();
    Check(coop::event_fire_sync::HostFire(coop::event_fire_sync::FireKind::RunEvent, L"ventCrawler", L"None"),
          "the host fired ventCrawler");
    void* crawler = nullptr;
    std::string box, start;
    Check(WaitCrawlerPlaced(crawler, box, start), "the host's crawler is in the world");
    UE_LOGI("crawler-drill: host crawler at %hs, its box at %hs, no client yet", start.c_str(), box.c_str());
    const auto s = Split(start);
    Check(WalkInto(box), "the host's player walked into the box");
    Check(WaitFor([&] {
              ue_wrap::FVector at{};
              return s.size() == 3 && R::IsLive(crawler) && E::TryGetActorLocation(crawler, at) &&
                     std::fabs(at.Y - std::atof(s[1].c_str())) > 50.f;
          }, 15),
          "the host's own player started the crawl");
    // The arm measures a crawl begun ALONE: a client already seated makes it measure the live cue instead. The rig holds
    // the client's launch on the crawl's end (mp.py smoke --client-after); a run without that is not this arm's.
    bool alone = true;
    OnGameThread([&] { alone = !harness::session_runtime::Session().connected(); });
    if (!alone) {
        UE_LOGW("crawler-drill: VERDICT INCONCLUSIVE -- a client was seated when the host's crawl began "
                "(launch the client with mp.py smoke --client-after)");
        return;
    }
    Check(WaitFor([&] { return Crawler() == nullptr; }, 60), "the host's crawler went at the crawl's end");
    AwaitBalconyOpen();
    // The client joins after this, as late as it takes it to load: its readiness is the host's cue to read the rows.
    if (!Await("client_ready", 400)) return;
    std::string row;
    OnGameThread([&] { row = StateRow(); });
    UE_LOGI("crawler-drill: host for the late joiner %hs", row.c_str());
    Mark("hf_row", row);
    std::string clientText;
    const int clientFails = AwaitText("c_done", clientText, 240) ? ParseFails(clientText) : 1;   // no word is no pass
    if (g_fails == 0 && clientFails == 0) UE_LOGI("crawler-drill: VERDICT PASS");
    else UE_LOGW("crawler-drill: VERDICT FAIL host=%d client=%d", g_fails, clientFails);
    UE_LOGI("crawler-drill: DONE on host fails=%d", g_fails);
}

// ---- CLIENT --------------------------------------------------------------------------------------------------------

// This life is ready to act: its world up, the host's puppet here, and past the join's catch-up window, as on day 5 long
// after a join. The window closes at the load tail's quiescence, where the sweep runs and the deferred mirrors are
// revealed (join_membership_sweep.h, mirror_defer.h). Then it stands beside the host.
bool ClientReady() {
    if (!HostPuppetUp()) return false;
    if (!PS::WaitFor("the join's load tail to quiesce", 180, [&] {
            bool quiet = false;
            OnGameThread([&] { quiet = coop::join_membership_sweep::HasLoadTailQuiesced(); });
            return quiet;
        }))
        return false;
    return meet::WalkToHost("crawler-drill");
}

void RunClient() {
    if (!ClientReady()) return;
    Mark("client_ready");
    std::string host, row;
    if (!AwaitText("h_before", host)) return;
    WaitFor([&] { return Agrees(StateRow(), host); }, 20);   // the host's door has reached this copy; Compare reports what differs
    OnGameThread([&] { row = StateRow(); });
    Compare(row, host, "before");
    Mark("c_before");
    std::string box;
    if (!AwaitText("h_spawn", box)) return;
    void* mirror = nullptr;
    Check(WaitFor([&] {
              ue_wrap::FVector at{};
              mirror = Crawler();
              return mirror && E::TryGetActorLocation(mirror, at) && std::fabs(at.X) + std::fabs(at.Y) >= 1.f;
          }, 10),
          "the host's crawler is here");
    std::string start;
    OnGameThread([&] { start = Where(mirror); });
    UE_LOGI("crawler-drill: client crawler at %hs (the host's box %hs)", start.c_str(), box.c_str());
    // Into the host's box, which starts the crawl; the player stays there.
    if (!WalkInto(box)) {
        UE_LOGW("crawler-drill: VERDICT ABORT -- the client's player could not walk into the box");
        return;
    }
    Mark("c_in");
    // The mirror crawls as the host's does, and goes when it goes.
    const auto s = Split(start);
    const unsigned sounds0 = coop::vent_crawler_sync::SoundsPlayed();
    bool sawOff = false, sawShut = false;
    float crawled = 0.f;
    for (int i = 0; i < 900; ++i) {
        bool live = false;
        OnGameThread([&] {
            void* c = Crawler();
            ue_wrap::FVector at{};
            live = c != nullptr;
            if (c && s.size() == 3 && E::TryGetActorLocation(c, at))
                crawled = (std::max)(crawled, std::fabs(at.Y - static_cast<float>(std::atof(s[1].c_str()))));
            const std::string mid = StateRow();
            sawOff = sawOff || mid.rfind("shut,off", 0) == 0;
            sawShut = sawShut || mid.rfind("shut", 0) == 0;
            if (i % 10 == 0) UE_LOGI("crawler-drill: client mid %hs at %hs", mid.c_str(), Where(c).c_str());
        });
        if (!live) break;
        ::Sleep(100);
    }
    UE_LOGI("crawler-drill: the client's crawler went %.0f cm along its crawl", crawled);
    Check(crawled > 1500.f, "the crawler crawled here as on the host");
    Check(sawShut, "the crawl closed the signal room door here");
    Check(sawOff, "the crawl took the signal room door's power here, as on the host");
    const unsigned sounds = coop::vent_crawler_sync::SoundsPlayed() - sounds0;
    UE_LOGI("crawler-drill: the crawl played %u sounds here", sounds);
    Check(sounds >= 4, "the two vent steps, steps along the crawl and the metal break played here");
    std::string after;
    if (!AwaitText("h_after", after, 120)) return;
    Check(WaitFor([&] { return Crawler() == nullptr; }, 5), "the crawler went here");
    // The vent's swing outlives the mirror: wait for the state to be the host's; Compare then reports what differs.
    WaitFor([&] { return Agrees(StateRow(), after); }, 20);
    OnGameThread([&] { row = StateRow(); });
    Compare(row, after, "after");
    Mark("c_done", std::to_string(g_fails));
    // The DONE line is the rig's cue to kill this life: printed only once the host watches slot 1, so the rejoin
    // happens with the crawl over. Life 1 leaves as a player's quit does (network first), so the host sees slot 1 empty
    // at once; a killed process only stops sending.
    Await("host_ack", 60);
    OnGameThread([] {
        auto& s = harness::session_runtime::Session();
        if (s.running()) coop::net_pump::FleeAfterNativeMenuTravel(s, "crawler drill: life 1 leaves for the rejoin");
    });
    PS::WaitFor("this life's session stopped", 15, [] { return !harness::session_runtime::Session().running(); }, false);
    UE_LOGI("crawler-drill: DONE on client fails=%d", g_fails);
}

// The late joiner of the host-first arm: the crawl began and ran its course on the host before this peer was here, and
// the vent, both doors and the balcony's keypads here must be the host's, which the join's answer (VentCrawlState) gave.
void RunLateJoiner() {
    if (!ClientReady()) return;
    Mark("client_ready");
    std::string host, row;
    if (!AwaitText("hf_row", host, 120)) return;
    WaitFor([&] { return Agrees(StateRow(), host); }, 20);   // the replay and the join window's corrections have landed
    OnGameThread([&] { row = StateRow(); });
    Compare(row, host, "late join");
    Check(coop::vent_crawler_sync::StatesHeard() > 0, "the host's crawl state reached this joiner");
    Mark("c_done", std::to_string(g_fails));
    UE_LOGI("crawler-drill: DONE on client fails=%d", g_fails);
}

// The rejoined life (VOTVCOOP_CRAWLER_REJOIN=1): the crawl is over on the host, and the vent, both doors and the
// balcony's keypads here must be the host's, which the join's answer (VentCrawlState) gave this life.
void RunRejoin() {
    if (!ClientReady()) return;
    Mark("rejoin_ready");
    std::string host, row;
    if (!AwaitText("e_row", host, 120)) return;
    WaitFor([&] { return Agrees(StateRow(), host); }, 20);   // the replay and the join window's corrections have landed
    OnGameThread([&] { row = StateRow(); });
    Compare(row, host, "rejoin");
    Check(coop::vent_crawler_sync::StatesHeard() > 0, "the host's crawl state reached this life");
    Mark("rejoin_done", std::to_string(g_fails));
    UE_LOGI("crawler-drill: REJOIN DONE fails=%d", g_fails);
}

}  // namespace

DWORD WINAPI VentCrawlerThread(LPVOID /*arg*/) {
    if (!PS::Begin("crawler-drill")) return 0;
    const bool hostFirst = coop::config::ReadEnv("VOTVCOOP_CRAWLER_HOST_FIRST") == "1";
    if (!IsClientRole()) hostFirst ? RunHostFirst() : RunHost();
    else if (hostFirst) RunLateJoiner();
    else if (coop::config::ReadEnv("VOTVCOOP_CRAWLER_REJOIN") == "1") RunRejoin();
    else RunClient();
    return 0;
}

}  // namespace harness::autotest
