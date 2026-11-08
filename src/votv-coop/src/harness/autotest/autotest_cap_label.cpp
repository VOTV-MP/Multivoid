// harness/autotest/autotest_cap_label.cpp -- TEST-ONLY: the refiner's cap label on two peers, with and without the
// Auto Processing module.
//
// The label is the cap button's tag (the hover's "Cap level [N]", or the "Upgrade Auto Processing to use" line) and
// the screen's cap text ("[N]", or "NaN"); only the cap press and the desk's upgraded_pcUpgrades write them. Before
// the client joins, the host plugs the module and presses the cap to its third level, so the join carries both.
// Then each peer in turn plugs or pulls the module or presses the cap, through the desk's own plugInModule and
// press, and after each step both peers must show the same label, the one the actor's level and module say. Each
// step logs `cap-label: STATE <step> level= module= tag= text=` on both peers and the drill ends with
// `cap-label: DONE fails=N`. Gated by VOTVCOOP_RUN_CAP_LABEL; the folder is VOTVCOOP_CAN_DIR. The host prepares only
// once the driver writes `host_loaded` there, after the host's log shows the desk's save restored: the save's setData
// writes the cap and the modules back after the session starts, and it repaints no label. The driver launches the
// client once the host logs `STATE 0-before-join`, and the host sends its label when the client's drill has begun
// (a signal counts only when written after the reader's drill began).

#include "harness/autotest.h"
#include "harness/autotest/peer_signal.h"

#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/session/net_pump.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/desk/console_desk.h"
#include "ue_wrap/desk/desk_press.h"
#include "ue_wrap/desk/phys_mods.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_pawn.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>

namespace harness::autotest {
namespace {

namespace R  = ue_wrap::reflection;
namespace CD = ue_wrap::console_desk;
namespace PM = ue_wrap::phys_mods;
namespace E  = ue_wrap::engine;
namespace PS = harness::autotest::peer_signal;
using PS::Await;
using PS::AwaitText;
using PS::Mark;
using PS::WaitFor;

int g_fails = 0;

template <class Fn>
void OnGameThread(Fn&& fn) {
    ue_wrap::game_thread::RunAndWait([&fn](std::atomic<int>& d) { fn(); d.store(1); });
}

void Check(bool ok, const char* what) {
    if (ok) {
        UE_LOGI("cap-label: PASS %hs", what);
    } else {
        ++g_fails;
        UE_LOGW("cap-label: FAIL %hs", what);
    }
}

std::string Narrow(const std::wstring& w) {
    std::string out;
    for (wchar_t c : w) out += (c > 0 && c < 128) ? static_cast<char>(c) : '?';
    return out;
}

// The cap as this peer shows it: the level, the module, and the label. ok is false while any of it does not read.
struct Label {
    bool ok = false;
    int32_t level = -1;
    bool module = false;
    std::string tag, text;
};

Label Read() {
    Label l;
    OnGameThread([&] {
        CD::Scalars sc;
        uint8_t arr[PM::kSlots];
        std::wstring tag, text;
        if (!CD::EnsureResolved() || !PM::EnsureResolved() || !CD::ReadScalars(sc) || !PM::ReadArray(arr) ||
            !CD::ReadCapLabel(tag, text))
            return;
        l.ok = true;
        l.level = sc.compMaxLevel;
        l.module = PM::HasAutoProcessing(arr);
        l.tag = Narrow(tag);
        l.text = Narrow(text);
    });
    return l;
}

// What the two peers compare: the whole label, with the level and the module it stands for.
std::string Format(const Label& l) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%d %d|", l.level, l.module ? 1 : 0);
    return buf + l.tag + "|" + l.text;
}

// The screen text the game writes for this level and module: "[level + 1]", or "NaN" without the module.
std::string ExpectedText(const Label& l) {
    if (!l.module) return "NaN";
    return "[" + std::to_string(l.level + 1) + "]";
}

void LogState(const char* step) {
    const Label l = Read();
    if (!l.ok) {
        UE_LOGW("cap-label: STATE %hs -- the desk or its label did not read", step);
        return;
    }
    UE_LOGI("cap-label: STATE %hs level=%d module=%d tag='%hs' text='%hs'", step, l.level, l.module ? 1 : 0,
            l.tag.c_str(), l.text.c_str());
}

// ---- the desk's own verbs ----

bool ReadDeskBool(void* desk, const wchar_t* name) {
    int32_t off = -1;
    uint8_t mask = 0;
    return R::FindBoolProperty(R::ClassOf(desk), name, off, mask) &&
           (reinterpret_cast<const uint8_t*>(desk)[off] & mask) != 0;
}

void WriteDeskBool(void* desk, const wchar_t* name, bool on) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!R::FindBoolProperty(R::ClassOf(desk), name, off, mask)) return;
    uint8_t& b = reinterpret_cast<uint8_t*>(desk)[off];
    b = on ? static_cast<uint8_t>(b | mask) : static_cast<uint8_t>(b & ~mask);
}

