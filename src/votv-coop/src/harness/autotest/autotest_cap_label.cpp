// harness/autotest/autotest_cap_label.cpp -- TEST-ONLY: the refiner's cap label on two peers, with and without the
// Auto Processing module.
//
// The label is the cap button's tag ("Cap level [N]", or the line asking for the module) and the screen's cap text
// ("[N]", or "NaN"); only the cap press and the desk's upgraded_pcUpgrades write them. The host plugs the module and
// presses the cap to its third level before the client joins; the joiner must show that label. Then each peer in
// turn plugs, pulls or presses, through the desk's own verbs, and both peers must show the actor's label. Each step
// logs `cap-label: STATE <step> ...`. The first failed check says `rig: VERDICT cap-label FAIL` and no later step
// runs; a wait that ends on its bound says INVALID. The receiver waits for the actor's level and module, which its
// apply writes in one game-thread task with the repaint, and reads its label once. Armed by the row cap_label_drill
// on both peers (VOTVCOOP_CAN_DIR). Red: cap_label_repaint_off. The driver launches the client once the host logs
// `STATE 0-before-join`. The saved arm (cap_label_drill=saved, the host on the rig's own save, no --host-fresh) skips
// the steps: the host's loaded desk must show its saved level and module's label, which only the lane's install
// repaint gives it, and the joiner the same one; both are read once the joiner has applied the host's state. Its red
// is cap_label_install_repaint_off.

#include "harness/autotest.h"
#include "disc_drills.h"   // co-located private header (src tree, not include/)
#include "harness/autotest/peer_signal.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/player/players_registry.h"
#include "coop/session/rig_verdict.h"
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

template <class Fn>
void OnGameThread(Fn&& fn) {
    ue_wrap::game_thread::RunAndWait([&fn](std::atomic<int>& d) { fn(); d.store(1); });
}


// A failed check is the verdict, and the caller returns without a later step. A pass says what matched; a fail says
// what was expected and what was read.
bool Check(bool ok, const std::string& matched, const std::string& expectedVsGot) {
    if (ok) {
        UE_LOGI("cap-label: PASS %hs", matched.c_str());
        return true;
    }
    UE_LOGW("cap-label: FAIL %hs", expectedVsGot.c_str());
    coop::rig_verdict::Say("cap-label", coop::rig_verdict::Outcome::Fail, "%s", expectedVsGot.c_str());
    return false;
}

// A check whose one sentence is the same either way (a verb that ran, a state reached).
bool Check(bool ok, const std::string& what) { return Check(ok, what, what); }

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

// The part of Format that is the desk's own state, the level and the module: what a wire apply writes before it
// repaints, so a peer showing it has finished the apply.
std::string StateOf(const std::string& formatted) { return formatted.substr(0, formatted.find('|')); }

// The label is the one this level and module call for: the screen text, and the tag ("Cap level [N]" with the
// module, the line asking for the module without it).
bool LabelIsOwn(const Label& l) {
    if (!l.ok || l.text != ExpectedText(l)) return false;
    return l.module ? l.tag.find("[" + std::to_string(l.level + 1) + "]") != std::string::npos
                    : l.tag.find("Auto Processing") != std::string::npos;
}

std::string Describe(const Label& l) {
    if (!l.ok) return "nothing (the desk or its label did not read)";
    return "level " + std::to_string(l.level) + ", module " + (l.module ? "1" : "0") + ", tag '" + l.tag + "', text '" +
           l.text + "'";
}

// LabelIsOwn as a check: the pass names the text and level that matched, the fail what the label should have been.
bool CheckOwn(const Label& l, const std::string& step) {
    const std::string want = l.module ? "text '" + ExpectedText(l) + "' and a tag naming level " +
                                            std::to_string(l.level + 1)
                                      : "text 'NaN' and a tag asking for Auto Processing";
    return Check(LabelIsOwn(l), step + ": the label is its level's and module's (" + Describe(l) + ")",
                 step + ": expected " + want + ", got " + Describe(l));
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

// After the join (the host's module and final level), each peer in turn: with the module, without it, and the cap
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

// The actor acts and waits for its own desk to show the change; the other peer waits for the actor's level and
// module to arrive and then reads its label once. Both log the step's STATE line, and the first failed check ends
// the run (false).
bool RunSteps(bool host) {
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
            if (!Check(did, std::string(st.name) + ": the desk's own verb ran")) return false;
            if (!WaitFor("this desk's level or module to change", 15, [&] {
                    const Label l = Read();
                    return l.ok && (l.level != was.level || l.module != was.module);
                }))
                return false;
            const Label now = Read();
            LogState(st.name);
            if (!CheckOwn(now, std::string(st.name) + " (the actor)")) return false;
            Mark(st.name, Format(now));
            if (!Await(seen.c_str())) return false;
        } else {
            std::string theirs;
            if (!AwaitText(st.name, theirs)) return false;
            if (!WaitFor("this peer's level and module to be the actor's", 15, [&] {
                    const Label l = Read();
                    return l.ok && StateOf(Format(l)) == StateOf(theirs);
                }))
                return false;
            const Label l = Read();
            LogState(st.name);
            if (!Check(l.ok && Format(l) == theirs,
                       std::string(st.name) + ": this peer's label is the actor's, '" + theirs + "'",
                       std::string(st.name) + ": expected the actor's label '" + theirs + "', got '" + Format(l) + "'"))
                return false;
            Mark(seen.c_str());
        }
    }
    return true;
}

