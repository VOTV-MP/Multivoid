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
// on both peers (VOTVCOOP_CAN_DIR). Red: cap_label_repaint_off. The join's own adopt has no red: a change the host
// makes after its world is taken reaches the joiner live and repaints it. The driver launches the client once the
// host logs `STATE 0-before-join`. The saved arm is host-only and described at "the saved arm" below.

#include "harness/autotest.h"
#include "disc_drills.h"   // co-located private header (src tree, not include/)
#include "harness/autotest/peer_signal.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/interactables/desk_input_sync.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/save/save_transfer.h"
#include "coop/session/net_pump.h"
#include "coop/session/rig_verdict.h"
#include "harness/session_runtime.h"
#include "harness/world_boot.h"
#include "ue_wrap/actors/inventory.h"
#include "ue_wrap/actors/puppet.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/desk/console_desk.h"
#include "ue_wrap/desk/desk_press.h"
#include "ue_wrap/desk/phys_mods.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_pawn.h"
#include "ue_wrap/engine/engine_save.h"
#include "ue_wrap/engine/save_to_slot_hook.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/world/game_mode.h"
#include "ue_wrap/world/world_singleton.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
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

// The verdict tag of this run's arm: `cap-label`, or `cap-label-saved` for the host-only saved arm.
const char* g_tag = "cap-label";

