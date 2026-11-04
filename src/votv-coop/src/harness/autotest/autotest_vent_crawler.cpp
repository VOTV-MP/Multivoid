// harness/autotest/autotest_vent_crawler.cpp -- TEST-ONLY: the day 5 vent crawler event, host and client side by side.
//
// The host fires `ventCrawler` through the eventer as the scheduler does. The client has the crawler's mirror; its
// player goes into the crawler's box, which starts the crawl on the host (the host's own player does, if that one
// does not). The crawl closes the signal room door (opened first) and takes its power, plays its steps, breaks and
// swings the hanging vent down, gives the power back, and at its end opens the balcony door's keypads and the door,
// and the crawler goes. The client's door closes and loses its power, and the crawl's sounds play there; each peer
// then reads the two doors, the keypads and the vent, and the client's must be the host's. The client waits a minute
// after its join first, past the join's catch-up window, and the game's backrooms clip is off around the drill's
// teleports. Gated by VOTVCOOP_RUN_VENT_CRAWLER; the folder is VOTVCOOP_CAN_DIR.

#include "harness/autotest.h"

#include "coop/config/config.h"
#include "coop/player/players_registry.h"
#include "coop/session/net_pump.h"
#include "coop/world/event_fire_sync.h"
#include "coop/world/vent_crawler_sync.h"
#include "ue_wrap/actors/prop.h"
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
#include "ue_wrap/world/world_singleton.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace harness::autotest {
namespace {

namespace D = ue_wrap::door;
namespace E = ue_wrap::engine;
namespace PL = ue_wrap::passwordlock;
namespace R = ue_wrap::reflection;

std::string g_dir;
int g_fails = 0;

template <class Fn>
void OnGameThread(Fn&& fn) {
    ue_wrap::game_thread::RunAndWait([&fn](std::atomic<int>& d) { fn(); d.store(1); });
}

void Mark(const char* name, const std::string& body = "x") {
    std::string tmp = g_dir + "\\" + name + ".tmp";
    { std::ofstream(tmp, std::ios::binary | std::ios::trunc) << body; }
    ::MoveFileExA(tmp.c_str(), (g_dir + "\\" + name).c_str(), MOVEFILE_REPLACE_EXISTING);
}

bool Await(const char* name, std::string* body = nullptr, int seconds = 240) {
    for (int i = 0; i < seconds * 10; ++i) {
        std::ifstream f(g_dir + "\\" + name, std::ios::binary);
        if (f) {
            if (body) body->assign(std::istreambuf_iterator<char>(f), {});
            return true;
        }
        ::Sleep(100);
    }
    UE_LOGW("crawler-drill: VERDICT ABORT -- no '%hs' from the other peer", name);
    return false;
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

// The game's clip check: a camera that crosses static geometry between two frames sends its player to the
// backrooms, after a save. The drill's teleports into the vent cross walls, so it turns that off around them and
// gives back what it found. Returns what it was.
bool Backrooms(bool on) {
    void* gm = ue_wrap::world_singleton::Gamemode();
    int32_t off = -1;
    uint8_t mask = 0;
    if (!gm || !R::IsLive(gm) || !R::FindBoolProperty(R::ClassOf(gm), L"backroomsEnabled", off, mask)) return false;
    uint8_t& b = static_cast<uint8_t*>(gm)[off];
    const bool was = (b & mask) != 0;
    b = on ? (b | mask) : (b & ~mask);
    return was;
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

void RunHost() {
    if (!Await("client_ready", nullptr, 400)) return;
    // The signal room door open, so the crawl's close has something to move.
    OnGameThread([&] {
        void* door = ue_wrap::keyed_objects::Resolve(L"basedoor_signalroom");
        if (door) D::CallDoorOpen(door, true);
    });
    ::Sleep(4000);
    std::string row;
    OnGameThread([&] { row = StateRow(); });
    UE_LOGI("crawler-drill: host before %hs", row.c_str());
    Mark("h_before", row);
    if (!Await("c_before")) return;
    Check(coop::event_fire_sync::HostFire(coop::event_fire_sync::FireKind::RunEvent, L"ventCrawler", L"None"),
          "the host fired ventCrawler");
    void* crawler = nullptr;
    std::string box, start;
    // Placed: its spawn is deferred, and it stands at the origin until it finishes.
    Check(WaitFor([&] {
              crawler = Crawler();
              void* b = ObjField(crawler, L"Box");
              ue_wrap::FVector at{}, me{};
              if (!b || !E::TryGetComponentLocation(b, at) || !E::TryGetActorLocation(crawler, me) ||
                  std::fabs(me.X) + std::fabs(me.Y) < 1.f)
                  return false;
              char buf[96];
              std::snprintf(buf, sizeof(buf), "%.0f|%.0f|%.0f", at.X, at.Y, at.Z);
              box = buf;
              start = Where(crawler);
              return true;
          }, 10),
          "the host's crawler is in the world");
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
        bool backrooms = false;
        OnGameThread([&] {
            backrooms = Backrooms(false);
            const auto b = Split(box);
            if (b.size() == 3)
                E::TeleportTo(Me(), {static_cast<float>(std::atof(b[0].c_str())),
                                     static_cast<float>(std::atof(b[1].c_str())),
                                     static_cast<float>(std::atof(b[2].c_str()))}, {});
        });
        Check(WaitFor(moved, 15), "the host's own player started it");
        ::Sleep(1000);
        OnGameThread([&] { Backrooms(backrooms); });
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
    ::Sleep(3000);
    OnGameThread([&] { row = StateRow(); });
    UE_LOGI("crawler-drill: host after %hs", row.c_str());
    Mark("h_after", row);
    Await("c_done");
    UE_LOGI("crawler-drill: DONE on host fails=%d", g_fails);
}

// ---- CLIENT --------------------------------------------------------------------------------------------------------

void RunClient() {
    WorldReady();
    ::Sleep(60000);   // past the join's catch-up window, as on day 5 long after a join
    Mark("client_ready");
    std::string host, row;
    if (!Await("h_before", &host)) return;
    OnGameThread([&] { row = StateRow(); });
    Compare(row, host, "before");
    Mark("c_before");
    std::string box;
    if (!Await("h_spawn", &box)) return;
    void* mirror = nullptr;
    Check(WaitFor([&] {
              ue_wrap::FVector at{};
              mirror = Crawler();
              return mirror && E::TryGetActorLocation(mirror, at) && std::fabs(at.X) + std::fabs(at.Y) >= 1.f;
          }, 10),
          "the host's crawler is here");
    std::string start, home;
    OnGameThread([&] {
        start = Where(mirror);
        home = Where(Me());
    });
    UE_LOGI("crawler-drill: client crawler at %hs (the host's box %hs)", start.c_str(), box.c_str());
    // Into the host's box, and back out a second later.
    bool backrooms = false;
    OnGameThread([&] {
        backrooms = Backrooms(false);
        const auto b = Split(box);
        if (b.size() == 3)
            E::TeleportTo(Me(), {static_cast<float>(std::atof(b[0].c_str())), static_cast<float>(std::atof(b[1].c_str())),
                                 static_cast<float>(std::atof(b[2].c_str()))}, {});
    });
    Mark("c_in");
    ::Sleep(1500);
    OnGameThread([&] {
        const auto h = Split(home);
        if (h.size() == 3)
            E::TeleportTo(Me(), {static_cast<float>(std::atof(h[0].c_str())), static_cast<float>(std::atof(h[1].c_str())),
                                 static_cast<float>(std::atof(h[2].c_str()))}, {});
    });
    ::Sleep(1000);
    OnGameThread([&] { Backrooms(backrooms); });
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
    if (!Await("h_after", &after, 120)) return;
    Check(WaitFor([&] { return Crawler() == nullptr; }, 5), "the crawler went here");
    ::Sleep(1000);
    OnGameThread([&] { row = StateRow(); });
    Compare(row, after, "after");
    Mark("c_done");
    UE_LOGI("crawler-drill: DONE on client fails=%d", g_fails);
}

}  // namespace

DWORD WINAPI VentCrawlerThread(LPVOID /*arg*/) {
    g_dir = coop::config::ReadEnv("VOTVCOOP_CAN_DIR");
    if (g_dir.empty()) {
        UE_LOGW("crawler-drill: VERDICT ABORT -- VOTVCOOP_CAN_DIR is not set");
        return 0;
    }
    if (!IsClientRole()) RunHost();
    else RunClient();
    return 0;
}

}  // namespace harness::autotest
