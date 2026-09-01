// coop/dev/save_probe.cpp -- see coop/dev/save_probe.h.

#include "coop/dev/save_probe.h"

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/save_browser.h"

#include <windows.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace coop::dev::save_probe {
namespace {

namespace GT = ue_wrap::game_thread;
namespace SB = ue_wrap::save_browser;

std::string ReadEnv(const char* name) {
    char buf[256] = {};
    const DWORD n = ::GetEnvironmentVariableA(name, buf, sizeof(buf));
    return (n > 0 && n < sizeof(buf)) ? std::string(buf) : std::string();
}

// Post EnumerateSaves to the game thread and block (this worker) until it runs.
// EnumerateSaves logs each save itself; we log the count + ok with a tag.
void PostEnumerate(const char* tag) {
    auto done = std::make_shared<std::atomic<int>>(-1);  // -1 pending, else count (or -2 fail)
    GT::Post([tag, done] {
        std::vector<SB::SaveInfo> v;
        const bool ok = SB::EnumerateSaves(v);
        UE_LOGI("save_probe[%s]: EnumerateSaves ok=%d count=%zu", tag, ok ? 1 : 0, v.size());
        done->store(ok ? static_cast<int>(v.size()) : -2, std::memory_order_release);
    });
    for (int i = 0; i < 800 && done->load(std::memory_order_acquire) == -1; ++i) ::Sleep(5);
}

DWORD WINAPI Thread(LPVOID) {
    UE_LOGI("save_probe: starting (wait 8 s for the menu / GameInstance + save-slots widget to load)");
    ::Sleep(8000);
    PostEnumerate("enum1");
    ue_wrap::log::Flush();

    // Optional: create NAMED story saves then re-enumerate to confirm they persist + appear.
    // VOTVCOOP_TEST_SAVE_CREATE=<base name> (e.g. "coopProbe"). ASCII.
    //
    // THREE CALLS ON ONE NAME, because what needs proving is a DIFFERENTIAL and not a
    // success: the exact primitive must REFUSE a name it has already taken, and the unique
    // variant must keep going beside it. A run that only exercised the unique call would
    // pass identically on a rig where the base name happened to be free -- which is exactly
    // the condition under which the defect it closes is invisible. So the probe MAKES the
    // collision it then measures, rather than depending on the state of the box's saves.
    const std::string createName = ReadEnv("VOTVCOOP_TEST_SAVE_CREATE");
    if (!createName.empty()) {
        const std::wstring wname(createName.begin(), createName.end());
        ::Sleep(1500);
        auto done = std::make_shared<std::atomic<int>>(0);  // 0 pending,1 done
        GT::Post([wname, done] {
            std::wstring first, dup, second;
            const bool okFirst  = SB::CreateNamedSaveUnique(wname, /*mode=*/0 /*story*/, first);
            const bool okDup    = SB::CreateNamedSave(wname, /*mode=*/0 /*story*/, dup);
            const bool okSecond = SB::CreateNamedSaveUnique(wname, /*mode=*/0 /*story*/, second);
            UE_LOGI("save_probe: unique#1('%ls') ok=%d slot='%ls'", wname.c_str(), okFirst ? 1 : 0,
                    first.c_str());
            UE_LOGI("save_probe: exact-on-taken('%ls') ok=%d slot='%ls' (MUST be ok=0)",
                    wname.c_str(), okDup ? 1 : 0, dup.c_str());
            UE_LOGI("save_probe: unique#2('%ls') ok=%d slot='%ls' (MUST differ from unique#1)",
                    wname.c_str(), okSecond ? 1 : 0, second.c_str());
            const bool pass = okFirst && !okDup && okSecond && !second.empty() && second != first;
            UE_LOGI("save_probe: UNIQUE-NAME DIFFERENTIAL %s", pass ? "PASS" : "FAIL");
            done->store(1, std::memory_order_release);
        });
        for (int i = 0; i < 800 && done->load(std::memory_order_acquire) == 0; ++i) ::Sleep(5);
        ::Sleep(1500);
        PostEnumerate("enum2-after-create");
    }

    UE_LOGI("save_probe: DONE");
    ue_wrap::log::Flush();
    return 0;
}

}  // namespace

void Init() {
    if (ReadEnv("VOTVCOOP_TEST_SAVE_ENUM") != "1") return;
    if (HANDLE t = ::CreateThread(nullptr, 0, &Thread, nullptr, 0, nullptr)) {
        ::CloseHandle(t);
    }
}

}  // namespace coop::dev::save_probe
