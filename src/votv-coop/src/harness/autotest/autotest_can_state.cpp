// harness/autotest/autotest_can_state.cpp -- TEST-ONLY: a canned food's lid and contents across two peers.
//
// The host spawns two cans and names them in a folder both peers read. Can A: the host runs Open, the client runs eat.
// Can B: the client runs Open, the host runs eat. Each step is one peer's reflected call of the can's own verb, and
// each peer then logs `can-drill: STATE <step> <can> opened= foods= mesh=` for the driver to compare; a file per step
// orders the two peers. Gated by VOTVCOOP_RUN_CAN_STATE; the folder is VOTVCOOP_CAN_DIR.

#include "harness/autotest.h"

#include "coop/config/config.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_key_index.h"
#include "coop/session/net_pump.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <atomic>
#include <fstream>
#include <string>

namespace harness::autotest {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

std::string g_dir;

template <class Fn>
void OnGameThread(Fn&& fn) {
    ue_wrap::game_thread::RunAndWait([&fn](std::atomic<int>& d) { fn(); d.store(1); });
}

void Mark(const char* name, const std::string& body = "x") {
    std::ofstream(g_dir + "\\" + name, std::ios::binary | std::ios::trunc) << body;
}

bool Await(const char* name, std::string* body = nullptr, int seconds = 240) {
    for (int i = 0; i < seconds * 10; ++i) {
        std::ifstream f(g_dir + "\\" + name, std::ios::binary);
        std::string s;
        if (f && std::getline(f, s) && !s.empty()) {
            if (body) *body = s;
            return true;
        }
        ::Sleep(100);
    }
    UE_LOGW("can-drill: VERDICT ABORT -- no '%hs' from the other peer", name);
    return false;
}

void* Find(const std::string& key) {
    void* a = nullptr;
    const std::wstring w(key.begin(), key.end());
    OnGameThread([&] { a = coop::prop_key_index::Find(w); });
    return a;
}

void LogState(const char* step, const char* can, const std::string& key) {
    OnGameThread([&] {
        const std::wstring w(key.begin(), key.end());
        void* a = coop::prop_key_index::Find(w);
        if (!a) {
            UE_LOGW("can-drill: STATE %hs %hs -- no such can here", step, can);
            return;
        }
        const auto* base = static_cast<const uint8_t*>(a);
        const int32_t openedOff = R::FindPropertyOffset(R::ClassOf(a), L"opened");
        const int32_t foodsOff = R::FindPropertyOffset(R::ClassOf(a), L"foods");
        const int32_t typeOff = R::FindPropertyOffset(R::ClassOf(a), L"Type");
        void* mesh = ue_wrap::prop::GetStaticMesh(a);
        void* asset = nullptr;
        if (mesh) {
            const int32_t off = R::FindPropertyOffset(R::ClassOf(mesh), L"StaticMesh");
            if (off >= 0) asset = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(mesh) + off);
        }
        UE_LOGI("can-drill: STATE %hs %hs opened=%d foods=%d type=%d mesh=%ls", step, can, base[openedOff] ? 1 : 0,
                *reinterpret_cast<const int32_t*>(base + foodsOff), *reinterpret_cast<const int32_t*>(base + typeOff),
                asset ? R::ToString(R::NameOf(asset)).c_str() : L"?");
    });
}

// The can's own verb by reflection, with this peer's player where the verb takes one.
void Verb(const std::string& key, const wchar_t* name) {
    void* a = Find(key);
    OnGameThread([&] {
        void* fn = a ? R::FindDispatchFunctionCached(R::ClassOf(a), name) : nullptr;
        if (!fn) {
            UE_LOGW("can-drill: %ls did not resolve", name);
            return;
        }
        ue_wrap::ParamFrame f(fn);
        if (R::FindParamOffset(fn, L"player") >= 0) f.Set<void*>(L"player", coop::players::Registry::Get().Local());
        const bool ok = ue_wrap::Call(a, f);
        UE_LOGI("can-drill: %ls ran on the can (dispatched=%d)", name, ok ? 1 : 0);
    });
}

