// harness/autotest/autotest_world_sound.cpp -- TEST-ONLY: the earth teleport's sounds and a faucet's click across two peers.
//
// The client presses a faucet and the host presses it back, each checked on the other as the faucet's state, and then the
// host runs the sky's own tp, whose two sounds are about 86 s apart. The driver reads each peer's `world_sound:` lines,
// since what a peer heard cannot be read back. Gated by VOTVCOOP_RUN_WORLD_SOUND; the folder is VOTVCOOP_CAN_DIR.

#include "harness/autotest.h"

#include "coop/config/config.h"
#include "coop/player/players_registry.h"
#include "coop/session/net_pump.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/prop_use.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/component_calls.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/devices/appliance.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <fstream>
#include <string>

namespace harness::autotest {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;
namespace A = ue_wrap::appliance;

constexpr uint8_t kActionFaucet = 5;  // the faucet's use action, which its click follows
constexpr int     kTeleportSeconds = 100;

std::string g_dir;
int g_fails = 0;

template <class Fn>
void OnGameThread(Fn&& fn) {
    ue_wrap::game_thread::RunAndWait([&fn](std::atomic<int>& d) { fn(); d.store(1); });
}

void Mark(const char* name) { std::ofstream(g_dir + "\\" + name, std::ios::binary | std::ios::trunc) << "x"; }

bool Await(const char* name, int seconds = 240) {
    for (int i = 0; i < seconds * 10; ++i) {
        std::ifstream f(g_dir + "\\" + name, std::ios::binary);
        std::string s;
        if (f && std::getline(f, s) && !s.empty()) return true;
        ::Sleep(100);
    }
    UE_LOGW("sound-drill: VERDICT ABORT -- no '%hs' from the other peer", name);
    return false;
}

void Verdict(const char* step, bool pass, const std::string& detail) {
    if (!pass) ++g_fails;
    UE_LOGI("sound-drill: %hs %hs %hs", step, pass ? "PASS" : "FAIL", detail.c_str());
}

// The one keyed faucet both peers' sorted keys agree on.
void* FirstFaucet() {
    struct Best {
        void* actor = nullptr;
        std::wstring key;
    } best;
    A::EnsureResolved();
    void* const cls = ue_wrap::object_index::ClassByName(L"faucet_C");
    if (!cls) return nullptr;
    ue_wrap::object_index::ForEachInstance(
        cls,
        [](void* ctx, void* obj, int32_t) {
            auto* b = static_cast<Best*>(ctx);
            const std::wstring key = A::GetKeyString(obj);
            if (R::IsLive(obj) && !key.empty() && key != L"None" && (!b->actor || key < b->key)) {
                b->actor = obj;
                b->key = key;
            }
        },
        &best);
    return best.actor;
}

bool FaucetOn(bool& on) {
    bool read = false;
    OnGameThread([&] { read = A::TryReadState(FirstFaucet(), on); });
    return read;
}

void PressFaucet() {
    OnGameThread([&] {
        void* faucet = FirstFaucet();
        void* local = coop::players::Registry::Get().Local();
        ue_wrap::FVector at{};
        if (!faucet || !local || !E::TryGetActorLocation(faucet, at)) {
            UE_LOGW("sound-drill: no faucet to press");
            return;
        }
        void* mesh = ue_wrap::prop::GetStaticMesh(faucet);
        ue_wrap::prop_use::CallActionOptionIndex(faucet, local, mesh, at, kActionFaucet, mesh);
    });
}

void CheckFaucet(const char* step, bool want) {
    bool on = !want, read = false;
    for (int i = 0; i < 24 && !(read && on == want); ++i) {
        read = FaucetOn(on);
        if (!(read && on == want)) ::Sleep(250);
    }
    Verdict(step, read && on == want, read ? "active=" + std::to_string(on) : std::string("no faucet"));
}

// The save holds no faucet and a spawned one does not cross, so each peer spawns one under the same Key, as a saved faucet
// would have it. Returns the Key.
std::string SpawnFaucet(const std::string& wantKey) {
    std::string out;
    OnGameThread([&] {
        void* cls = ue_wrap::asset_load::LoadObjectByPath(L"/Game/objects/faucet.faucet_C");
        void* local = coop::players::Registry::Get().Local();
        ue_wrap::FVector me{};
        void* made = cls && local && E::TryGetActorLocation(local, me) ? E::SpawnActor(cls, {me.X + 100.f, me.Y, me.Z}) : nullptr;
        if (!made) return;
        if (!wantKey.empty()) {
            const int32_t off = R::FindPropertyOffset(R::ClassOf(made), L"Key");
            const std::wstring wide(wantKey.begin(), wantKey.end());
            const R::FName name = ue_wrap::fname_utils::StringToFName(wide.c_str());
            if (off >= 0) std::memcpy(static_cast<uint8_t*>(made) + off, &name, sizeof(name));
        }
        const std::wstring key = A::GetKeyString(made);
        out.assign(key.begin(), key.end());
    });
    return out;
}

void RunHost() {
    if (!Await("client_ready")) return;
    const std::string key = SpawnFaucet("");
    UE_LOGI("sound-drill: host faucet key='%hs'", key.c_str());
    std::ofstream(g_dir + "\\faucet_key", std::ios::binary | std::ios::trunc) << key;
    if (!Await("client_faucet")) return;
    ::Sleep(4000);  // each peer's lane lists a new faucet on its scan
    bool start = false;
    if (!FaucetOn(start)) {
        size_t found = 0;
        OnGameThread([&] {
            void* cls = ue_wrap::object_index::ClassByName(L"faucet_C");
            found = cls ? ue_wrap::object_index::ForEachInstance(cls, [](void*, void*, int32_t) {}, nullptr) : 0;
        });
        Verdict("00-a-faucet-to-press", false, std::to_string(found) + " faucet_C with no readable state and key");
        Mark("no_faucet");
    } else {
        Mark(start ? "start_on" : "start_off");
        if (!Await("c1")) return;
        CheckFaucet("01-client-presses", !start);
        Mark("h1");
        PressFaucet();
        CheckFaucet("02-host-presses", start);
        Mark("h2");
        if (!Await("c2")) return;
    }
    OnGameThread([&] {
        void* sky = R::FindObjectByClass(L"newsky_C");
        const bool ran = sky && ue_wrap::component_calls::CallParamlessNamed(sky, L"tp");
        UE_LOGI("sound-drill: the sky's tp ran=%d", ran ? 1 : 0);
    });
    ::Sleep(kTeleportSeconds * 1000);
    Mark("h3");
    if (!Await("c3")) return;
    UE_LOGI("sound-drill: DONE on host fails=%d", g_fails);
}

void RunClient() {
    bool ready = false;
    for (int i = 0; i < 480 && !ready; ++i) {
        OnGameThread([&] { ready = coop::net_pump::HasAnnouncedWorldReady(); });
        if (!ready) ::Sleep(500);
    }
    ::Sleep(8000);
    Mark("client_ready");
    std::string key;
    for (int i = 0; i < 1200 && key.empty(); ++i) {
        std::ifstream f(g_dir + "\\faucet_key", std::ios::binary);
        if (f) std::getline(f, key);
        if (key.empty()) ::Sleep(100);
    }
    UE_LOGI("sound-drill: client faucet key='%hs'", SpawnFaucet(key).c_str());
    Mark("client_faucet");
    bool start = false, faucet = true;
    for (int i = 0; i < 2400; ++i) {
        std::ifstream on(g_dir + "\\start_on"), off(g_dir + "\\start_off"), none(g_dir + "\\no_faucet");
        if (none) {
            faucet = false;
            break;
        }
        if (on || off) {
            start = static_cast<bool>(on);
            break;
        }
        ::Sleep(100);
    }
    if (faucet) {
        ::Sleep(4000);
        PressFaucet();
        CheckFaucet("01-client-presses", !start);
        Mark("c1");
        if (!Await("h1")) return;
        if (!Await("h2")) return;
        CheckFaucet("02-host-presses", start);
        Mark("c2");
    }
    if (!Await("h3", 240)) return;
    UE_LOGI("sound-drill: DONE on client fails=%d", g_fails);
    Mark("c3");
}

}  // namespace

DWORD WINAPI WorldSoundThread(LPVOID /*arg*/) {
    g_dir = coop::config::ReadEnv("VOTVCOOP_CAN_DIR");
    if (g_dir.empty()) {
        UE_LOGW("sound-drill: VERDICT ABORT -- VOTVCOOP_CAN_DIR is unset");
        return 0;
    }
    if (IsClientRole()) RunClient();
    else RunHost();
    return 0;
}

}  // namespace harness::autotest
