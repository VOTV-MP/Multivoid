// ue_wrap/core/game_thread_wait.cpp -- RunAndWait, the game-thread wait with a fault guard. See game_thread.h.
#include "ue_wrap/core/game_thread.h"

#include "ue_wrap/core/log.h"

#include <chrono>
#include <memory>
#include <thread>

namespace ue_wrap::game_thread {

int RunAndWait(std::function<void(std::atomic<int>&)> body) {
    if (IsGameThread()) {
        static std::atomic<int> s_said{0};   // the first three, so the selftest's own call leaves a misuse its line
        if (s_said.fetch_add(1) < 3)
            UE_LOGE("game_thread: RunAndWait called on the game thread, which would wait on itself -- "
                    "answered kTaskFaulted (said for the first three)");
        return kTaskFaulted;
    }
    auto done = std::make_shared<std::atomic<int>>(0);
    Post([done, body = std::move(body)]() mutable {
        // Runs as the pump unwinds past this frame too, so a body that faulted leaves its mark.
        struct Settle {
            std::atomic<int>& d;
            ~Settle() {
                int none = 0;
                d.compare_exchange_strong(none, kTaskFaulted);
            }
        } settle{*done};
        body(*done);
    });
    while (done->load() == 0) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return done->load();
}

}  // namespace ue_wrap::game_thread
