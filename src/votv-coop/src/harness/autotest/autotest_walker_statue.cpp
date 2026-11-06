// harness/autotest/autotest_walker_statue.cpp -- TEST-ONLY: a statue turned into a walker by the game's own route, and a real
// fire angering it.
//
// The host forces the mannequin ticker's roll (manndest 1, day 14 at least) and runs its own tick: the ticker destroys a statue,
// puts a broken mannequin prop in its place and spawns a walker at a spawn point nobody sees. The client must lose the statue,
// gain the broken prop and gain the walker. Then the host sets off a game explosion beside that walker: the explosion's fire
// angers it through the walker's own fireDamage, and the client's copy must turn angry. Gated by VOTVCOOP_RUN_WALKER_STATUE; the
// folder is VOTVCOOP_CAN_DIR.

#include "harness/autotest.h"

#include "coop/config/config.h"
#include "coop/player/players_registry.h"
#include "coop/session/net_pump.h"
#include "ue_wrap/actors/ambient_creature.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/world/explosion.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace harness::autotest {
namespace {

namespace AC = ue_wrap::ambient_creature;
namespace E = ue_wrap::engine;
namespace R = ue_wrap::reflection;
namespace OI = ue_wrap::object_index;
namespace X = ue_wrap::explosion;

constexpr int kStatueDay = 14;   // the ticker's walker branch needs daynightcycle.timez.Z at least this

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
    UE_LOGW("walker-statue: VERDICT ABORT -- no '%hs' from the other peer", name);
    return false;
}

