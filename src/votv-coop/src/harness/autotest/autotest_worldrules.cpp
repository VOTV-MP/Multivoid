// harness/autotest_worldrules.cpp -- the world-rules probe
// (VOTVCOOP_RUN_WORLDRULES_PROBE): exercises the F1>World>Rules read path on
// both peers + measures G1. Extracted verbatim from harness/autotest.cpp
// (2026-07-19 dissolve); interface + doc in harness/autotest.h.

#include "harness/autotest.h"

#include "ue_wrap/world/game_rules.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"

#include <atomic>
#include <memory>

namespace harness::autotest {
namespace {

namespace GT = ue_wrap::game_thread;

}  // namespace

// --- World-rules probe (VOTVCOOP_RUN_WORLDRULES_PROBE=1) ----------------------
//
// Exercises the EXACT read path behind the F1 > World > Rules panel
// (ue_wrap::game_rules::ReadLocal) on BOTH peers and logs every rule. Two jobs:
//   (1) proves the reflected gameRules read works + doesn't crash (the panel is
//       UI-only, so a headless smoke can't otherwise run its code), and
//   (2) MEASURES G1 -- diff the host's `worldrules:` lines against the client's;
//       equal rule set == the host's rules reached the client's GI.gameRules for
//       free via the save-load spine (the open gate from the settings /qf).
// Waits for gameplay + the client's save-load to settle before reading.
void RunWorldRulesProbe() {
    UE_LOGI("worldrules: probe start (waiting 35 s for gameplay + client save-load settle)");
    ::Sleep(35000);
    auto done = std::make_shared<std::atomic<int>>(0);
    GT::Post([done] {
        ue_wrap::game_rules::Snapshot s;
        if (!ue_wrap::game_rules::ReadLocal(s) || !s.valid) {
            UE_LOGW("worldrules: ReadLocal failed / not valid (GameInstance not up?)");
            done->store(2);
            return;
        }
        UE_LOGI("worldrules: gamemode=%s  (%d rules)", s.gamemodeName.c_str(),
                static_cast<int>(s.fields.size()));
        for (const auto& f : s.fields) {
            switch (f.kind) {
                case ue_wrap::game_rules::Kind::Bool:
                    UE_LOGI("worldrules:   %-28s = %s", f.label.c_str(), f.bval ? "On" : "Off");
                    break;
                case ue_wrap::game_rules::Kind::Float:
                    UE_LOGI("worldrules:   %-28s = %.2f", f.label.c_str(), f.fval);
                    break;
                case ue_wrap::game_rules::Kind::Enum:
                    UE_LOGI("worldrules:   %-28s = #%d", f.label.c_str(), f.ival);
                    break;
            }
        }
        UE_LOGI("worldrules: DONE (diff host vs client `worldrules:` lines for G1)");
        done->store(1);
    });
    while (done->load() == 0) ::Sleep(5);
}

DWORD WINAPI WorldRulesProbeThread(LPVOID /*arg*/) {
    RunWorldRulesProbe();
    return 0;
}

}  // namespace harness::autotest
