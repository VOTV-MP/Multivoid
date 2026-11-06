// harness/autotest/autotest_ground_hose.cpp -- TEST-ONLY: a garden hose laid by each peer, run, cut and taken up.
//
// Each peer has a faucet under one Key (the save holds none, and a spawned one does not cross); the host drops two
// sprinklers, which cross as props. The host lays a hose from the faucet to the first: the client's copy must be laid,
// tied to its own faucet and sprinkler. The host runs the faucet: the client's sprinkler must spray. The client lays a
// second hose to the second sprinkler, its tick run first as the coil's would: the host must lay it, tied. The client
// cuts it from its sprinkler: the host's must be cut. The client takes up the first: the host's must go. A hose is
// laid here by its record, as the game's own load lays one, not walked out. Gated by VOTVCOOP_RUN_GROUND_HOSE; the
// folder is VOTVCOOP_CAN_DIR.

#include "harness/autotest.h"

#include "coop/config/config.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_key_index.h"
#include "coop/session/net_pump.h"
#include "ue_wrap/actors/ground_hose.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/devices/appliance.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <string>

namespace harness::autotest {
namespace {

namespace A = ue_wrap::appliance;
namespace E = ue_wrap::engine;
namespace H = ue_wrap::ground_hose;
namespace R = ue_wrap::reflection;
using SaveRecord = ue_wrap::save_record::SaveRecord;

constexpr const wchar_t* kFaucetPath = L"/Game/objects/faucet.faucet_C";
constexpr const wchar_t* kSprinklerPath = L"/Game/objects/prop_sprinkler.prop_sprinkler_C";
constexpr const wchar_t* kCoilPath = L"/Game/objects/prop_hose.prop_hose_C";
constexpr const wchar_t* kHostHose = L"mvdrill_hose_host";
constexpr const wchar_t* kClientHose = L"mvdrill_hose_client";

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
    UE_LOGW("ground-hose: VERDICT ABORT -- no '%hs' from the other peer", name);
    return false;
}

void Check(bool ok, const char* what) {
    UE_LOGI("ground-hose: %hs %hs", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fails;
}

std::string Narrow(const std::wstring& w) { return std::string(w.begin(), w.end()); }
std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

// `pred` on the game thread, every 100 ms until it holds or `seconds` pass.
bool WaitFor(const std::function<bool()>& pred, int seconds = 10) {
    for (int i = 0; i < seconds * 10; ++i) {
        bool ok = false;
        OnGameThread([&] { ok = pred(); });
        if (ok) return true;
        ::Sleep(100);
    }
    return false;
}

void* HoseByKey(const std::wstring& key) {
    std::vector<void*> hoses;
    H::ListHoses(hoses);
    for (void* h : hoses)
        if (H::KeyOf(h) == key) return h;
    return nullptr;
}

bool ReadFlag(void* obj, const wchar_t* name, bool& out) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!obj || !R::FindBoolProperty(R::ClassOf(obj), name, off, mask) || off < 0 || !mask) return false;
    out = (static_cast<const uint8_t*>(obj)[off] & mask) != 0;
    return true;
}

bool Sprays(void* sprinkler) {
    bool on = false;
    return ReadFlag(sprinkler, L"active", on) && on;
}

ue_wrap::FVector Near(float dx, float dy) {
    void* pawn = coop::players::Registry::Get().Local();
    ue_wrap::FVector me{};
    if (pawn) E::TryGetActorLocation(pawn, me);
    return {me.X + dx, me.Y + dy, me.Z};
}

// A faucet before this player, under `key` when one is given. Returns its Key.
std::wstring SpawnFaucet(const std::wstring& key) {
    void* cls = ue_wrap::asset_load::LoadObjectByPath(kFaucetPath);
    void* made = cls ? E::SpawnActor(cls, Near(100.f, 0.f)) : nullptr;
    if (!made) return {};
    if (!key.empty()) {
        const int32_t off = R::FindPropertyOffset(R::ClassOf(made), L"Key");
        const R::FName name = ue_wrap::fname_utils::StringToFName(key.c_str());
        if (off >= 0) std::memcpy(static_cast<uint8_t*>(made) + off, &name, sizeof(name));
    }
    A::EnsureResolved();
    return A::GetKeyString(made);
}

