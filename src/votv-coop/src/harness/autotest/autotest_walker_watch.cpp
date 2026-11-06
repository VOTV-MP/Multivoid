// harness/autotest/autotest_walker_watch.cpp -- TEST-ONLY: a walking mannequin freezes for any viewer.
//
// The host looks away for the whole drill past its own first check, so only the client's view can freeze the host's walker. Each
// step reads the walker's own `looking`, which is what stops its walking, and its place. Gated by VOTVCOOP_RUN_WALKER_WATCH; the
// folder is VOTVCOOP_CAN_DIR.

#include "harness/autotest.h"

#include "coop/config/config.h"
#include "coop/dev/mannequin_drill.h"
#include "coop/player/players_registry.h"
#include "coop/session/net_pump.h"
#include "ue_wrap/actors/ambient_creature.h"
#include "ue_wrap/actors/vitals.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_pawn.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <fstream>
#include <string>
#include <vector>

namespace harness::autotest {
namespace {

namespace AC = ue_wrap::ambient_creature;
namespace E = ue_wrap::engine;
namespace R = ue_wrap::reflection;

std::string g_dir;
int g_fails = 0;

template <class Fn>
void OnGameThread(Fn&& fn) {
    ue_wrap::game_thread::RunAndWait([&fn](std::atomic<int>& d) { fn(); d.store(1); });
}

void Mark(const char* name) {
    std::string tmp = g_dir + "\\" + name + ".tmp";
    { std::ofstream(tmp, std::ios::binary | std::ios::trunc) << "x"; }
    ::MoveFileExA(tmp.c_str(), (g_dir + "\\" + name).c_str(), MOVEFILE_REPLACE_EXISTING);
}

bool Seen(const char* name) { return static_cast<bool>(std::ifstream(g_dir + "\\" + name)); }

bool Await(const char* name, int seconds = 240) {
    for (int i = 0; i < seconds * 10; ++i) {
        if (Seen(name)) return true;
        ::Sleep(100);
    }
    UE_LOGW("walker-watch: VERDICT ABORT -- no '%hs' from the other peer", name);
    return false;
}

void Check(bool ok, const char* what) {
    UE_LOGI("walker-watch: %hs %hs", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fails;
}

void* FindWalker() {
    std::vector<void*> v;
    AC::ListWalkers(v);
    return v.empty() ? nullptr : v.back();
}

// Faces this player toward the walker, or away from it.
void Face(void* walker, bool toward) {
    void* pawn = coop::players::Registry::Get().Local();
    ue_wrap::FVector at{};
    if (!pawn || !walker || !R::IsLive(walker) || !E::TryGetActorLocation(walker, at)) return;
    const ue_wrap::FVector me = E::GetCameraLocation();
    const float dx = at.X - me.X, dy = at.Y - me.Y, dz = at.Z - me.Z;
    const float yaw = std::atan2(dy, dx) * 57.29578f + (toward ? 0.f : 180.f);
    const float pitch = toward ? std::atan2(dz, std::sqrt(dx * dx + dy * dy)) * 57.29578f : 0.f;
    E::SetControlRotation(E::GetController(pawn), ue_wrap::FRotator{pitch, yaw, 0.f});
}

// The game's ticker_foodSleeper_C drops a hungry or tired player into a ragdoll at random, and a walker's tick stands it while
// player 0 lies down, so the rig save's low vitals would read as a look.
void KeepFed() {
    namespace V = ue_wrap::vitals;
    float food = 0.f, sleep = 0.f;
    V::Read(V::Field::Food, &food);
    V::Read(V::Field::Sleep, &sleep);
    static bool sSaid = false;
    if (!sSaid) {
        sSaid = true;
        UE_LOGI("walker-watch: the save's vitals were food %.1f sleep %.1f", food, sleep);
    }
    V::Write(V::Field::Food, 100.f);
    V::Write(V::Field::Sleep, 100.f);
}

// Keeps the host's camera off the walker for `ms`, and returns how many samples read it looking, of how many.
void HostSample(void* walker, int ms, int& looking, int& total, float& moved) {
    ue_wrap::FVector first{}, last{};
    looking = total = 0;
    OnGameThread([&] { E::TryGetActorLocation(walker, first); });
    last = first;
    for (int t = 0; t < ms; t += 100) {
        OnGameThread([&] {
            KeepFed();
            Face(walker, false);
            if (AC::ReadWalkerLooking(walker)) ++looking;
            ++total;
            E::TryGetActorLocation(walker, last);
        });
        ::Sleep(100);
    }
    const float dx = last.X - first.X, dy = last.Y - first.Y;
    moved = std::sqrt(dx * dx + dy * dy);
}

void RunHost() {
    if (!Await("client_ready", 480)) return;
    OnGameThread([] { KeepFed(); });
    coop::dev::mannequin_drill::SpawnWalker();
    void* walker = nullptr;
    for (int i = 0; i < 100 && !walker; ++i) {
        ::Sleep(100);
        OnGameThread([&] { walker = FindWalker(); });
    }
    Check(walker != nullptr, "a walker stands");
    if (!walker) return;
    Mark("walker");
    if (!Await("client_away")) return;

    // Nobody looks: its own timer lets it go.
    bool free = false;
    for (int i = 0; i < 400 && !free; ++i) {
        OnGameThread([&] { KeepFed(); Face(walker, false); free = !AC::ReadWalkerLooking(walker); });
        ::Sleep(100);
    }
    Check(free, "with no viewer the walker is free");
    if (!free) return;
    int looking = 0, total = 0;
    float moved = 0.f;
    HostSample(walker, 6000, looking, total, moved);
    UE_LOGI("walker-watch: no viewer: looking %d of %d, moved %.0f cm", looking, total, moved);
    Check(looking == 0, "it stays free while nobody looks");

    Mark("look");
    if (!Await("looking", 60)) return;
    // A walker stopped in stride rocks on its feet for about a second, a few cm on rough ground.
    ::Sleep(2000);
    HostSample(walker, 25000, looking, total, moved);
    UE_LOGI("walker-watch: client looking: looking %d of %d, moved %.0f cm", looking, total, moved);
    Check(looking == total, "the client's view holds the host's walker frozen, past its own timer");
    Check(moved < 10.f, "and it does not move");

    Mark("stop");
    if (!Await("stopped", 60)) return;
    free = false;
    for (int i = 0; i < 300 && !free; ++i) {
        OnGameThread([&] { KeepFed(); Face(walker, false); free = !AC::ReadWalkerLooking(walker); });
        ::Sleep(100);
    }
    Check(free, "when the client looks away the walker goes free");
    Mark("host_done");
    Await("client_done", 30);
    UE_LOGI("walker-watch: DONE on host fails=%d", g_fails);
}

void RunClient() {
    bool ready = false;
    for (int i = 0; i < 480 && !ready; ++i) {
        OnGameThread([&] { ready = coop::net_pump::HasAnnouncedWorldReady(); });
        if (!ready) ::Sleep(500);
    }
    ::Sleep(8000);
    Mark("client_ready");
    if (!Await("walker")) return;
    void* walker = nullptr;
    for (int i = 0; i < 300 && !walker; ++i) {
        ::Sleep(100);
        OnGameThread([&] { walker = FindWalker(); });
    }
    Check(walker != nullptr, "the walker crossed");
    if (!walker) return;
    OnGameThread([&] { Face(walker, false); });
    ::Sleep(500);
    Mark("client_away");
    while (!Seen("look")) {
        OnGameThread([&] { Face(walker, false); });
        ::Sleep(100);
    }
    bool inView = false;
    for (int i = 0; i < 100 && !inView; ++i) {
        OnGameThread([&] {
            Face(walker, true);
            void* pawn = coop::players::Registry::Get().Local();
            inView = AC::WalkerInView(walker, pawn ? E::GetController(pawn) : nullptr);
        });
        ::Sleep(100);
    }
    Check(inView, "the walker's own view test reads it on this screen");
    Mark("looking");
    while (!Seen("stop")) {
        OnGameThread([&] { Face(walker, true); });
        ::Sleep(100);
    }
    bool outOfView = false;
    for (int i = 0; i < 100 && !outOfView; ++i) {
        OnGameThread([&] {
            Face(walker, false);
            void* pawn = coop::players::Registry::Get().Local();
            outOfView = !AC::WalkerInView(walker, pawn ? E::GetController(pawn) : nullptr);
        });
        ::Sleep(100);
    }
    Check(outOfView, "looking away takes it off this screen");
    Mark("stopped");
    for (int i = 0; i < 400 && !Seen("host_done"); ++i) {
        OnGameThread([&] { Face(walker, false); });
        ::Sleep(100);
    }
    Mark("client_done");
    UE_LOGI("walker-watch: DONE on client fails=%d", g_fails);
}

}  // namespace

DWORD WINAPI WalkerWatchThread(LPVOID /*arg*/) {
    g_dir = coop::config::ReadEnv("VOTVCOOP_CAN_DIR");
    if (g_dir.empty()) {
        UE_LOGW("walker-watch: VERDICT ABORT -- VOTVCOOP_CAN_DIR is unset");
        return 0;
    }
    if (IsClientRole()) RunClient();
    else RunHost();
    return 0;
}

}  // namespace harness::autotest