void Check(bool ok, const char* what) {
    UE_LOGI("walker-statue: %hs %hs", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fails;
}

std::string Narrow(const std::wstring& w) { return std::string(w.begin(), w.end()); }

float Dist(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Every live instance of exactly `cls`.
std::vector<void*> AllOf(const wchar_t* cls) {
    std::vector<void*> out;
    if (void* c = OI::ClassByName(cls))
        OI::ForEachInstance(c, [](void* ctx, void* obj, int32_t) {
            if (R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__"))
                static_cast<std::vector<void*>*>(ctx)->push_back(obj);
        }, &out);
    return out;
}

struct Statue {
    std::string key;
    ue_wrap::FVector at{};
};

std::vector<Statue> Statues() {
    std::vector<Statue> out;
    for (void* s : AllOf(L"prop_mannequin_C")) {
        Statue st;
        st.key = Narrow(ue_wrap::prop::GetInteractableKeyString(s));
        E::TryGetActorLocation(s, st.at);
        out.push_back(st);
    }
    return out;
}

bool StatueStands(const std::string& key) {
    for (const Statue& s : Statues())
        if (s.key == key) return true;
    return false;
}

// A plain prop_C within `cm` of `at`: the broken mannequin the ticker leaves.
void* BrokenNear(const ue_wrap::FVector& at, float cm) {
    for (void* p : AllOf(L"prop_C")) {
        ue_wrap::FVector pa{};
        if (E::TryGetActorLocation(p, pa) && Dist(pa, at) < cm) return p;
    }
    return nullptr;
}

std::vector<void*> Walkers() {
    std::vector<void*> w;
    AC::ListWalkers(w);
    return w;
}

void* NewWalker(const std::vector<void*>& before) {
    for (void* w : Walkers()) {
        bool old = false;
        for (void* b : before) old = old || b == w;
        if (!old) return w;
    }
    return nullptr;
}

template <class T>
T* Field(void* obj, const wchar_t* name) {
    const int32_t off = obj ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    return off < 0 ? nullptr : reinterpret_cast<T*>(static_cast<uint8_t*>(obj) + off);
}

// The ticker's own tick, with its roll forced and its day gate met. Returns the day it found, or -1.
int RunStatueTick(void* ticker) {
    void** gm = Field<void*>(ticker, L"gamemode");
    if (!gm || !*gm) return -1;
    float* dest = Field<float>(*gm, L"manndest");
    void** cycle = Field<void*>(*gm, L"daynightcycle");
    int32_t* timez = cycle && *cycle ? Field<int32_t>(*cycle, L"timez") : nullptr;
    if (!dest || !timez) return -1;
    const int day = timez[2];
    if (timez[2] < kStatueDay) timez[2] = kStatueDay;
    *dest = 1.f;
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(ticker), L"ReceiveTick");
    if (fn) {
        ue_wrap::ParamFrame f(fn);
        if (f.valid()) {
            f.Set<float>(L"DeltaSeconds", 0.016f);
            ue_wrap::Call(ticker, f);
        }
    }
    return day;
}

void RestoreDay(void* ticker, int day) {
    void** gm = Field<void*>(ticker, L"gamemode");
    void** cycle = gm && *gm ? Field<void*>(*gm, L"daynightcycle") : nullptr;
    int32_t* timez = cycle && *cycle ? Field<int32_t>(*cycle, L"timez") : nullptr;
    if (timez && day >= 0) timez[2] = day;
}

void RunHost() {
    if (!Await("client_ready", nullptr, 480)) return;
    std::vector<Statue> before;
    std::vector<void*> walkersBefore;
    void* ticker = nullptr;
    OnGameThread([&] {
        before = Statues();
        walkersBefore = Walkers();
        const std::vector<void*> t = AllOf(L"ticker_mannequinSpawner_C");
        ticker = t.empty() ? nullptr : t.front();
    });
    UE_LOGI("walker-statue: %zu statue(s), %zu walker(s), ticker %p", before.size(), walkersBefore.size(), ticker);
    Check(!before.empty(), "the world has a statue");
    Check(ticker != nullptr, "the mannequin ticker stands");
    if (before.empty() || !ticker) return;

    int day = -1;
    OnGameThread([&] { day = RunStatueTick(ticker); });
    Check(day >= 0, "the ticker's roll and day were forced");
    UE_LOGI("walker-statue: the ticker ran on day %d", day);

    // The walker comes half a second after the statue goes: the ticker's own Delay.
    Statue gone;
    void* walker = nullptr;
    for (int i = 0; i < 100 && (gone.key.empty() || !walker); ++i) {
        ::Sleep(100);
        OnGameThread([&] {
            if (gone.key.empty())
                for (const Statue& s : before)
                    if (!StatueStands(s.key)) gone = s;
            if (!walker) walker = NewWalker(walkersBefore);
        });
    }
    OnGameThread([&] { RestoreDay(ticker, day); });
    Check(!gone.key.empty(), "a statue went");
    Check(walker != nullptr, "a walker came");
    if (gone.key.empty() || !walker) return;
    void* broken = nullptr;
    ue_wrap::FVector wAt{};
    OnGameThread([&] {
        broken = BrokenNear(gone.at, 150.f);
        E::TryGetActorLocation(walker, wAt);
    });
    Check(broken != nullptr, "a broken mannequin lies where it stood");
    UE_LOGI("walker-statue: statue '%hs' at (%.0f,%.0f,%.0f) went; walker at (%.0f,%.0f,%.0f)", gone.key.c_str(), gone.at.X,
            gone.at.Y, gone.at.Z, wAt.X, wAt.Y, wAt.Z);
    char body[256];
    std::snprintf(body, sizeof(body), "%s %f %f %f %f %f %f", gone.key.c_str(), gone.at.X, gone.at.Y, gone.at.Z, wAt.X, wAt.Y,
                  wAt.Z);
    Mark("statue", body);
    if (!Await("statue_seen", nullptr, 60)) return;

    // A real fire: a game explosion on the walker. explosion_C ignites what it reaches with a chance of (1 - d/r)^3 times
    // fireStrength, so a strength of 10 makes it certain; the burn then deals the walker's own fireDamage, which angers it.
    bool calm = false;
    OnGameThread([&] { calm = !AC::ReadWalkerAngry(walker); });
    Check(calm, "the new walker is calm");
    if (!Await("calm_seen", nullptr, 60)) return;
    bool blown = false;
    OnGameThread([&] {
        ue_wrap::FVector at{};
        if (!X::ResolveNow(X::Cls::Explosion) || !E::TryGetActorLocation(walker, at)) return;
        const float loc[3] = {at.X, at.Y, at.Z};
        const float rot[3] = {0.f, 0.f, 0.f};
        const float scale[3] = {1.f, 1.f, 1.f};
        X::Params p{};
        p.damage = 5.f;
        p.radius = 300.f;
        p.force = 0.f;
        p.fireStrength = 10.f;
        p.fireFuel = 20.f;
        blown = X::SpawnDeferred(X::Cls::Explosion, loc, rot, scale, p, nullptr) != nullptr;
    });
    Check(blown, "an explosion went off on the walker");
    bool angry = false;
    for (int i = 0; i < 150 && !angry; ++i) {
        ::Sleep(100);
        OnGameThread([&] { angry = R::IsLive(walker) && AC::ReadWalkerAngry(walker); });
    }
    Check(angry, "the fire angered the host's walker");
    Mark("fired");
    Await("client_done", nullptr, 60);
    UE_LOGI("walker-statue: DONE on host fails=%d", g_fails);
}

void RunClient() {
    bool ready = false;
    for (int i = 0; i < 480 && !ready; ++i) {
        OnGameThread([&] { ready = coop::players::Registry::Get().Local() && coop::net_pump::HasAnnouncedWorldReady(); });
        if (!ready) ::Sleep(500);
    }
    ::Sleep(8000);
    std::vector<void*> walkersBefore;
    OnGameThread([&] { walkersBefore = Walkers(); });
    Mark("client_ready");
    std::string body;
    if (!Await("statue", &body, 120)) return;
    char key[128] = {};
    Statue gone;
    ue_wrap::FVector hostWalker{};
    std::sscanf(body.c_str(), "%127s %f %f %f %f %f %f", key, &gone.at.X, &gone.at.Y, &gone.at.Z, &hostWalker.X, &hostWalker.Y,
                &hostWalker.Z);
    gone.key = key;

    bool statueGone = false;
    void* broken = nullptr;
    void* walker = nullptr;
    for (int i = 0; i < 100 && !(statueGone && broken && walker); ++i) {
        OnGameThread([&] {
            statueGone = !StatueStands(gone.key);
            if (!broken) broken = BrokenNear(gone.at, 150.f);
            if (!walker) walker = NewWalker(walkersBefore);
        });
        if (!(statueGone && broken && walker)) ::Sleep(100);
    }
    Check(statueGone, "the host's statue went here too");
    Check(broken != nullptr, "the broken mannequin lies here too");
    Check(walker != nullptr, "the walker crossed");
    if (walker) {
        ue_wrap::FVector at{};
        OnGameThread([&] { E::TryGetActorLocation(walker, at); });
        UE_LOGI("walker-statue: walker copy at (%.0f,%.0f,%.0f), %.0f cm from the host's", at.X, at.Y, at.Z, Dist(at, hostWalker));
        Check(Dist(at, hostWalker) < 1500.f, "the walker came where the host's did");
    }
    Mark("statue_seen");
    if (!walker) return;

    bool calm = false;
    OnGameThread([&] { calm = !AC::ReadWalkerAngry(walker); });
    Check(calm, "the walker's copy is calm");
    Mark("calm_seen");
    if (!Await("fired", nullptr, 60)) return;
    bool angry = false;
    for (int i = 0; i < 100 && !angry; ++i) {
        OnGameThread([&] { angry = R::IsLive(walker) && AC::ReadWalkerAngry(walker); });
        if (!angry) ::Sleep(100);
    }
    Check(angry, "the host's fire angered this copy");
    Mark("client_done");
    UE_LOGI("walker-statue: DONE on client fails=%d", g_fails);
}

}  // namespace

DWORD WINAPI WalkerStatueThread(LPVOID /*arg*/) {
    g_dir = coop::config::ReadEnv("VOTVCOOP_CAN_DIR");
    if (g_dir.empty()) {
        UE_LOGW("walker-statue: VERDICT ABORT -- VOTVCOOP_CAN_DIR is not set");
        return 0;
    }
    if (IsClientRole()) RunClient();
    else RunHost();
    return 0;
}

}  // namespace harness::autotest