std::wstring SpawnSprinkler(float dy) {
    void* cls = ue_wrap::asset_load::LoadObjectByPath(kSprinklerPath);
    void* made = cls ? E::SpawnActor(cls, Near(250.f, dy)) : nullptr;
    return made ? ue_wrap::prop::GetInteractableKeyString(made) : std::wstring();
}

bool HoseLoaded() {
    if (H::HoseClass()) return true;
    ue_wrap::asset_load::LoadObjectByPath(kCoilPath);
    return H::HoseClass() != nullptr;
}

// A hose from the faucet to the sprinkler under `key`, laid by its record. `laying` first runs its tick, as a coil's hose
// laying behind its player does.
void* LayHose(const std::wstring& key, void* faucet, void* sprinkler, bool laying) {
    ue_wrap::FVector f{}, s{};
    if (!HoseLoaded() || !faucet || !sprinkler || !E::TryGetActorLocation(faucet, f) || !E::TryGetActorLocation(sprinkler, s))
        return nullptr;
    void* hose = H::Spawn(f);
    if (!hose) return nullptr;
    if (laying) {
        E::SetActorTickEnabled(hose, true);
        return hose;
    }
    SaveRecord rec;
    if (!H::Capture(hose, rec)) return nullptr;
    rec.key = key;
    rec.vectors = {{{f.X, f.Y, f.Z - 70.f}, {(f.X + s.X) / 2, (f.Y + s.Y) / 2, s.Z}, {s.X, s.Y, s.Z}}};
    rec.names = {{A::GetKeyString(faucet), ue_wrap::prop::GetInteractableKeyString(sprinkler)}};
    return H::Apply(hose, rec, faucet, sprinkler) ? hose : nullptr;
}

void* Faucet(const std::wstring& key) { return H::FindFaucet(key); }
void* Prop(const std::wstring& key) { return coop::prop_key_index::Find(key); }

void RunHost() {
    if (!Await("client_ready", nullptr, 480)) return;
    std::wstring fk, s1, s2;
    OnGameThread([&] {
        fk = SpawnFaucet(L"");
        s1 = SpawnSprinkler(-80.f);
        s2 = SpawnSprinkler(80.f);
    });
    Check(!fk.empty() && fk != L"None" && !s1.empty() && !s2.empty(), "a faucet and two sprinklers stand");
    if (fk.empty() || s1.empty() || s2.empty()) return;
    Mark("ends", Narrow(fk) + "\n" + Narrow(s1) + "\n" + Narrow(s2));
    if (!Await("ends_seen", nullptr, 60)) return;

    bool laid = false;
    OnGameThread([&] { laid = LayHose(kHostHose, Faucet(fk), Prop(s1), false) != nullptr; });
    Check(laid && WaitFor([&] {
              void* h = HoseByKey(kHostHose);
              return h && H::FaucetOf(h) == Faucet(fk) && H::SprinklerOf(h) == Prop(s1);
          }),
          "the host's hose is laid, tied");
    Mark("h1");
    if (!Await("h1_seen", nullptr, 30)) return;

    OnGameThread([&] { A::CallAction(Faucet(fk), coop::players::Registry::Get().Local(), A::kTapToggleAction); });
    Check(WaitFor([&] { return Sprays(Prop(s1)); }), "the host's faucet runs its sprinkler");
    Mark("on");
    if (!Await("on_seen", nullptr, 30)) return;

    if (!Await("h2", nullptr, 60)) return;
    Check(WaitFor([&] {
              void* h = HoseByKey(kClientHose);
              return h && H::FaucetOf(h) == Faucet(fk) && H::SprinklerOf(h) == Prop(s2);
          }),
          "the client's hose is laid here, tied");
    Check(WaitFor([&] { return Sprays(Prop(s2)); }), "the client's hose runs its sprinkler here");
    Mark("h2_seen");

    if (!Await("cut", nullptr, 30)) return;
    Check(WaitFor([&] {
              void* h = HoseByKey(kClientHose);
              return h && !H::SprinklerOf(h) && !Sprays(Prop(s2));
          }),
          "the client's cut is made here");
    Mark("cut_seen");

    if (!Await("took", nullptr, 30)) return;
    Check(WaitFor([&] { return HoseByKey(kHostHose) == nullptr; }), "the client took up the host's hose here");
    Mark("took_seen");
    Await("client_done", nullptr, 30);
    OnGameThread([&] {
        if (void* h = HoseByKey(kClientHose)) E::DestroyActor(h);
        for (const std::wstring& k : {s1, s2})
            if (void* p = Prop(k)) E::DestroyActor(p);
        if (void* f = Faucet(fk)) E::DestroyActor(f);
    });
    UE_LOGI("ground-hose: DONE on host fails=%d", g_fails);
}

