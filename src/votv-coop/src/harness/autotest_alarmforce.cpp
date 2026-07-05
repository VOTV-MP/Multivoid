// harness/autotest_alarmforce.cpp -- base radar alarm lane e2e driver (v101, docs/events/alarm.md).
//
// HOST-ONLY. After a client-settle window, forces the native trigger_alarm_C ON through
// alarm_sync::DevForce (a game-thread runTrigger(1) -- the exact call the radar makes), waits,
// then forces OFF. The lane's own 1 Hz poll must detect both transitions and broadcast; the
// assert is per-axis DISCRIMINATING (the state bit + the applied line on the RECEIVING peer,
// not an arrival line -- [[lesson-e2e-assert-must-discriminate-the-axis]]). Expect:
//   host log:   "alarm_sync: applied active=1 (native runTrigger replay)"
//               "alarm_sync: host broadcast active=1"   (poll edge -> wire)
//   client log: "alarm_sync: applied active=1 (native runTrigger replay)"
//   ...then the same pair with active=0.
// A mid-alarm JOIN leg instead gates the client launch on the host's "broadcast active=1"
// line and expects "alarm_sync: connect-snapshot -- sent active=1 to slot 1" + one client apply.
//
// Gated by env VOTVCOOP_RUN_ALARMFORCE_TEST=1 (autonomous mp.py only; not an ini flag).

#include "harness/autotest.h"

#include "coop/world/alarm_sync.h"
#include "harness/config.h"
#include "ue_wrap/log.h"

#include <windows.h>

#include <string>

namespace harness::autotest {

void RunAutonomousAlarmForceTest() {
    const std::string roleEnv = harness::config::ReadEnv("VOTVCOOP_NET_ROLE");
    if (roleEnv == "client") {
        UE_LOGI("alarmforce_test: not host -- this routine is host-only (client observes via wire)");
        return;
    }
    // Settle: host world up (the client may still be mid save-transfer -- deliberate: an ON
    // landing pre-world-ready exercises the JOIN answer, the connect snapshot delivers it).
    // The 45 s hold then guarantees the client is world-ready for the LIVE OFF transition.
    UE_LOGI("alarmforce_test: starting on host (55 s settle, then ON -> 45 s hold -> OFF)");
    ::Sleep(55000);

    coop::alarm_sync::DevForce(true);
    UE_LOGI("alarmforce_test: posted runTrigger(1) -- expect host 'applied active=1' + "
            "'host broadcast active=1', then the client 'applied active=1' (live or via "
            "the connect snapshot if it was still loading)");
    ::Sleep(45000);

    coop::alarm_sync::DevForce(false);
    UE_LOGI("alarmforce_test: posted runTrigger(0) -- expect the same pair with active=0");
}

DWORD WINAPI AlarmForceTestThread(LPVOID /*arg*/) {
    RunAutonomousAlarmForceTest();
    return 0;
}

}  // namespace harness::autotest
