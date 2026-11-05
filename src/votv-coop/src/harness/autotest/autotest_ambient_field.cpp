// harness/autotest/autotest_ambient_field.cpp -- TEST-ONLY: one set of hexahive holes, sus holes and beehive branches across
// two peers.
//
// A hexahive stands before the client joins, so its field reaches the joiner. Then the host runs each spawner's own body and the
// client checks what arrives and that its own rolls do not stand. Gated by VOTVCOOP_RUN_AMBIENT_FIELD; the folder is
// VOTVCOOP_CAN_DIR.

#include "harness/autotest.h"

#include "coop/config/config.h"
#include "coop/player/players_registry.h"
#include "coop/session/net_pump.h"
#include "ue_wrap/actors/ambient_creature.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/core/walk_census.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace harness::autotest {
namespace {

namespace AC = ue_wrap::ambient_creature;
namespace E = ue_wrap::engine;
namespace R = ue_wrap::reflection;
namespace OI = ue_wrap::object_index;

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

bool Await(const char* name, std::string* body = nullptr, int seconds = 300) {
    for (int i = 0; i < seconds * 10; ++i) {
        std::ifstream f(g_dir + "\\" + name, std::ios::binary);
        std::string s;
        if (f && std::getline(f, s) && !s.empty()) {
            if (body) *body = s;
            return true;
        }
        ::Sleep(100);
    }
    UE_LOGW("ambient-field: VERDICT ABORT -- no '%hs' from the other peer", name);
    return false;
}

void Check(bool ok, const char* what) {
    UE_LOGI("ambient-field: %hs %hs", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fails;
}

void* FirstOf(const wchar_t* cls) {
    void* found = nullptr;
    if (void* c = OI::ClassByName(cls))
        OI::ForEachInstance(c, [](void* ctx, void* obj, int32_t) {
            if (R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__")) *static_cast<void**>(ctx) = obj;
        }, &found);
    return found;
}

size_t CountOf(const wchar_t* cls) {
    size_t n = 0;
    if (void* c = OI::ClassByName(cls))
        OI::ForEachInstance(c, [](void* ctx, void* obj, int32_t) {
            if (R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__")) ++*static_cast<size_t*>(ctx);
        }, &n);
    return n;
}

bool RunTick(const wchar_t* cls) {
    void* ticker = FirstOf(cls);
    void* fn = ticker ? R::FindDispatchFunctionCached(R::ClassOf(ticker), L"ReceiveTick") : nullptr;
    if (!fn) return false;
    ue_wrap::ParamFrame f(fn);
    if (!f.valid()) return false;
    f.Set<float>(L"DeltaSeconds", 0.016f);
    return ue_wrap::Call(ticker, f);
}

bool RunBeehiveSpawn() {
    void* ticker = FirstOf(L"ticker_beehiveSpawner_C");
    void* fn = ticker ? R::FindDispatchFunctionCached(R::ClassOf(ticker), L"Spawn") : nullptr;
    if (!fn) return false;
    ue_wrap::ParamFrame f(fn);
    return f.valid() && ue_wrap::Call(ticker, f);
}

// A sum of every place: the two peers' lists hold the same places when the counts and these agree.
struct Field {
    size_t n = 0;
    double x = 0, y = 0, z = 0;
};

Field ReadField(bool branches) {
    Field f;
    std::vector<void*> v;
    if (branches) AC::ListBranches(v);
    else AC::ListHoles(v);
    for (void* a : v) {
        ue_wrap::FVector at{};
        if (!E::TryGetActorLocation(a, at)) continue;
        ++f.n;
        f.x += at.X;
        f.y += at.Y;
        f.z += at.Z;
    }
    return f;
}

std::string Text(const Field& f) {
    char b[128];
    std::snprintf(b, sizeof(b), "%zu %.1f %.1f %.1f", f.n, f.x, f.y, f.z);
    return b;
}

// Waits for this peer's field to be the host's.
void ExpectField(const std::string& hostText, bool branches, const char* what) {
    Field host;
    if (std::sscanf(hostText.c_str(), "%zu %lf %lf %lf", &host.n, &host.x, &host.y, &host.z) != 4) {
        Check(false, what);
        return;
    }
    Field here;
    bool same = false;
    for (int i = 0; i < 100 && !same; ++i) {
        OnGameThread([&] { here = ReadField(branches); });
        const double tol = 2.0 * static_cast<double>((std::max<size_t>)(host.n, 1));
        same = here.n == host.n && std::fabs(here.x - host.x) < tol && std::fabs(here.y - host.y) < tol && std::fabs(here.z - host.z) < tol;
        if (!same) ::Sleep(100);
    }
    UE_LOGI("ambient-field: %hs here=%hs host=%hs", what, Text(here).c_str(), hostText.c_str());
    Check(same, what);
}

void* LocalPawn() { return coop::players::Registry::Get().Local(); }

// The beehive's own retries can add a branch and its nest after the host counted, so the host keeps `branchFile` and
// `nestFile` current and these wait for this peer to agree with the latest.
void ExpectBranchesLive(const char* branchFile, const char* nestFile, const char* branchWhat, const char* nestWhat) {
    std::string body, nests;
    Field host, here;
    size_t nestsHere = 0;
    bool same = false, nestsSame = false;
    for (int i = 0; i < 100 && !(same && nestsSame && i >= 20); ++i) {
        if (!Await(branchFile, &body) || !Await(nestFile, &nests)) return;
        std::sscanf(body.c_str(), "%zu %lf %lf %lf", &host.n, &host.x, &host.y, &host.z);
        OnGameThread([&] {
            here = ReadField(true);
            nestsHere = CountOf(L"prop_waspnest_bee_C");
        });
        const double tol = 2.0 * static_cast<double>((std::max<size_t>)(host.n, 1));
        same = here.n == host.n && std::fabs(here.x - host.x) < tol && std::fabs(here.y - host.y) < tol && std::fabs(here.z - host.z) < tol;
        nestsSame = std::to_string(nestsHere) == nests;
        ::Sleep(100);
    }
    UE_LOGI("ambient-field: %hs here=%hs host=%hs", branchWhat, Text(here).c_str(), body.c_str());
    Check(same, branchWhat);
    UE_LOGI("ambient-field: nests here=%zu host=%hs", nestsHere, nests.c_str());
    Check(nestsSame, nestWhat);
}


// The branches whose nest is live: a branch the save brought links its nest by key, and a copy this lane spawned has none.
size_t BranchesWithNest() {
    std::vector<void*> v;
    AC::ListBranches(v);
    size_t n = 0;
    for (void* b : v) {
        const int32_t off = R::FindPropertyOffset(R::ClassOf(b), L"hive");
        void* hive = off >= 0 ? *reinterpret_cast<void**>(static_cast<uint8_t*>(b) + off) : nullptr;
        if (hive && R::IsLive(hive)) ++n;
    }
    return n;
}

void RunHost() {
    void* pawn = nullptr;
    for (int i = 0; i < 480 && !pawn; ++i) {
        OnGameThread([&] { pawn = LocalPawn(); });
        if (!pawn) ::Sleep(500);
    }
    if (!pawn) return;
    ::Sleep(4000);

    // A real hive's own begin play, beside the base: its field and its self-destroy.
    OnGameThread([&] {
        ue_wrap::FVector me{};
        void* cls = OI::ClassByName(L"hexahiveSPawner_C");
        if (!cls) cls = R::FindClass(L"hexahiveSPawner_C");
        if (cls && E::TryGetActorLocation(pawn, me)) E::SpawnActor(cls, {me.X + 1500.f, me.Y, me.Z + 200.f});
    });
    ::Sleep(1000);
    Field hive;
    OnGameThread([&] { hive = ReadField(false); });
    UE_LOGI("ambient-field: the hive made %zu holes", hive.n);
    Check(hive.n > 0, "the hive's own body made a field of holes");
    size_t hiveSpawners = 99;
    OnGameThread([&] { hiveSpawners = CountOf(L"hexahiveSPawner_C"); });
    Check(hiveSpawners == 0, "the hive's spawner destroyed itself");
    // Read once the client is in: the load's branches come late, and a sus hole the client rolls and the host makes can add a
    // hole of its own.
    if (!Await("client_in", nullptr, 480)) return;
    Field joinBranches;
    OnGameThread([&] {
        hive = ReadField(false);
        joinBranches = ReadField(true);
    });
    Mark("join_branches", Text(joinBranches));
    Mark("hive", Text(hive));
    if (!Await("hive_done", nullptr, 480)) return;

    // The sus hole's tick rolls by the camera and a trace, so it runs until one stands.
    Field before = hive, after = hive;
    for (int i = 0; i < 400 && after.n == before.n; ++i) {
        OnGameThread([&] { RunTick(L"ticker_susHoleSpawner_C"); after = ReadField(false); });
        ::Sleep(10);
    }
    Check(after.n > before.n, "the sus hole spawner's own body made a hole");
    ::Sleep(500);
    OnGameThread([&] { after = ReadField(false); });
    Mark("sus", Text(after));
    if (!Await("sus_done")) return;

    // One hole ends.
    OnGameThread([&] {
        std::vector<void*> v;
        AC::ListHoles(v);
        if (!v.empty()) AC::Destroy(v.back());
    });
    ::Sleep(500);
    OnGameThread([&] { after = ReadField(false); });
    Mark("hole_end", Text(after));
    if (!Await("hole_end_done")) return;

    // The beehive: a random tree inside the map's box, and the body retries by a Delay when the tree is outside it.
    Field branchesBefore, branches;
    OnGameThread([&] { branchesBefore = ReadField(true); branches = branchesBefore; });
    size_t nestsBefore = 0;
    OnGameThread([&] { nestsBefore = CountOf(L"prop_waspnest_bee_C"); });
    for (int i = 0; i < 90 && branches.n == branchesBefore.n; ++i) {
        OnGameThread([&] { RunBeehiveSpawn(); branches = ReadField(true); });
        if (branches.n == branchesBefore.n) ::Sleep(1000);
    }
    Check(branches.n > branchesBefore.n, "the beehive spawner's own body made a branch");
    // A body that found its tree outside the box waits 5 s and runs again, so more may follow the first.
    ::Sleep(7000);
    OnGameThread([&] { branches = ReadField(true); });
    size_t nests = 0;
    OnGameThread([&] { nests = CountOf(L"prop_waspnest_bee_C"); });
    Check(nests > nestsBefore, "the host's branch has its nest");
    Mark("branch", Text(branches));
    Mark("nests", std::to_string(nests));
    // A retry queued by an earlier run of the body can still add a branch and its nest: the counts stay current until the
    // client is done.
    for (int i = 0; i < 3000 && !Seen("branch_done"); ++i) {
        OnGameThread([&] {
            branches = ReadField(true);
            nests = CountOf(L"prop_waspnest_bee_C");
        });
        Mark("branch", Text(branches));
        Mark("nests", std::to_string(nests));
        ::Sleep(100);
    }

    OnGameThread([&] {
        std::vector<void*> v;
        AC::ListBranches(v);
        if (!v.empty()) AC::Destroy(v.back());
    });
    ::Sleep(1000);
    OnGameThread([&] { branches = ReadField(true); nests = CountOf(L"prop_waspnest_bee_C"); });
    Mark("branch_end", Text(branches));
    Mark("nests_end", std::to_string(nests));
    for (int i = 0; i < 3000 && !Seen("branch_end_done"); ++i) {
        OnGameThread([&] {
            branches = ReadField(true);
            nests = CountOf(L"prop_waspnest_bee_C");
        });
        Mark("branch_end", Text(branches));
        Mark("nests_end", std::to_string(nests));
        ::Sleep(100);
    }

    Await("client_done");
    UE_LOGI("ambient-field: DONE on host fails=%d", g_fails);
}

void RunClient() {
    bool ready = false;
    for (int i = 0; i < 480 && !ready; ++i) {
        OnGameThread([&] { ready = coop::net_pump::HasAnnouncedWorldReady(); });
        if (!ready) ::Sleep(500);
    }
    ::Sleep(8000);
    OnGameThread([&] {
        for (const wchar_t* cls : {L"ticker_hexahiveSpawner_C", L"ticker_susHoleSpawner_C"}) {
            void* t = FirstOf(cls);
            UE_LOGI("ambient-field: spawner '%ls' here=%d", cls, t ? 1 : 0);
            // W6 converts this step: the rows of coop/world/spawn_authority refuse the spawners, not a parked tick.
        }
    });

    // W6 converts this step: the beehive spawner's refusal is a row of coop/world/spawn_authority, not a paused timer.
    Mark("client_in");

    // The save's branches stand with their nests: none was swept as this client's own.
    // W6 converts this step: a joiner binds each loaded branch by its key and a census-end ends the ones the host lacks.
    std::string body;
    if (!Await("join_branches", &body)) return;
    ExpectField(body, true, "a joiner has the host's branches, at its places");
    size_t joinBranches = 0, withNest = 0;
    OnGameThread([&] {
        joinBranches = ReadField(true).n;
        withNest = BranchesWithNest();
    });
    UE_LOGI("ambient-field: at join %zu branches, %zu with a live nest", joinBranches, withNest);
    Check(joinBranches > 0 && withNest == joinBranches, "every branch the load brought keeps its nest");

    if (!Await("hive", &body)) return;
    ExpectField(body, false, "a joiner has the hive's field, every hole at the host's place");

    // This client's own rolls do not stand.
    Field had, now;
    OnGameThread([&] { had = ReadField(false); });
    for (int i = 0; i < 100; ++i) {
        OnGameThread([] { RunTick(L"ticker_susHoleSpawner_C"); });
        ::Sleep(10);
    }
    ::Sleep(1500);
    OnGameThread([&] { now = ReadField(false); });
    Check(now.n == had.n, "holes this client rolled itself were swept");
    Mark("hive_done");

    if (!Await("sus", &body)) return;
    ExpectField(body, false, "the host's sus hole stands here at its place");
    Mark("sus_done");
    if (!Await("hole_end", &body)) return;
    ExpectField(body, false, "a hole that ended on the host ended here");
    Mark("hole_end_done");

    ExpectBranchesLive("branch", "nests", "the host's branch stands here at its place", "the nests are one set, the host's");
    // A roll of this client's own.
    Field branchesHad, branchesNow;
    OnGameThread([&] { branchesHad = ReadField(true); RunBeehiveSpawn(); });
    ::Sleep(1500);
    OnGameThread([&] { branchesNow = ReadField(true); });
    Check(branchesNow.n == branchesHad.n, "a branch this client rolled itself was swept");
    Mark("branch_done");

    // W6 converts this step: the nest ends by the prop lane, never with the branch's own end on a client.
    ExpectBranchesLive("branch_end", "nests_end", "a branch that ended on the host ended here", "its nest ended with it");
    Mark("branch_end_done");

    // The lanes' own cost at rest: whole-array walks of the object table, against the census.
    const unsigned long long walksBefore = ue_wrap::walk_census::ArrayWalkCountTotal();
    ::Sleep(20000);
    const unsigned long long walks = ue_wrap::walk_census::ArrayWalkCountTotal() - walksBefore;
    UE_LOGI("ambient-field: whole-array walks in 20 s: %llu (%.1f a second)", walks, static_cast<double>(walks) / 20.0);
    Mark("client_done");
    UE_LOGI("ambient-field: DONE on client fails=%d", g_fails);
}

}  // namespace

DWORD WINAPI AmbientFieldThread(LPVOID /*arg*/) {
    g_dir = coop::config::ReadEnv("VOTVCOOP_CAN_DIR");
    if (g_dir.empty()) {
        UE_LOGW("ambient-field: VERDICT ABORT -- VOTVCOOP_CAN_DIR is unset");
        return 0;
    }
    if (IsClientRole()) RunClient();
    else RunHost();
    return 0;
}

}  // namespace harness::autotest