void RunClient() {
    bool ready = false;
    for (int i = 0; i < 480 && !ready; ++i) {
        OnGameThread([&] { ready = coop::players::Registry::Get().Local() && coop::net_pump::HasAnnouncedWorldReady(); });
        if (!ready) ::Sleep(500);
    }
    ::Sleep(8000);
    OnGameThread([] { HoseLoaded(); });
    Mark("client_ready");
    std::string ends;
    if (!Await("ends", &ends, 120)) return;
    const size_t a = ends.find('\n'), b = ends.find('\n', a + 1);
    const std::wstring fk = Widen(ends.substr(0, a)), s1 = Widen(ends.substr(a + 1, b - a - 1)), s2 = Widen(ends.substr(b + 1));
    std::wstring mine;
    OnGameThread([&] { mine = SpawnFaucet(fk); });
    Check(mine == fk, "this faucet takes the host's key");
    Check(WaitFor([&] { return Prop(s1) && Prop(s2); }), "the host's sprinklers crossed");
    ::Sleep(4000);   // the interactable lane lists a new faucet on its scan
    Mark("ends_seen");

    if (!Await("h1", nullptr, 30)) return;
    Check(WaitFor([&] {
              void* h = HoseByKey(kHostHose);
              return h && H::FaucetOf(h) == Faucet(fk) && H::SprinklerOf(h) == Prop(s1) && !H::IsLaying(h);
          }),
          "the host's hose is laid here, tied");
    Mark("h1_seen");

    if (!Await("on", nullptr, 30)) return;
    Check(WaitFor([&] { return Sprays(Prop(s1)); }), "the host's faucet runs the sprinkler here");
    Mark("on_seen");

    void* hose = nullptr;
    OnGameThread([&] { hose = LayHose(kClientHose, Faucet(fk), Prop(s2), true); });
    ::Sleep(1200);   // the lane sees it laying
    bool laid = false;
    OnGameThread([&] {
        void* f = Faucet(fk);
        void* s = Prop(s2);
        ue_wrap::FVector fa{}, sa{};
        SaveRecord rec;
        if (!hose || !R::IsLive(hose) || !f || !s || !E::TryGetActorLocation(f, fa) || !E::TryGetActorLocation(s, sa) ||
            !H::Capture(hose, rec))
            return;
        rec.key = kClientHose;
        rec.vectors = {{{fa.X, fa.Y, fa.Z - 70.f}, {(fa.X + sa.X) / 2, (fa.Y + sa.Y) / 2, sa.Z}, {sa.X, sa.Y, sa.Z}}};
        rec.names = {{fk, s2}};
        laid = H::Apply(hose, rec, f, s);
    });
    Check(laid, "this player's hose is laid and finished");
    Mark("h2");
    if (!Await("h2_seen", nullptr, 30)) return;

    OnGameThread([&] {
        if (void* h = HoseByKey(kClientHose)) H::CallDetachSprinkler(h);
    });
    Check(WaitFor([&] {
              void* h = HoseByKey(kClientHose);
              return h && !H::SprinklerOf(h);
          }, 3),
          "this player cut its hose from the sprinkler");
    Mark("cut");
    if (!Await("cut_seen", nullptr, 30)) return;

    OnGameThread([&] {
        if (void* h = HoseByKey(kHostHose)) E::DestroyActor(h);
    });
    Mark("took");
    if (!Await("took_seen", nullptr, 30)) return;
    ::Sleep(1500);
    Check(WaitFor([&] { return HoseByKey(kHostHose) == nullptr; }, 2), "the host's hose stays taken up here");
    Mark("client_done");
    UE_LOGI("ground-hose: DONE on client fails=%d", g_fails);
}

}  // namespace

DWORD WINAPI GroundHoseThread(LPVOID /*arg*/) {
    g_dir = coop::config::ReadEnv("VOTVCOOP_CAN_DIR");
    if (g_dir.empty()) {
        UE_LOGW("ground-hose: VERDICT ABORT -- VOTVCOOP_CAN_DIR is not set");
        return 0;
    }
    if (IsClientRole()) RunClient();
    else RunHost();
    return 0;
}

}  // namespace harness::autotest
