// harness/autotest_saveui.cpp -- autonomous tests for the save-safety UI.
//
// The pair belongs together: both are the client-side half of host-only persistence, and both are
// client-only and verify from the log the module itself writes.
//   - RunAutonomousSaveBlockTest drives saveSlot.saveToSlot, so the native SaveGameToSlot block is
//     observable without waiting for an autosave or a menu trigger.
//   - RunAutonomousSaveBtnDisableTest drives mainPlayer_C::InpActEvt_Escape, so the pause-menu
//     Save-button grey-out is observable without a real ESC press.

#include "harness/autotest.h"

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace harness::autotest {
namespace {

namespace P = ue_wrap::profile;
namespace R = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;

// Local copy of harness's ReadEnv (matches autotest.cpp's -- tiny, not worth crossing TUs).
std::string ReadEnv(const char* name) {
    char buf[256] = {};
    const DWORD n = ::GetEnvironmentVariableA(name, buf, sizeof(buf));
    return (n > 0 && n < sizeof(buf)) ? std::string(buf) : std::string();
}

}  // namespace

// ---- the autonomous client save-block test ---------------------------------
//
// CLIENT-ONLY. Proves the SaveGameToSlot native hook (coop/save/save_block.cpp) actually CANCELS a
// world save on the client, not merely that it installed. Over a short run neither the autosave
// timer nor a menu save fires, so the exact write path is driven by reflection: resolve the live
// saveSlot_C world-save object and call its saveToSlot UFunction directly -- the blueprint funnel
// closest to the hook, with no gamemode-level gate in the way. saveToSlot's body invokes
// UGameplayStatics::SaveGameToSlot, our hook target, once per slot and subsave; on the client the
// detour returns false WITHOUT calling the trampoline, so nothing reaches disk.
//
// The client log shows `saveblock_test: invoking ...` immediately followed by `save_block: BLOCKED
// client world-save ...`. The host neither runs this routine nor installs the hook, so its log
// carries no save_block line at all and its save path is byte-for-byte untouched. Gated by
// VOTVCOOP_RUN_SAVEBLOCK_TEST="1".
void RunAutonomousSaveBlockTest() {
    const bool isHost = !IsClientRole();
    if (isHost) {
        UE_LOGI("saveblock_test: not client -- this routine is client-only (the host "
                "installs no save hook; its save path is untouched). Returning.");
        return;
    }
    UE_LOGI("saveblock_test: starting on client (waiting 18 s: connect + possession + "
            "world load so saveSlot_C exists and the save_block hook is installed)");
    ::Sleep(18000);

    auto done = std::make_shared<std::atomic<int>>(0);  // 0=pending 1=invoked -1=resolve-fail
    GT::Post([done] {
        void* saveSlot = R::FindObjectByClass(L"saveSlot_C");
        if (!saveSlot) {
            UE_LOGW("saveblock_test: no live saveSlot_C instance -- world not fully "
                    "loaded? cannot exercise the save path");
            done->store(-1, std::memory_order_release);
            return;
        }
        void* fn = R::FindFunction(R::ClassOf(saveSlot), L"saveToSlot");
        if (!fn) {
            UE_LOGW("saveblock_test: saveToSlot UFunction not found on saveSlot_C -- "
                    "cannot exercise the save path");
            done->store(-1, std::memory_order_release);
            return;
        }
        // saveToSlot(quicksave, overwriteSubsave, isForcedSave): a zeroed frame = full,
        // non-forced save -> the body's UGameplayStatics::SaveGameToSlot calls hit our
        // client hook and are cancelled. Serialization is content-agnostic (it serializes
        // whatever the live save object holds), and the write is blocked regardless, so
        // calling it directly is safe.
        const int32_t frameSize = R::FunctionFrameSize(fn);
        std::vector<uint8_t> frame(frameSize > 0 ? static_cast<size_t>(frameSize) : 0, 0u);
        UE_LOGI("saveblock_test: invoking saveSlot.saveToSlot on client (saveSlot=%p, "
                "frameSize=%d) -- expect a 'save_block: BLOCKED' line next if the hook fired",
                saveSlot, frameSize);
        R::CallFunction(saveSlot, fn, frame.empty() ? nullptr : frame.data());
        done->store(1, std::memory_order_release);
    });

    for (int i = 0; i < 1000 && done->load(std::memory_order_acquire) == 0; ++i) ::Sleep(5);
    const int code = done->load(std::memory_order_acquire);
    if (code == 1) {
        UE_LOGI("saveblock_test: DONE -- saveToSlot invoked; grep this client log for "
                "'save_block: BLOCKED' to confirm the write was cancelled");
    } else {
        UE_LOGW("saveblock_test: did NOT invoke saveToSlot (resolve failed or task "
                "did not run) -- inconclusive");
    }
}

DWORD WINAPI SaveBlockTestThread(LPVOID /*arg*/) {
    RunAutonomousSaveBlockTest();
    return 0;
}

// ---- the autonomous Save-button grey-out test ------------------------------
//
// CLIENT-ONLY. Proves that the InpActEvt_Escape POST observer in coop/save/save_button_disable.cpp
// actually DISABLES the pause-menu Save button, not merely that it installed. The pause menu is
// input-driven and a real ESC press cannot be produced autonomously, so the hooked input event is
// driven by reflection: find the live GameInstance-owned ui_menu_C, force its isPause flag true so
// the observer treats it as the pause menu without a real PlayerController or SetGamePaused, then
// CallFunction(mainPlayer_C::InpActEvt_Escape), which routes through ProcessEvent and fires
// OnEscapePost exactly as a real ESC does. The module logs its read-back on apply.
//
// The client log shows `save_button_disable: ... INSTALLED` and then, once this drives the event,
// `save_button_disable: greyed button_Save ... GetIsEnabled now=0`. That the button is visibly grey
// and that the ESC menu still OPENS are the two things only a person can see. Gated by
// VOTVCOOP_RUN_SAVEBTN_TEST="1".
void RunAutonomousSaveBtnDisableTest() {
    if (IsClientRole()) {
        UE_LOGI("savebtn_test: starting on client (waiting 18 s: connect + possession + "
                "ui_menu_C loaded + the save_button_disable observers installed)");
    } else {
        UE_LOGI("savebtn_test: not client -- this routine is client-only (the host's Save "
                "button stays functional). Returning.");
        return;
    }
    ::Sleep(18000);

    auto done = std::make_shared<std::atomic<int>>(0);  // 0=pending 1=drove -1=resolve-fail
    GT::Post([done] {
        void* menu = R::FindObjectByClass(P::name::UiMenuClass);
        void* mp = R::FindObjectByClass(P::name::MainPlayerClass);
        if (!menu || !mp) {
            UE_LOGW("savebtn_test: ui_menu_C=%p mainPlayer_C=%p not loaded -- cannot drive", menu, mp);
            done->store(-1, std::memory_order_release);
            return;
        }
        // Force the pause variant so OnEscapePost/OnMenuTickPost treat this instance as the
        // pause menu (the autonomous drive does not go through a real SetGamePaused).
        const int32_t isPauseOff = R::FindPropertyOffset(R::ClassOf(menu), P::name::UiMenuIsPauseProp);
        if (isPauseOff >= 0) *(reinterpret_cast<uint8_t*>(menu) + isPauseOff) = 1;
        void* escFn = R::FindFunction(R::ClassOf(mp), P::name::MainPlayerEscapeFn);
        if (!escFn) {
            UE_LOGW("savebtn_test: InpActEvt_Escape not found on mainPlayer_C -- cannot drive");
            done->store(-1, std::memory_order_release);
            return;
        }
        const int32_t fs = R::FunctionFrameSize(escFn);
        std::vector<uint8_t> frame(fs > 0 ? static_cast<size_t>(fs) : 0, 0u);
        UE_LOGI("savebtn_test: driving mainPlayer_C::InpActEvt_Escape on client (menu=%p, "
                "isPauseOff=%d) -- expect a 'save_button_disable: greyed button_Save' line next",
                menu, isPauseOff);
        R::CallFunction(mp, escFn, frame.empty() ? nullptr : frame.data());
        done->store(1, std::memory_order_release);
    });

    for (int i = 0; i < 1000 && done->load(std::memory_order_acquire) == 0; ++i) ::Sleep(5);
    UE_LOGI("savebtn_test: DONE (code=%d) -- grep this client log for "
            "'save_button_disable: greyed button_Save' (GetIsEnabled now=0 => the disable took)",
            done->load(std::memory_order_acquire));
}

DWORD WINAPI SaveBtnDisableTestThread(LPVOID /*arg*/) {
    RunAutonomousSaveBtnDisableTest();
    return 0;
}

}  // namespace harness::autotest