// plugInModule and the slot press explode a hot desk with coldswap on: the drill's call runs as a cold desk's.
template <class F>
bool Cold(void* desk, F call) {
    const bool hot = (ReadDeskBool(desk, L"active_console") || ReadDeskBool(desk, L"active_comp") ||
                      ReadDeskBool(desk, L"active_coords") || ReadDeskBool(desk, L"active_download") ||
                      ReadDeskBool(desk, L"active_play")) &&
                     ReadDeskBool(desk, L"coldswapEnabled");
    if (hot) WriteDeskBool(desk, L"coldswapEnabled", false);
    const bool ok = call();
    if (hot) WriteDeskBool(desk, L"coldswapEnabled", true);
    return ok;
}

// An Auto Processing module, spawned at the desk and handed to plugInModule in the first empty slot that takes it.
bool PlugModule() {
    bool took = false;
    OnGameThread([&] {
        void* desk = CD::Instance();
        void* pawn = coop::players::Registry::Get().Local();
        void* cls = PM::ClassForByte(PM::kAutoProcessing);
        ue_wrap::FVector at{};
        uint8_t arr[PM::kSlots];
        if (!desk || !pawn || !cls || !E::TryGetActorLocation(desk, at) || !PM::ReadArray(arr)) return;
        for (int slot = 0; slot < PM::kSlots && !took; ++slot) {
            if (arr[slot]) continue;
            void* module = E::SpawnActor(cls, {at.X, at.Y, at.Z + 120.f});
            if (!module) return;
            Cold(desk, [&] { return PM::CallPlugInModule(desk, module, slot, pawn); });
            took = PM::ReadArray(arr) && arr[slot] == PM::kAutoProcessing;
            if (!took && R::IsLive(module)) E::DestroyActor(module);
        }
    });
    UE_LOGI("cap-label: plug %hs", took ? "took" : "NOT taken");
    return took;
}

// This player's E on every slot holding the module: each one goes to the hand, as the game's unplug does.
bool UnplugModule() {
    bool any = false;
    OnGameThread([&] {
        void* desk = CD::Instance();
        void* pawn = coop::players::Registry::Get().Local();
        uint8_t arr[PM::kSlots];
        if (!desk || !pawn || !PM::ReadArray(arr)) return;
        for (int slot = 0; slot < PM::kSlots; ++slot)
            if (arr[slot] == PM::kAutoProcessing)
                any |= Cold(desk, [&] { return PM::CallPressSlot(desk, pawn, slot); });
    });
    return any;
}

// This player's press of the cap button, its body's (level + 1) % 3 and the label it writes.
bool PressCap() {
    bool ok = false;
    OnGameThread([&] {
        void* desk = CD::Instance();
        void* pawn = coop::players::Registry::Get().Local();
        void* button = desk ? ue_wrap::desk_press::Member(desk, L"button_comp_maxLevel") : nullptr;
        ok = button && pawn && ue_wrap::desk_press::Press(desk, pawn, button);
    });
    return ok;
}

// ---- the steps ----

enum class Act : uint8_t { Plug, Unplug, Press };
struct Step {
    const char* name;
    bool hostActs;
    Act act;
};

