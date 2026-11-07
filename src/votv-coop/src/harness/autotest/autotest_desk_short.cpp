// harness/autotest/autotest_desk_short.cpp -- TEST-ONLY: the base computer's short circuit on both peers.
//
// One peer (the client, or the host with VOTVCOOP_DESK_SHORT_WHO=host) steps beside the desk and shorts its own desk the
// way a liquid's hit does:
// the desk's own shortCircuit, as grimeProjectile_C and the sponge bucket's pour call it. Each peer reads whether its
// desk's `shortCircuiting` rose within 5 s, and, 16 s after the short (it ends in about ten seconds with the base on
// solar power), the power panel's lever mask. The host says `rig: VERDICT desk-short PASS|FAIL|INVALID`: PASS when both
// desks shorted and both panels read the same mask, changed from the mask before. Gated by VOTVCOOP_RUN_DESK_SHORT; the
// peers' folder is VOTVCOOP_CAN_DIR.

#include "harness/autotest.h"
#include "disc_drills.h"   // co-located private header (src tree, not include/)
#include "harness/autotest/peer_signal.h"

#include "coop/config/config.h"
#include "coop/player/players_registry.h"
#include "coop/session/rig_verdict.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/desk/console_desk.h"
#include "ue_wrap/desk/desk_short.h"
#include "ue_wrap/devices/power_control.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <cstdio>
#include <string>

namespace harness::autotest {
namespace {

namespace PS = harness::autotest::peer_signal;
namespace PC = ue_wrap::power_control;
using disc_drill::OnGameThread;

constexpr const char* kTag = "desk-short";
constexpr int kStepSec = 120;

bool HostActs() { return coop::config::ReadEnv("VOTVCOOP_DESK_SHORT_WHO") == "host"; }

void* Desk() { return ue_wrap::console_desk::EnsureResolved() ? ue_wrap::console_desk::Instance() : nullptr; }

int Shorting() {
    int v = -1;
    OnGameThread([&] {
        bool on = false;
        if (ue_wrap::desk_short::ReadShortCircuiting(Desk(), on)) v = on ? 1 : 0;
    });
    return v;
}

int PowerMask() {
    int m = -1;
    OnGameThread([&] {
        uint8_t mask = 0;
        void* p = PC::EnsureResolved() ? PC::Panel() : nullptr;
        if (p && PC::ReadPress(p, mask)) m = mask;
    });
    return m;
}

// The actor's short, by the desk's own event, from beside the desk (a liquid is thrown from the room; the host checks
// the sender's reach).
void Short(const char* me) {
    bool moved = false;
    OnGameThread([&] {
        void* local = coop::players::Registry::Get().Local();
        ue_wrap::FVector at{};
        if (local && ue_wrap::engine::TryGetActorLocation(Desk(), at))
            moved = ue_wrap::engine::TeleportTo(local, ue_wrap::FVector{at.X + 150.f, at.Y, at.Z + 60.f}, ue_wrap::FRotator{});
    });
    ::Sleep(3000);  // the pose reaches the other peer
    UE_LOGI("desk-short: %hs stands beside the desk (teleport=%d)", me, moved ? 1 : 0);
    bool ran = false;
    OnGameThread([&] { ran = ue_wrap::desk_short::CallShortCircuit(Desk()); });
    UE_LOGI("desk-short: %hs shorted its desk as a liquid's hit does (call=%d)", me, ran ? 1 : 0);
}

// "shorted before after" on this peer: whether the desk shorted within 5 s, and the power mask before and after.
std::string Watch(const char* me, bool acts, int before) {
    if (acts) Short(me);
    else PS::Await("acted", kStepSec);
    int shorted = 0;
    for (int i = 0; i < 50 && !shorted; ++i) {
        if (Shorting() == 1) shorted = 1;
        else ::Sleep(100);
    }
    UE_LOGI("desk-short: %hs desk shorted=%d", me, shorted);
    if (acts) PS::Mark("acted");
    ::Sleep(16000);
    const int after = PowerMask();
    UE_LOGI("desk-short: %hs power mask before=%d after=%d", me, before, after);
    return std::to_string(shorted) + " " + std::to_string(before) + " " + std::to_string(after);
}

void RunClient() {
    if (!disc_drill::HostPuppetUp()) return;
    const int before = PowerMask();
    PS::Mark("client_ready");
    if (!PS::Await("host_ready", kStepSec)) return;
    PS::Mark("client_result", Watch("CLIENT", !HostActs(), before));
    PS::Await("host_done", kStepSec);
    UE_LOGI("desk-short: DONE on client");
}

void RunHost() {
    if (!disc_drill::HostWorldUp()) return;
    if (!PS::Await("client_ready", 480)) return;
    const int before = PowerMask();
    PS::Mark("host_ready");
    const std::string mine = Watch("HOST", HostActs(), before);
    std::string theirs;
    PS::AwaitText("client_result", theirs, kStepSec);
    PS::Mark("host_done");
    int hs = 0, hb = -1, ha = -1, cs = 0, cb = -1, ca = -1;
    if (std::sscanf(mine.c_str(), "%d %d %d", &hs, &hb, &ha) != 3 || std::sscanf(theirs.c_str(), "%d %d %d", &cs, &cb, &ca) != 3 ||
        ha < 0 || ca < 0) {
        coop::rig_verdict::Say(kTag, coop::rig_verdict::Outcome::Invalid, "a peer's desk or power panel did not read");
        return;
    }
    const bool pass = hs && cs && ha == ca && ha != hb;
    coop::rig_verdict::Say(kTag, pass ? coop::rig_verdict::Outcome::Pass : coop::rig_verdict::Outcome::Fail,
                           "the %s's short: the host's desk shorted=%d, the client's=%d; power mask host %d -> %d, client %d -> %d",
                           HostActs() ? "host" : "client", hs, cs, hb, ha, cb, ca);
    UE_LOGI("desk-short: DONE on host");
}

}  // namespace

DWORD WINAPI DeskShortThread(LPVOID /*arg*/) {
    if (!PS::Begin(kTag)) return 0;
    if (!IsClientRole()) RunHost();
    else RunClient();
    return 0;
}

}  // namespace harness::autotest