// A failed check is the verdict, and the caller returns without a later step. A pass says what matched; a fail says
// what was expected and what was read.
bool Check(bool ok, const std::string& matched, const std::string& expectedVsGot) {
    if (ok) {
        UE_LOGI("cap-label: PASS %hs", matched.c_str());
        return true;
    }
    UE_LOGW("cap-label: FAIL %hs", expectedVsGot.c_str());
    coop::rig_verdict::Say(g_tag, coop::rig_verdict::Outcome::Fail, "%s", expectedVsGot.c_str());
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

std::string Arm() { return coop::config::ResolveEnum(coop::config_registry::rows::cap_label_drill); }

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

// ---- the saved arm ----
// cap_label_drill=saved is a host-only run on a New Game (--host-fresh) for the desk's setData repaint, whose red is
// cap_label_setdata_off. The host plugs the module and presses the cap to its third level, saves through the game's
// own save, leaves for the menu as a quit does (the session ends first), loads that slot through the game's own load
// and waits for the desk's setData in the new world. Its label, read at that event, must be its saved level's and
// module's: `rig: VERDICT cap-label-saved PASS|FAIL|INVALID`. A host that loaded a save is INVALID.

// The slot the game's own save wrote, told by the engine's save seam on the game thread: the world's live save
// object under a slot of the game's own. The slot is left before the flag, so a reader that sees the flag sees it.
std::atomic<bool> g_saveWritten{false};
std::wstring g_savedSlot;

void OnWorldSaveWritten(void* saveObject, const wchar_t* slotName) {
    if (saveObject != ue_wrap::inventory::ResolveSaveSlot()) return;
    if (std::wcsncmp(slotName, L"zcoop_", 6) == 0) return;
    g_savedSlot = slotName;
    g_saveWritten.store(true, std::memory_order_release);
}

// The game's own save, as its save button runs it, and the wait for the engine to report the slot written.
bool SaveWorld() {
    if (!ue_wrap::save_to_slot_hook::AddWritten(&OnWorldSaveWritten)) {
        coop::rig_verdict::Say(g_tag, coop::rig_verdict::Outcome::Invalid, "the save seam has no free subscriber slot");
        return false;
    }
    if (!WaitFor("the player on its feet (the game's save refuses one in mid air)", 60, [] {
            bool walking = false;
            OnGameThread([&] {
                ue_wrap::puppet::ReadCharacterIsWalking(coop::players::Registry::Get().Local(), walking);
            });
            return walking;
        }))
        return false;
    bool ran = false;
    OnGameThread([&] {
        void* const gm = ue_wrap::world_singleton::Gamemode();
        void* const fn = gm ? R::FindFunction(R::ClassOf(gm), L"save") : nullptr;
        if (!fn) return;
        ue_wrap::ParamFrame f(fn);
        f.Set<bool>(L"quicksave", false);
        f.Set<bool>(L"bypassEvent", false);
        f.Set<bool>(L"overwriteSubsave", false);
        ran = f.valid() && ue_wrap::Call(gm, f);
    });
    UE_LOGI("cap-label: the game's save ran (ok=%d)", ran ? 1 : 0);
    if (!ran) {
        coop::rig_verdict::Say(g_tag, coop::rig_verdict::Outcome::Invalid, "the game's save did not dispatch");
        return false;
    }
    return WaitFor("the game's save written to a slot", 30, [] { return g_saveWritten.load(std::memory_order_acquire); });
}

// The host leaves for the menu as its player's quit does: the session ends first (its stop and teardown), then the
// game's own transition travels.
bool LeaveForMenu() {
    bool dispatched = false;
    OnGameThread([&] {
        auto& s = harness::session_runtime::Session();
        if (s.running()) coop::net_pump::FleeAfterNativeMenuTravel(s, "cap-label drill: the host leaves to load its save");
        dispatched = E::ReturnToMainMenu();
    });
    if (!dispatched)
        coop::rig_verdict::Say(g_tag, coop::rig_verdict::Outcome::Invalid, "the game's travel to the menu did not dispatch");
    return dispatched && WaitFor("the menu", 90, [] {
               return ue_wrap::world_identity::CurrentWorldKind() == ue_wrap::world_identity::WorldKind::Other;
           });
}

// The desk this peer reads stands in the world now current: the world just left may still hold a desk of its own.
bool DeskInCurrentWorld() {
    bool in = false;
    OnGameThread([&] {
        void* const desk = CD::Instance();
        in = desk && ue_wrap::world_identity::WorldOf(desk) == ue_wrap::world_identity::CurrentWorld();
    });
    return in;
}

void RunSavedArm(bool newGame) {
    if (!newGame) {
        coop::rig_verdict::Say(g_tag, coop::rig_verdict::Outcome::Invalid,
                               "the saved arm saves a New Game of its own; this host loaded a save (use --host-fresh)");
        return;
    }
    if (!WaitFor("the desk and its label readable", 120, [] { return Read().ok; })) return;
    LogState("0-host-new-game");
    Label saved;
    if (!PrepareCap("0-before-save", saved)) return;
    int mode = -1;
    OnGameThread([&] { mode = ue_wrap::game_mode::ReadLocal(); });
    if (mode < 0) {
        coop::rig_verdict::Say(g_tag, coop::rig_verdict::Outcome::Invalid, "the game mode could not be read");
        return;
    }
    if (!SaveWorld()) return;
    const std::wstring slot = g_savedSlot;
    UE_LOGI("cap-label: SAVED slot '%ls' (%hs)", slot.c_str(), Describe(saved).c_str());
    if (!LeaveForMenu()) return;
    UE_LOGI("cap-label: at the menu -- loading the slot through the game's own load");
    OnGameThread([] { E::ResetCachedSave(); });
    if (!harness::world_boot::BootStorySaveBlocking(false, slot.c_str(), mode)) {
        coop::rig_verdict::Say(g_tag, coop::rig_verdict::Outcome::Invalid, "the saved slot did not load to gameplay");
        return;
    }
    // The load's own setData writes the cap level and the modules back and owes the label's repaint; the label is
    // read at that event, in the world the load built.
    if (!WaitFor("the loaded world's desk and its save data applied", 120,
                 [] { return coop::desk_input_sync::DeskDataApplied() && DeskInCurrentWorld() && Read().ok; }))
        return;
    LogState("1-host-loaded-save");
    const Label loaded = Read();
    if (loaded.level != saved.level || loaded.module != saved.module) {
        coop::rig_verdict::Say(g_tag, coop::rig_verdict::Outcome::Invalid,
                               "the load did not bring back what the save held: saved %s, loaded %s",
                               Describe(saved).c_str(), Describe(loaded).c_str());
        return;
    }
    if (!CheckOwn(loaded, "1 (the loaded desk)")) return;
    coop::rig_verdict::Say(g_tag, coop::rig_verdict::Outcome::Pass, "the loaded desk's label is its saved level's and "
                           "module's: %s", Describe(loaded).c_str());
    UE_LOGI("cap-label: DONE");
}

void RunHost() {
    // A world restored from a save settles at the desk's setData; a New Game has no slot and nothing to restore (the
    // game's loadObjects reaches the desk's setData only past a saved player transform, which a blank save lacks).
    bool newGame = false;
    OnGameThread([&] { newGame = coop::save_transfer::HostSlot().empty(); });
    if (Arm() == "saved") {
        RunSavedArm(newGame);
        return;
    }
    if (newGame) {
        UE_LOGI("cap-label: HOST world is a New Game -- nothing to restore, the desk is settled");
    } else {
        UE_LOGI("cap-label: HOST waiting for the desk's save data (setData returned)");
        if (!WaitFor("the desk's save data applied (setData returned)", 120,
                     [] { return coop::desk_input_sync::DeskDataApplied(); }))
            return;
    }
    if (!WaitFor("the desk and its label readable", 120, [&] { return Read().ok; })) return;
    LogState("0-host-loaded");
    Label prepared;
    if (!PrepareCap("0-before-join", prepared)) return;
    if (!Await("client_begun")) return;
    Mark("host_prepared", Format(prepared));
    if (!Await("client_joined")) return;
    if (!RunSteps(true)) return;
    UE_LOGI("cap-label: DONE");
    // Every step was confirmed by the peer that did not act before the next ran: a client's failure said FAIL itself.
    coop::rig_verdict::Say(g_tag, coop::rig_verdict::Outcome::Pass,
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
    if (!Check(joined.ok && Format(joined) == hostLabel, "1: the joined label is the host's, '" + hostLabel + "'",
               "1: expected the host's label '" + hostLabel + "', got '" + Format(joined) + "'"))
        return;
    Mark("client_joined");
    if (!RunSteps(false)) return;
    UE_LOGI("cap-label: DONE");
}

}  // namespace

DWORD WINAPI CapLabelThread(LPVOID /*arg*/) {
    if (Arm() == "saved") g_tag = "cap-label-saved";
    if (!PS::Begin(g_tag)) return 0;
    if (IsClientRole()) RunClient();
    else                RunHost();
    return 0;
}

}  // namespace harness::autotest
