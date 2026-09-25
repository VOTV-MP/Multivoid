// coop/dev/run_and_wait_selftest.cpp -- [dev] the one game-thread wait's five endings. See the header.
#include "coop/dev/run_and_wait_selftest.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <stdexcept>

namespace coop::dev::run_and_wait_selftest {
namespace {

namespace GT = ue_wrap::game_thread;

// An address the compiler cannot prove null, so the provoked write is emitted.
volatile std::uintptr_t g_nowhere = 0;

void Judge(const char* name, int got, int want, int& pass) {
    const bool ok = got == want;
    if (ok) ++pass;
    UE_LOGI("run_and_wait_selftest: %s -- answered %d, expected %d: %s", name, got, want, ok ? "ok" : "WRONG");
}

DWORD WINAPI PassThread(LPVOID) {
    int pass = 0;
    Judge("an answer", GT::RunAndWait([](std::atomic<int>& d) { d.store(7); }), 7, pass);
    Judge("no answer", GT::RunAndWait([](std::atomic<int>&) {}), GT::kTaskFaulted, pass);
    Judge("a fault", GT::RunAndWait([](std::atomic<int>& d) {
              *reinterpret_cast<volatile int*>(g_nowhere) = 1;   // the pump's firewall absorbs it
              d.store(1);
          }), GT::kTaskFaulted, pass);
    Judge("a C++ throw", GT::RunAndWait([](std::atomic<int>&) {
              throw std::runtime_error("run_and_wait_selftest: a provoked throw");
          }), GT::kTaskFaulted, pass);
    // The game thread calling the wait on itself: the inner call must answer at once, and the outer
    // body reports what it got as a positive answer (1 = kTaskFaulted came back).
    Judge("a call on the game thread", GT::RunAndWait([](std::atomic<int>& d) {
              const int inner = GT::RunAndWait([](std::atomic<int>& dd) { dd.store(1); });
              d.store(inner == GT::kTaskFaulted ? 1 : 2);
          }), 1, pass);
    if (pass == 5)
        UE_LOGI("run_and_wait_selftest: VERDICT PASS (5/5) -- the ERROR lines 'posted task FAULT', 'posted task "
                "threw a C++ exception' and 'RunAndWait called on the game thread' of this pass are its own, "
                "provoked on purpose");
    else
        UE_LOGW("run_and_wait_selftest: VERDICT FAIL (%d/5) -- read the WRONG lines above", pass);
    return 0;
}

}  // namespace

void Tick() {
    static const bool s_on = coop::config::ResolveFlag(::coop::config_registry::rows::run_and_wait_selftest);
    if (!s_on) return;
    static bool s_started = false;
    if (s_started) return;
    s_started = true;
    UE_LOGI("run_and_wait_selftest: BEGIN -- five endings of one game-thread wait, from a worker thread");
    if (HANDLE h = ::CreateThread(nullptr, 0, &PassThread, nullptr, 0, nullptr))
        ::CloseHandle(h);
    else
        UE_LOGW("run_and_wait_selftest: VERDICT FAIL (0/5) -- the worker thread did not start");
}

}  // namespace coop::dev::run_and_wait_selftest
