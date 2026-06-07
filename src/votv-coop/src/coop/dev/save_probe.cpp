// coop/dev/save_probe.cpp -- see coop/dev/save_probe.h.

#include "coop/dev/save_probe.h"

#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"
#include "ue_wrap/save_browser.h"

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

    // Optional: create a NAMED story save then re-enumerate to confirm it persists +
    // appears. VOTVCOOP_TEST_SAVE_CREATE=<base name> (e.g. "coopProbe"). ASCII.
    const std::string createName = ReadEnv("VOTVCOOP_TEST_SAVE_CREATE");
    if (!createName.empty()) {
        const std::wstring wname(createName.begin(), createName.end());
        ::Sleep(1500);
        auto done = std::make_shared<std::atomic<int>>(0);  // 0 pending,1 done
        GT::Post([wname, done] {
            std::wstring outSlot;
            const bool ok = SB::CreateNamedSave(wname, /*mode=*/0 /*story*/, outSlot);
            UE_LOGI("save_probe: CreateNamedSave('%ls') ok=%d slot='%ls'",
                    wname.c_str(), ok ? 1 : 0, outSlot.c_str());
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