// The module plugged and the cap at its third level, through the desk's own verbs; `prepared` is the label then.
bool PrepareCap(const char* stateName, Label& prepared) {
    if (!Read().module && !Check(PlugModule(), "0: the host's module plugged")) return false;
    for (int i = 0; i < 3 && Read().level != 2; ++i) {
        const int32_t was = Read().level;
        PressCap();
        if (!WaitFor("the host's cap press", 10, [&] { return Read().level != was; })) return false;
    }
    prepared = Read();
    LogState(stateName);
    return Check(prepared.module && prepared.level == 2 && prepared.text == "[3]",
                 "0: the host shows the third level (" + Describe(prepared) + ")",
                 "0: expected the third level with the module and text '[3]', got " + Describe(prepared));
}

std::string Arm() { return coop::config::ResolveEnum(coop::config_registry::rows::cap_label_drill); }

// The saved arm's host: its world was loaded from the rig's own save, whose desk the gamemode's load restored before
// this lane installed, so its label is right only if the lane's install repaint ran. Its state (level, module) is
// what the joiner waits for; the host's label is judged at the event the fresh arm judges the joiner's, the joiner's
// own apply, by which the host's session has ticked.
void RunSavedHost() {
    LogState("0-before-join");
    if (!Await("client_begun")) return;
    const Label loaded = Read();
    if (!loaded.module) {  // without the module the right label is the default one at every level
        coop::rig_verdict::Say("cap-label", coop::rig_verdict::Outcome::Invalid,
                               "the save holds no Auto Processing: the right label is the default one, so a stale "
                               "label could not be told from it");
        return;
    }
    Mark("host_prepared", Format(loaded));
    if (!Await("client_joined")) return;
    const Label held = Read();
    LogState("1-host-after-join");
    if (!CheckOwn(held, "1 (the host's loaded desk)")) return;
    UE_LOGI("cap-label: DONE");
    coop::rig_verdict::Say("cap-label", coop::rig_verdict::Outcome::Pass,
                           "the host's loaded desk and the joiner's show the saved level and module's label: %s",
                           Describe(held).c_str());
}

void RunHost() {
    if (!WaitFor("the desk and its label readable", 120, [&] { return Read().ok; })) return;
    if (Arm() == "saved") {
        RunSavedHost();
        return;
    }
    LogState("0-host-loaded");
    Label prepared;
    if (!PrepareCap("0-before-join", prepared)) return;
    if (!Await("client_begun")) return;
    Mark("host_prepared", Format(prepared));
    if (!Await("client_joined")) return;
    if (!RunSteps(true)) return;
    UE_LOGI("cap-label: DONE");
    // Every step was confirmed by the peer that did not act before the next ran: a client's failure said FAIL itself.
    coop::rig_verdict::Say("cap-label", coop::rig_verdict::Outcome::Pass,
                           "the cap label read the level and module's own on both peers after every step");
}

void RunClient() {
    Mark("client_begun");
    if (!disc_drill::HostPuppetUp()) return;
    std::string hostLabel;
    if (!AwaitText("host_prepared", hostLabel)) return;
    if (!WaitFor("the joined desk's level and module to be the host's", 30, [&] {
            const Label l = Read();
            return l.ok && StateOf(Format(l)) == StateOf(hostLabel);
        }))
        return;
    const Label joined = Read();
    LogState("1-after-join");
    // The saved host judges its own label at this apply, so the saved joiner is judged against its own level.
    if (Arm() == "saved") {
        if (!CheckOwn(joined, "1 (the joined desk)")) return;
    } else if (!Check(joined.ok && Format(joined) == hostLabel, "1: the joined label is the host's, '" + hostLabel + "'",
                      "1: expected the host's label '" + hostLabel + "', got '" + Format(joined) + "'"))
        return;
    Mark("client_joined");
    if (Arm() != "saved" && !RunSteps(false)) return;
    UE_LOGI("cap-label: DONE");
}

}  // namespace

DWORD WINAPI CapLabelThread(LPVOID /*arg*/) {
    if (!PS::Begin("cap-label")) return 0;
    if (IsClientRole()) RunClient();
    else                RunHost();
    return 0;
}

}  // namespace harness::autotest
