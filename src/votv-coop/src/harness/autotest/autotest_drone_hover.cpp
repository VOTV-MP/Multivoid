// harness/autotest/autotest_drone_hover.cpp -- TEST-ONLY: how steadily the delivery drone flies and hovers on each peer.
//
// The host runs its garage console's own verb (drone.triggerFly), so the drone flies. While it is active, each peer
// samples its own drone's location and yaw about every 10 ms for up to 60 s, and sums the jerk of the path: the second
// difference of the location (cm) and of the yaw (degrees) from one sample to the next. A smooth flight has a small sum;
// a copy that a stream writes and its own physics pulls between the writes jerks at every write. Each peer reports its
// mean jerk per sample, whether its drone's root simulates physics, and its sample count. The host says
// `rig: VERDICT drone-hover PASS|FAIL|INVALID`: PASS when the client's mean jerk is at most three times the host's, in
// both location and yaw. Gated by VOTVCOOP_RUN_DRONE_HOVER; the peers' folder is VOTVCOOP_CAN_DIR.

#include "harness/autotest.h"
#include "disc_drills.h"   // co-located private header (src tree, not include/)
#include "harness/autotest/peer_signal.h"

#include "coop/config/config.h"
#include "coop/interactables/drone_sync.h"
#include "coop/session/rig_verdict.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/devices/drone.h"
#include "ue_wrap/devices/drone_console.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <string>

namespace harness::autotest {
namespace {

namespace PS = harness::autotest::peer_signal;
namespace DR = ue_wrap::drone;
using disc_drill::OnGameThread;

constexpr const char* kTag = "drone-hover";
constexpr int kStepSec = 120;
constexpr int kSampleMs = 10;
constexpr int kMaxSamples = 6000;  // 60 s

struct Sample {
    bool ok = false, active = false, phys = false;
    ue_wrap::FVector loc{};
    float yaw = 0.f;
};

Sample Read() {
    Sample s;
    OnGameThread([&] {
        void* d = DR::EnsureResolved() ? DR::Find() : nullptr;
        ue_wrap::FRotator rot{};
        if (!d || !DR::ReadPose(d, s.loc, rot)) return;
        s.ok = true;
        s.yaw = rot.Yaw;
        s.active = DR::IsActive(d);
        s.phys = ue_wrap::engine::IsActorRootSimulatingPhysics(d);
    });
    return s;
}

// "samples meanJerkCm meanJerkDeg phys": the drone's path while it is active on this peer. The host's word on
// Active is the client's clock too, since the client's copy never runs its own flight.
std::string Measure(const char* me, bool host) {
    // Every sample also goes to <VOTVCOOP_CAN_DIR>/drone_<role>.csv: ms, x, y, z, yaw.
    const std::string dir = coop::config::ReadEnv("VOTVCOOP_CAN_DIR");
    FILE* csv = dir.empty() ? nullptr : std::fopen((dir + (host ? "/drone_host.csv" : "/drone_client.csv")).c_str(), "w");
    const DWORD t0 = ::GetTickCount();
    double jerkCm = 0.0, jerkDeg = 0.0;
    int n = 0, phys = 0;
    Sample a, b;
    for (int i = 0; i < kMaxSamples; ++i) {
        ::Sleep(kSampleMs);
        const Sample c = Read();
        bool active = c.active;
        if (!host) {
            int ha = -1;
            OnGameThread([&] { ha = coop::drone_sync::HostActive(); });
            active = ha == 1;
        }
        if (!c.ok || !active) {
            if (n > 50) break;  // the flight is over
            continue;
        }
        if (c.phys) ++phys;
        if (csv) std::fprintf(csv, "%lu,%.2f,%.2f,%.2f,%.3f\n", static_cast<unsigned long>(::GetTickCount() - t0), c.loc.X, c.loc.Y, c.loc.Z, c.yaw);
        if (a.ok && b.ok) {
            const double ddx = c.loc.X - 2.0 * b.loc.X + a.loc.X;
            const double ddy = c.loc.Y - 2.0 * b.loc.Y + a.loc.Y;
            const double ddz = c.loc.Z - 2.0 * b.loc.Z + a.loc.Z;
            jerkCm += std::sqrt(ddx * ddx + ddy * ddy + ddz * ddz);
            const double y1 = ue_wrap::NormalizeAxis(c.yaw - b.yaw), y0 = ue_wrap::NormalizeAxis(b.yaw - a.yaw);
            jerkDeg += std::fabs(y1 - y0);
            ++n;
        }
        a = b;
        b = c;
    }
    if (csv) std::fclose(csv);
    const double mCm = n ? jerkCm / n : -1.0, mDeg = n ? jerkDeg / n : -1.0;
    UE_LOGI("drone-hover: %hs samples=%d meanJerkCm=%.3f meanJerkDeg=%.3f physSamples=%d", me, n, mCm, mDeg, phys);
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%d %.4f %.4f %d", n, mCm, mDeg, phys);
    return buf;
}

void RunClient() {
    if (!disc_drill::HostPuppetUp()) return;
    PS::Mark("client_ready");
    if (!PS::Await("flying", kStepSec)) return;
    PS::Mark("client_result", Measure("CLIENT", false));
    PS::Await("host_done", kStepSec);
    UE_LOGI("drone-hover: DONE on client");
}

void RunHost() {
    if (!disc_drill::HostWorldUp()) return;
    if (!PS::Await("client_ready", 480)) return;
    bool flew = false;
    OnGameThread([&] {
        void* consoles[4] = {};
        const int32_t n = ue_wrap::drone_console::LiveConsoles(consoles, 4);
        flew = n > 0 && ue_wrap::drone_console::TriggerFly(consoles[0]);
    });
    UE_LOGI("drone-hover: HOST ran the console's fly verb (call=%d)", flew ? 1 : 0);
    if (!PS::WaitFor("the host's drone active", 20, [] { return Read().active; })) return;
    PS::Mark("flying");
    const std::string mine = Measure("HOST", true);
    std::string theirs;
    PS::AwaitText("client_result", theirs, kStepSec);
    PS::Mark("host_done");
    int hn = 0, cn = 0, hp = 0, cp = 0;
    double hc = 0, hd = 0, cc = 0, cd = 0;
    if (std::sscanf(mine.c_str(), "%d %lf %lf %d", &hn, &hc, &hd, &hp) != 4 ||
        std::sscanf(theirs.c_str(), "%d %lf %lf %d", &cn, &cc, &cd, &cp) != 4 || hn < 100 || cn < 100) {
        coop::rig_verdict::Say(kTag, coop::rig_verdict::Outcome::Invalid, "too few samples (host %d, client %d)", hn, cn);
        return;
    }
    const double eps = 0.01;
    const bool pass = cc <= 3.0 * hc + eps && cd <= 3.0 * hd + eps;
    coop::rig_verdict::Say(kTag, pass ? coop::rig_verdict::Outcome::Pass : coop::rig_verdict::Outcome::Fail,
                           "mean jerk per sample: host %.3f cm %.3f deg (%d samples, physics %d), client %.3f cm %.3f deg "
                           "(%d samples, physics %d)",
                           hc, hd, hn, hp, cc, cd, cn, cp);
    UE_LOGI("drone-hover: DONE on host");
}

}  // namespace

DWORD WINAPI DroneHoverThread(LPVOID /*arg*/) {
    if (!PS::Begin(kTag)) return 0;
    if (!IsClientRole()) RunHost();
    else RunClient();
    return 0;
}

}  // namespace harness::autotest
