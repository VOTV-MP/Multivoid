// harness/autotest/autotest_module_buy.cpp -- TEST-ONLY: the upgrade panel's module rows across two peers.
//
// The client holds the laptop and presses a module row's own buy button: the one with a level, and one with no store. Both peers
// must read the same levels and the same balance, charged once, and a second press of a full row charges nothing. Gated by VOTVCOOP_RUN_MODULE_BUY; the folder is VOTVCOOP_CAN_DIR.

#include "harness/autotest.h"

#include "coop/config/config.h"
#include "coop/interactables/device_occupancy.h"
#include "coop/player/players_registry.h"
#include "coop/session/net_pump.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/component_calls.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/devices/laptop_widget.h"
#include "ue_wrap/world/economy.h"
#include "ue_wrap/world/upgrades.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace harness::autotest {
namespace {

namespace OI = ue_wrap::object_index;
namespace R = ue_wrap::reflection;
namespace UP = ue_wrap::upgrades;

constexpr int kTriangleIndex = 27;
const wchar_t* const kBuy = L"BndEvt__button_upgDownloadSpd_K2Node_ComponentBoundEvent_0_OnButtonClickedEvent__DelegateSignature";

std::string g_dir;
int g_fails = 0;

template <class Fn>
void OnGameThread(Fn&& fn) {
    ue_wrap::game_thread::RunAndWait([&fn](std::atomic<int>& d) { fn(); d.store(1); });
}

void Mark(const std::string& name, const std::string& body = "x") {
    std::string tmp = g_dir + "\\" + name + ".tmp";
    { std::ofstream(tmp, std::ios::binary | std::ios::trunc) << body; }
    ::MoveFileExA(tmp.c_str(), (g_dir + "\\" + name).c_str(), MOVEFILE_REPLACE_EXISTING);
}

bool Await(const std::string& name, std::string* body = nullptr, int seconds = 240) {
    for (int i = 0; i < seconds * 10; ++i) {
        std::ifstream f(g_dir + "\\" + name, std::ios::binary);
        std::string s;
        if (f && std::getline(f, s) && !s.empty()) {
            if (body) *body = s;
            return true;
        }
        ::Sleep(100);
    }
    UE_LOGW("module-buy: VERDICT ABORT -- no '%hs' from the other peer", name.c_str());
    return false;
}

void Check(bool ok, const char* what) {
    UE_LOGI("module-buy: %hs %hs", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fails;
}

struct Row {
    void* widget = nullptr;
    int32_t index = -1, price = -1, maxLvl = -1, accumulation = -1;
    bool module = false;
};

std::vector<Row> Rows() {
    std::vector<Row> out;
    void* const cls = R::FindClass(L"uicomp_upgradeSlot_C");
    if (!cls) return out;
    struct Ctx { std::vector<Row>* out; void* cls; } ctx{&out, cls};
    OI::ForEachInstance(cls, [](void* c, void* obj, int32_t) {
        auto* x = static_cast<Ctx*>(c);
        if (!obj || !R::IsLive(obj) || R::NameStartsWith(R::NameOf(obj), L"Default__")) return;
        auto read = [&](const wchar_t* field) {
            const int32_t off = R::FindPropertyOffset(x->cls, field);
            return off < 0 ? -1 : *reinterpret_cast<const int32_t*>(static_cast<const uint8_t*>(obj) + off);
        };
        Row r;
        r.widget = obj;
        r.index = read(L"index");
        r.price = read(L"price");
        r.maxLvl = read(L"maxLvl");
        r.accumulation = read(L"levelAccumulation");
        int32_t byte = -1;
        uint8_t mask = 0;
        if (R::FindBoolProperty(x->cls, L"module", byte, mask)) r.module = (static_cast<const uint8_t*>(obj)[byte] & mask) != 0;
        x->out->push_back(r);
    }, &ctx);
    return out;
}

// The row of the live panel: a template of the panel stands beside it, and its gamemode was never set.
void* LiveModuleRow(int index) {
    void* const cls = R::FindClass(L"uicomp_upgradeSlot_C");
    const int32_t offGamemode = cls ? R::FindPropertyOffset(cls, L"gamemode") : -1;
    if (offGamemode < 0) return nullptr;
    for (const Row& r : Rows())
        if (r.module && r.index == index && *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(r.widget) + offGamemode)) return r.widget;
    return nullptr;
}

// The way the game's enter ends: the laptop's widget becomes the player's active interface, and the occupancy poll claims it.
bool EnterLaptop() {
    void* player = coop::players::Registry::Get().Local();
    void* widget = ue_wrap::laptop_widget::Widget();
    void* const fn = player ? R::FindDispatchFunctionCached(R::ClassOf(player), L"setActiveInterface") : nullptr;
    if (!fn || !widget) return false;
    ue_wrap::ParamFrame f(fn);
    if (!f.valid()) return false;
    f.Set<void*>(L"activeInterface", widget);
    f.Set<bool>(L"isActiveINterface3D", true);
    return ue_wrap::Call(player, f);
}

// The levels and the balance as one line.
std::string State() {
    int32_t levels[UP::kLevelCount] = {};
    int32_t points = 0;
    UP::ReadLevels(levels);
    ue_wrap::economy::ReadPoints(&points);
    std::string s = "points=" + std::to_string(points) + " levels=";
    for (int i = 0; i < UP::kLevelCount; ++i) s += std::to_string(levels[i]) + (i + 1 < UP::kLevelCount ? "," : "");
    return s;
}

std::string Read() {
    std::string s;
    OnGameThread([&] { s = State(); });
    return s;
}

}  // namespace