void Settle() { ::Sleep(1500); }

void RunHost() {
    if (!Await("client_ready")) return;
    ::Sleep(3000);
    std::string keyA, keyB;
    OnGameThread([&] {
        void* cls = ue_wrap::asset_load::LoadObjectByPath(L"/Game/objects/prop_cannedFood_0.prop_cannedFood_0_C");
        void* local = coop::players::Registry::Get().Local();
        ue_wrap::FVector me{};
        if (!cls || !local || !E::TryGetActorLocation(local, me)) return;
        void* a = E::SpawnActor(cls, {me.X + 120.f, me.Y, me.Z + 40.f});
        void* b = E::SpawnActor(cls, {me.X + 120.f, me.Y + 80.f, me.Z + 40.f});
        const std::wstring ka = a ? ue_wrap::prop::GetInteractableKeyString(a) : L"";
        const std::wstring kb = b ? ue_wrap::prop::GetInteractableKeyString(b) : L"";
        keyA.assign(ka.begin(), ka.end());
        keyB.assign(kb.begin(), kb.end());
    });
    if (keyA.empty() || keyB.empty()) {
        UE_LOGW("can-drill: VERDICT ABORT -- the cans did not spawn");
        return;
    }
    ::Sleep(2000);
    LogState("0-born", "A", keyA);
    LogState("0-born", "B", keyB);
    Mark("keys", keyA + " " + keyB);
    if (!Await("client_saw_born")) return;
    Verb(keyA, L"Open");   // A: the host opens
    Settle();
    LogState("1-host-opened", "A", keyA);
    Mark("host_opened");
    if (!Await("client_ate")) return;   // A: the client eats
    Settle();
    LogState("2-client-ate", "A", keyA);
    Mark("host_saw_eat");
    if (!Await("client_opened")) return;   // B: the client opens
    Settle();
    LogState("3-client-opened", "B", keyB);
    Verb(keyB, L"eat");   // B: the host eats
    Settle();
    LogState("4-host-ate", "B", keyB);
    Mark("host_ate");
    if (!Await("client_done")) return;
    UE_LOGI("can-drill: DONE");
}

void RunClient() {
    bool ready = false;
    for (int i = 0; i < 480 && !ready; ++i) {
        OnGameThread([&] { ready = coop::net_pump::HasAnnouncedWorldReady(); });
        if (!ready) ::Sleep(500);
    }
    ::Sleep(8000);
    Mark("client_ready");
    std::string keys;
    if (!Await("keys", &keys)) return;
    const size_t sp = keys.find(' ');
    const std::string keyA = keys.substr(0, sp), keyB = keys.substr(sp + 1);
    for (int i = 0; i < 100 && (!Find(keyA) || !Find(keyB)); ++i) ::Sleep(100);
    LogState("0-born", "A", keyA);
    LogState("0-born", "B", keyB);
    Mark("client_saw_born");
    if (!Await("host_opened")) return;
    Settle();
    LogState("1-host-opened", "A", keyA);
    Verb(keyA, L"eat");
    Settle();
    LogState("2-client-ate", "A", keyA);
    Mark("client_ate");
    if (!Await("host_saw_eat")) return;
    Verb(keyB, L"Open");
    Settle();
    LogState("3-client-opened", "B", keyB);
    Mark("client_opened");
    if (!Await("host_ate")) return;
    Settle();
    LogState("4-host-ate", "B", keyB);
    Mark("client_done");
    UE_LOGI("can-drill: DONE");
}

}  // namespace

DWORD WINAPI CanStateThread(LPVOID /*arg*/) {
    g_dir = coop::config::ReadEnv("VOTVCOOP_CAN_DIR");
    if (g_dir.empty()) {
        UE_LOGW("can-drill: VERDICT ABORT -- VOTVCOOP_CAN_DIR is unset");
        return 0;
    }
    if (IsClientRole()) RunClient();
    else                RunHost();
    return 0;
}

}  // namespace harness::autotest
