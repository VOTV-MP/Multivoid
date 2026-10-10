// harness/autotest/autotest_wait.cpp -- see WaitPeerWorldReady in harness/autotest.h.

#include "harness/autotest.h"

#include "coop/net/session.h"
#include "harness/session_runtime.h"

namespace harness::autotest {

bool WaitPeerWorldReady(int slot, DWORD budgetMs) {
    constexpr DWORD kPollMs = 50;
    auto& s = harness::session_runtime::Session();
    for (DWORD waited = 0; !(s.IsSlotReady(slot) && s.IsSlotWorldReady(slot)); waited += kPollMs) {
        if (waited >= budgetMs) return false;
        ::Sleep(kPollMs);
    }
    return true;
}

}  // namespace harness::autotest