DWORD WINAPI ModuleBuyThread(LPVOID /*arg*/) {
    g_dir = coop::config::ReadEnv("VOTVCOOP_CAN_DIR");
    if (g_dir.empty()) {
        UE_LOGW("module-buy: VERDICT ABORT -- VOTVCOOP_CAN_DIR is unset");
        return 0;
    }
    const bool client = IsClientRole();
    if (client) {
        bool ready = false;
        for (int i = 0; i < 480 && !ready; ++i) {
            OnGameThread([&] { ready = coop::net_pump::HasAnnouncedWorldReady(); });
            if (!ready) ::Sleep(500);
        }
        ::Sleep(8000);
    } else {
        Await("client_ready", nullptr, 480);
    }
    // Each step: the client presses, then both peers must read one state.
    struct Step { const char* name; int index; int charge; const char* what; };
    const Step steps[] = {
        {"a", kTriangleIndex, 400, "the 400-credit module charged the group and raised the level on both peers"},
        {"b", kTriangleIndex, 0, "a second press of the full row charged nothing"},
        {"c", 23, 30, "a module row with no store charged the group, on both peers"},
    };
    if (!client) {
        // The fixture: the row the module writes starts empty, as on a fresh save, and the group can pay.
        OnGameThread([] { UP::WriteLevel(kTriangleIndex, 0); ue_wrap::economy::WritePoints(5000); });
        ::Sleep(1500);
        Mark("host_ready");
        for (const Step& st : steps) {
            std::string theirs;
            if (!Await(std::string("c_") + st.name, &theirs, 120)) return 0;
            ::Sleep(800);
            const std::string mine = Read();
            Mark(std::string("h_") + st.name, mine);
            UE_LOGI("module-buy: step %hs: host %hs", st.name, mine.c_str());
        }
        Await("client_done", nullptr, 60);
        UE_LOGI("module-buy: DONE on host fails=%d", g_fails);
        return 0;
    }
    Mark("client_ready");
    if (!Await("host_ready")) return 0;
    // Into the laptop: the host accepts a purchase from the player holding it.
    bool entered = false;
    OnGameThread([&] { entered = EnterLaptop(); });
    uint8_t holder = 0xFF;
    for (int i = 0; i < 100 && holder == 0xFF; ++i) {
        ::Sleep(100);
        OnGameThread([&] { holder = coop::device_occupancy::HolderOf(L"laptop"); });
    }
    UE_LOGI("module-buy: entered=%d holder=%u", entered ? 1 : 0, static_cast<unsigned>(holder));
    int32_t points = 0, pointsBefore = 0, triangleBefore = 0;
    OnGameThread([&] { ue_wrap::economy::ReadPoints(&pointsBefore); UP::ReadLevel(kTriangleIndex, &triangleBefore); });
    Check(triangleBefore == 0, "the fixture emptied the row on this client");
    for (const Step& st : steps) {
        OnGameThread([&] {
            ue_wrap::economy::ReadPoints(&pointsBefore);
            if (void* row = LiveModuleRow(st.index)) ue_wrap::component_calls::CallParamlessNamed(row, kBuy);
            else UE_LOGW("module-buy: no live module row %d", st.index);
        });
        ::Sleep(1500);
        OnGameThread([&] { ue_wrap::economy::ReadPoints(&points); });
        const std::string here = Read();
        Mark(std::string("c_") + st.name, here);
        std::string host;
        if (!Await(std::string("h_") + st.name, &host, 60)) return 0;
        ::Sleep(500);
        const std::string settled = Read();
        UE_LOGI("module-buy: step %hs: client %hs, host %hs", st.name, settled.c_str(), host.c_str());
        OnGameThread([&] { ue_wrap::economy::ReadPoints(&points); });
        Check(settled == host && points == pointsBefore - st.charge, st.what);
    }
    Mark("client_done");
    UE_LOGI("module-buy: DONE on client fails=%d", g_fails);
    return 0;
}

}  // namespace harness::autotest
