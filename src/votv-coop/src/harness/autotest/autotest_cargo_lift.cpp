// harness/autotest/autotest_cargo_lift.cpp -- TEST-ONLY: the cargo lift (the base's elevator), as each of two peers sees
// it.
//
// The host lists every cargoLift_C. Then the host presses the first one's call button (its own actionOptionIndex,
// action 4, as a player's use runs it) and, once it has stopped, the client presses it; each presser stands by the lift. After each press each peer
// logs `cargo-lift: STATE <step> t=<s>` once a second for 20 s: the lift's `dir`, `moving` and `alpha`, the height of
// its `attach` point and of its platform (the `lift` child actor) over the lift's root. `cargo-lift: CHECK` lines judge
// that both peers end each press with the same `dir` and the platform at the same height, and `cargo-lift: VERDICT`
// closes each peer's run. Gated by VOTVCOOP_RUN_CARGO_LIFT; the folder is VOTVCOOP_CAN_DIR.

#include "harness/autotest.h"

#include "coop/config/config.h"
#include "coop/player/players_registry.h"
#include "coop/session/net_pump.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_component.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace harness::autotest {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

std::string g_dir;
void* g_lift = nullptr;   // game thread only
int g_checks = 0, g_failed = 0;

template <class Fn>
void OnGameThread(Fn&& fn) {
    ue_wrap::game_thread::RunAndWait([&fn](std::atomic<int>& d) { fn(); d.store(1); });
}

const char* Who() { return IsClientRole() ? "client" : "host"; }

void Write(const std::string& name, const std::string& text) {
    std::ofstream(g_dir + "\\" + name, std::ios::binary | std::ios::trunc) << text;
}

bool Await(const std::string& name, std::string* text = nullptr, int seconds = 300) {
    for (int i = 0; i < seconds * 10; ++i) {
        std::ifstream f(g_dir + "\\" + name, std::ios::binary);
        std::string s;
        if (f && std::getline(f, s) && !s.empty()) {
            if (text) *text = s;
            return true;
        }
        ::Sleep(100);
    }
    UE_LOGW("cargo-lift: VERDICT ABORT -- no '%hs' from the other peer", name.c_str());
    return false;
}

template <class T>
bool Field(void* obj, const wchar_t* name, T& out) {
    const int32_t off = obj && R::IsLive(obj) ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    if (off < 0) return false;
    std::memcpy(&out, static_cast<const uint8_t*>(obj) + off, sizeof(T));
    return true;
}

void* Obj(void* obj, const wchar_t* name) {
    void* o = nullptr;
    Field(obj, name, o);
    return o && R::IsLive(o) ? o : nullptr;
}

std::vector<void*> Lifts() {
    std::vector<void*> out;
    for (void* o : R::FindObjectsByClass(L"cargoLift_C"))
        if (o && R::IsLive(o) && !R::NameStartsWith(R::NameOf(o), L"Default__")) out.push_back(o);
    return out;
}

struct Seen {
    bool dir = false, moving = false;
    float alpha = -1.f, attachZ = 0.f, platformZ = 0.f;
};

Seen Read() {
    Seen v;
    OnGameThread([&] {
        if (!g_lift || !R::IsLive(g_lift)) return;
        Field(g_lift, L"dir", v.dir);
        Field(g_lift, L"moving", v.moving);
        Field(g_lift, L"alpha", v.alpha);
        ue_wrap::FVector root{}, at{};
        E::TryGetActorLocation(g_lift, root);
        if (void* a = Obj(g_lift, L"attach"); a && E::TryGetComponentLocation(a, at)) v.attachZ = at.Z - root.Z;
        if (void* c = Obj(g_lift, L"lift")) {
            if (void* p = Obj(c, L"ChildActor"); p && E::TryGetActorLocation(p, at)) v.platformZ = at.Z - root.Z;
        }
    });
    return v;
}

void Watch(const std::string& step) {
    for (int t = 0; t <= 20; ++t) {
        const Seen v = Read();
        UE_LOGI("cargo-lift: STATE %hs %hs t=%d dir=%d moving=%d alpha=%.2f attachZ=%.0f platformZ=%.0f", step.c_str(),
                Who(), t, v.dir ? 1 : 0, v.moving ? 1 : 0, v.alpha, v.attachZ, v.platformZ);
        ::Sleep(1000);
    }
}

