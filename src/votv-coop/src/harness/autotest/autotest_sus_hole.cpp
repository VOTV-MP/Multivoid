// harness/autotest/autotest_sus_hole.cpp -- TEST-ONLY: a sus hole rolls around a client, not the host alone.
//
// The host rolls the sus hole spawner's roll around the client's body until one lands on grass. The hole must stand 100 to 500 m
// from the client, reach the client at the host's place, and stay there when the host's copy ends, as a hole ends by its own
// viewer. Gated by VOTVCOOP_RUN_SUS_HOLE; the folder is VOTVCOOP_CAN_DIR.

#include "harness/autotest.h"

#include "coop/config/config.h"
#include "coop/creatures/ambient_fixture_sync.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/session/net_pump.h"
#include "ue_wrap/actors/ambient_creature.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <fstream>
#include <string>
#include <unordered_set>
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

void Mark(const char* name, const std::string& body = "x") {
    std::string tmp = g_dir + "\\" + name + ".tmp";
    { std::ofstream(tmp, std::ios::binary | std::ios::trunc) << body; }
    ::MoveFileExA(tmp.c_str(), (g_dir + "\\" + name).c_str(), MOVEFILE_REPLACE_EXISTING);
}

bool Seen(const char* name) { return static_cast<bool>(std::ifstream(g_dir + "\\" + name)); }

bool Await(const char* name, int seconds = 240) {
    for (int i = 0; i < seconds * 10; ++i) {
        if (Seen(name)) return true;
        ::Sleep(100);
    }
    UE_LOGW("sus-hole: VERDICT ABORT -- no '%hs' from the other peer", name);
    return false;
}

void Check(bool ok, const char* what) {
    UE_LOGI("sus-hole: %hs %hs", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fails;
}

float Flat(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y;
    return std::sqrt(dx * dx + dy * dy);
}

// The hole standing within 100 cm of `at`, or null.
void* HoleAt(const ue_wrap::FVector& at) {
    std::vector<void*> holes;
    AC::ListHoles(holes);
    for (void* h : holes) {
        ue_wrap::FVector p{};
        if (E::TryGetActorLocation(h, p) && Flat(p, at) < 100.f && std::fabs(p.Z - at.Z) < 100.f) return h;
    }
    return nullptr;
}

void RunHost() {
    if (!Await("client_ready", 480)) return;
    bool rolls = false;
    std::unordered_set<void*> before;
    OnGameThread([&] {
        rolls = AC::HolesRoll();
        std::vector<void*> holes;
        AC::ListHoles(holes);
        before.insert(holes.begin(), holes.end());
    });
    Check(rolls, "the game's mode lets sus holes roll");
    int tries = 0;
    bool made = false;
    for (; tries < 400 && !made; ++tries) OnGameThread([&] { made = coop::ambient_fixture_sync::DebugRollHoleNear(1); });
    UE_LOGI("sus-hole: %d rolls around the client for one on grass", tries);
    Check(made, "a roll around the client found grass");
    if (!made) return;
    void* hole = nullptr;
    ue_wrap::FVector at{}, client{};
    OnGameThread([&] {
        std::vector<void*> holes;
        AC::ListHoles(holes);
        for (void* h : holes)
            if (!before.count(h)) hole = h;
        coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(1);
        if (hole) E::TryGetActorLocation(hole, at);
        if (rp && rp->valid()) E::TryGetActorLocation(rp->GetActor(), client);
    });
    Check(hole != nullptr, "the hole stands here");
    if (!hole) return;
    const float off = Flat(at, client);
    UE_LOGI("sus-hole: hole at (%.0f,%.0f,%.0f), %.0f m from the client", at.X, at.Y, at.Z, off / 100.f);
    Check(off >= 9000.f && off <= 51000.f, "it stands 100 to 500 m from the client");
    Mark("hole", std::to_string(at.X) + " " + std::to_string(at.Y) + " " + std::to_string(at.Z));
    if (!Await("hole_seen", 60)) return;

    // The host's copy ends by the host's view; the client's stays.
    OnGameThread([&] { if (R::IsLive(hole)) AC::Destroy(hole); });
    ::Sleep(3000);
    Mark("host_ended");
    Await("client_done", 30);
    UE_LOGI("sus-hole: DONE on host fails=%d", g_fails);
}

void RunClient() {
    bool ready = false;
    for (int i = 0; i < 480 && !ready; ++i) {
        OnGameThread([&] { ready = coop::players::Registry::Get().Local() && coop::net_pump::HasAnnouncedWorldReady(); });
        if (!ready) ::Sleep(500);
    }
    ::Sleep(8000);
    Mark("client_ready");
    if (!Await("hole", 240)) return;
    ue_wrap::FVector at{};
    std::ifstream(g_dir + "\\hole") >> at.X >> at.Y >> at.Z;
    void* hole = nullptr;
    for (int i = 0; i < 200 && !hole; ++i) {
        OnGameThread([&] { hole = HoleAt(at); });
        if (!hole) ::Sleep(100);
    }
    Check(hole != nullptr, "the host's hole stands here at its place");
    ue_wrap::FVector me{};
    OnGameThread([&] {
        void* pawn = coop::players::Registry::Get().Local();
        if (pawn) E::TryGetActorLocation(pawn, me);
    });
    const float off = Flat(at, me);
    UE_LOGI("sus-hole: the hole is %.0f m from this player", off / 100.f);
    Check(off >= 9000.f && off <= 51000.f, "it is 100 to 500 m from this player");
    Mark("hole_seen");
    if (!Await("host_ended", 60)) return;
    ::Sleep(2000);
    bool stays = false;
    OnGameThread([&] { stays = hole && R::IsLive(hole) && HoleAt(at) == hole; });
    Check(stays, "the host's copy ending leaves this player's hole");
    Mark("client_done");
    UE_LOGI("sus-hole: DONE on client fails=%d", g_fails);
}

}  // namespace

DWORD WINAPI SusHoleThread(LPVOID /*arg*/) {
    g_dir = coop::config::ReadEnv("VOTVCOOP_CAN_DIR");
    if (g_dir.empty()) {
        UE_LOGW("sus-hole: VERDICT ABORT -- VOTVCOOP_CAN_DIR is unset");
        return 0;
    }
    if (IsClientRole()) RunClient();
    else RunHost();
    return 0;
}

}  // namespace harness::autotest