// After the join (the host's module and third level), each peer in turn: with the module, without it, and the cap
// pressed both ways.
constexpr Step kSteps[] = {
    {"2-host-unplug",   true,  Act::Unplug},
    {"3-client-plug",   false, Act::Plug},
    {"4-client-press",  false, Act::Press},
    {"5-host-press",    true,  Act::Press},
    {"6-client-unplug", false, Act::Unplug},
    {"7-host-press",    true,  Act::Press},
    {"8-host-plug",     true,  Act::Plug},
};

// The actor acts and waits for its own desk to show the change; the other peer waits for its label to equal the
// actor's. Both log the step's STATE line.
void RunSteps(bool host) {
    for (const Step& st : kSteps) {
        const std::string seen = std::string(st.name) + "_seen";
        if (st.hostActs == host) {
            const Label was = Read();
            bool did = false;
            switch (st.act) {
            case Act::Plug:   did = PlugModule(); break;
            case Act::Unplug: did = UnplugModule(); break;
            case Act::Press:  did = PressCap(); break;
            }
            Check(did, (std::string(st.name) + ": the desk's own verb ran").c_str());
            WaitFor("this desk's change", 15, [&] {
                const Label l = Read();
                return l.ok && (l.level != was.level || l.module != was.module);
            }, false);
            const Label now = Read();
            LogState(st.name);
            Check(now.ok && now.text == ExpectedText(now),
                  (std::string(st.name) + ": the actor's label is its level's and module's").c_str());
            Mark(st.name, Format(now));
            if (!Await(seen.c_str())) return;
        } else {
            std::string theirs;
            if (!AwaitText(st.name, theirs)) return;
            const bool same = WaitFor("this peer's label to equal the actor's", 15, [&] {
                const Label l = Read();
                return l.ok && Format(l) == theirs;
            }, false);
            LogState(st.name);
            Check(same, (std::string(st.name) + ": this peer's label is the actor's ('" + theirs + "')").c_str());
            Mark(seen.c_str());
        }
    }
}

void RunHost() {
    if (!Await("host_loaded")) return;
    if (!WaitFor("the desk and its label readable", 120, [&] { return Read().ok; })) return;
    LogState("0-host-loaded");
    // Before the join: the module plugged and the cap at its third level, so the join has a label to carry.
    if (!Read().module) Check(PlugModule(), "0: the host's module plugged before the join");
    for (int i = 0; i < 3 && Read().level != 2; ++i) {
        const int32_t was = Read().level;
        PressCap();
        WaitFor("the host's cap press", 10, [&] { return Read().level != was; }, false);
    }
    const Label prepared = Read();
    LogState("0-before-join");
    Check(prepared.module && prepared.level == 2 && prepared.text == "[3]", "0: the host shows the third level");
    if (!Await("client_begun")) return;
    Mark("host_prepared", Format(prepared));
    if (!Await("client_joined")) return;
    RunSteps(true);
    UE_LOGI("cap-label: DONE fails=%d", g_fails);
}

void RunClient() {
    Mark("client_begun");
    bool ready = false;
    for (int i = 0; i < 480 && !ready; ++i) {
        OnGameThread([&] { ready = coop::net_pump::HasAnnouncedWorldReady(); });
        if (!ready) ::Sleep(500);
    }
    if (!WaitFor("the host's puppet", 60, [&] {
            bool have = false;
            OnGameThread([&] {
                coop::RemotePlayer* p = coop::players::Registry::Get().Puppet(0);
                have = p && p->valid();
            });
            return have;
        }))
        return;
    std::string hostLabel;
    if (!AwaitText("host_prepared", hostLabel)) return;
    const bool same = WaitFor("the joined label to equal the host's", 30, [&] {
        const Label l = Read();
        return l.ok && Format(l) == hostLabel;
    }, false);
    LogState("1-after-join");
    Check(same, ("1: the joined label is the host's ('" + hostLabel + "')").c_str());
    Mark("client_joined");
    RunSteps(false);
    UE_LOGI("cap-label: DONE fails=%d", g_fails);
}

}  // namespace

DWORD WINAPI CapLabelThread(LPVOID /*arg*/) {
    if (!PS::Begin("cap-label")) return 0;
    if (IsClientRole()) RunClient();
    else                RunHost();
    return 0;
}

}  // namespace harness::autotest