// The presser 200 cm from the lift's root, 100 cm above it, through the player's own teleportWObackrooms: a press is
// judged against the presser's reach.
void ComeNear() {
    OnGameThread([] {
        void* me = coop::players::Registry::Get().Local();
        ue_wrap::FVector at{};
        if (!me || !E::TryGetActorLocation(g_lift, at)) return;
        void* f = R::FindFunction(R::ClassOf(me), L"teleportWObackrooms");
        ue_wrap::ParamFrame pf(f);
        ue_wrap::FTransform t{};
        t.TX = at.X + 200.f; t.TY = at.Y; t.TZ = at.Z + 100.f;
        if (f && pf.valid() && pf.Set<ue_wrap::FTransform>(L"newTransform", t)) ue_wrap::Call(me, pf);
    });
    ::Sleep(2000);
}

void Press() {
    ComeNear();
    OnGameThread([] {
        void* f = R::FindFunction(R::ClassOf(g_lift), L"actionOptionIndex");
        ue_wrap::ParamFrame pf(f);
        const bool ok = f && pf.valid() && pf.Set<uint8_t>(L"action", uint8_t{4}) && ue_wrap::Call(g_lift, pf);
        UE_LOGI("cargo-lift: %hs pressed the lift's button (ok=%d)", Who(), ok ? 1 : 0);
    });
}

// One press: the presser presses, both watch 20 s, then each writes what it ended with and judges against the other.
void Step(bool hostPresses, const std::string& name) {
    const bool mine = hostPresses != IsClientRole();
    if (mine) {
        Press();
        Write(name + "_pressed", "x");
    } else if (!Await(name + "_pressed")) {
        return;
    }
    Watch(name);
    const Seen v = Read();
    char mineText[64];
    std::snprintf(mineText, sizeof(mineText), "%d %.0f", v.dir ? 1 : 0, v.platformZ);
    Write(name + "_" + Who(), mineText);
    std::string other;
    if (!Await(name + "_" + (IsClientRole() ? "host" : "client"), &other)) return;
    int odir = -1;
    float oz = 0.f;
    std::sscanf(other.c_str(), "%d %f", &odir, &oz);
    ++g_checks;
    const bool ok = odir == (v.dir ? 1 : 0) && std::fabs(oz - v.platformZ) < 20.f;
    if (!ok) ++g_failed;
    UE_LOGI("cargo-lift: CHECK %hs %hs %hs -- this peer dir=%d platform %.0f cm, the other dir=%d platform %.0f cm",
            name.c_str(), Who(), ok ? "PASS" : "FAIL", v.dir ? 1 : 0, v.platformZ, odir, oz);
}

void Run() {
    if (IsClientRole()) {
        bool ready = false;
        for (int i = 0; i < 480 && !ready; ++i) {
            OnGameThread([&] { ready = coop::net_pump::HasAnnouncedWorldReady(); });
            if (!ready) ::Sleep(500);
        }
        ::Sleep(10000);
        Write("client_ready", "x");
    } else {
        ::Sleep(20000);
        if (!Await("client_ready")) return;
    }
    OnGameThread([] {
        const std::vector<void*> lifts = Lifts();
        for (void* l : lifts) {
            ue_wrap::FVector at{};
            E::TryGetActorLocation(l, at);
            UE_LOGI("cargo-lift: %hs found %ls at (%.0f,%.0f,%.0f)", Who(), R::ToString(R::NameOf(l)).c_str(), at.X, at.Y,
                    at.Z);
        }
        g_lift = lifts.empty() ? nullptr : lifts.front();
    });
    if (!g_lift) {
        UE_LOGW("cargo-lift: VERDICT ABORT -- %hs has no cargo lift", Who());
        return;
    }
    Watch("0-start");
    Step(true, "1-host-press");
    Step(false, "2-client-press");
    UE_LOGI("cargo-lift: VERDICT %hs %hs -- %d of %d checks failed", Who(), g_failed == 0 ? "PASS" : "FAIL", g_failed,
            g_checks);
    UE_LOGI("cargo-lift: DONE");
}

}  // namespace

DWORD WINAPI CargoLiftThread(LPVOID /*arg*/) {
    g_dir = coop::config::ReadEnv("VOTVCOOP_CAN_DIR");
    if (g_dir.empty()) {
        UE_LOGW("cargo-lift: VERDICT ABORT -- VOTVCOOP_CAN_DIR is unset");
        return 0;
    }
    Run();
    return 0;
}

}  // namespace harness::autotest
