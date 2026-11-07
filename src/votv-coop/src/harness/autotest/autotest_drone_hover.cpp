// harness/autotest/autotest_drone_hover.cpp -- TEST-ONLY: how steadily the delivery drone flies and hovers on each peer.
//
// The host runs its garage console's own verb (drone.triggerFly), so the drone flies. While it is active, each peer
// samples its own drone's location and yaw about every 15 ms for up to two minutes, stamped on the game thread, and
// scores how far each sample sits off the straight path through its neighbours (see Score), over the flight and over
// the hover apart; the client also reads how its mirror played the stream (drone_sync::Stats). A copy whose own
// physics pulls it between the stream's writes sits off its path while it hovers; a playback that runs out of poses
// holds still and then jumps. The host says `rig: VERDICT drone-hover PASS|FAIL|INVALID`: PASS when, hovering, the
// client's copy sits off its path at most twice as far as the host's drone, plus 2 cm and 0.05 degrees, and, flying,
// its playback held past the newest pose in at most 1 frame in 100. VOTVCOOP_DRONE_HOVER_KICK=1 bumps the client's
// hovering copy twice, as a touch would. Gated by VOTVCOOP_RUN_DRONE_HOVER; the peers' folder is VOTVCOOP_CAN_DIR.

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
#include <vector>

namespace harness::autotest {
namespace {

namespace PS = harness::autotest::peer_signal;
namespace DR = ue_wrap::drone;
using disc_drill::OnGameThread;

constexpr const char* kTag = "drone-hover";
constexpr int kStepSec = 120;
constexpr int kSampleMs = 10;
constexpr int kMaxSamples = 8000;  // Sleep(10) runs about 15 ms: two minutes

double NowMs() {
    static LARGE_INTEGER f{};
    if (!f.QuadPart) ::QueryPerformanceFrequency(&f);
    LARGE_INTEGER c{};
    ::QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
}

struct Sample {
    double ms = 0;  // the game thread's clock when it read the pose
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
        s.ms = NowMs();
        s.ok = true;
        s.yaw = rot.Yaw;
        s.active = DR::IsActive(d);
        s.phys = ue_wrap::engine::IsActorRootSimulatingPhysics(d);
    });
    return s;
}

struct Point {
    double ms;
    ue_wrap::FVector loc;
    float yaw;
};


// How far each sample sits from the straight path through its neighbours within 150 ms either side
// (a least-squares line in time per axis), averaged over the flight (smoothed speed at least 50 cm/s)
// and over the hover (below it). A smooth path, flown or held, sits close to its line; a copy that
// jumps at each write, or that its own physics pulls between them, sits off it. Both peers sample on
// their own clock, so the measure needs no shared time.
struct Residual {
    double flyCm = -1, flyDeg = -1, hoverCm = -1, hoverDeg = -1;
    int fly = 0, hover = 0;
};

Residual Score(const std::vector<Point>& d) {
    Residual r;
    double fc = 0, fd = 0, hc = 0, hd = 0;
    size_t j0 = 0, j1 = 0;
    for (size_t i = 0; i < d.size(); ++i) {
        const double t = d[i].ms;
        while (d[j0].ms < t - 150.0) ++j0;
        while (j1 < d.size() && d[j1].ms <= t + 150.0) ++j1;
        const size_t n = j1 - j0;
        if (n < 5) continue;
        double mt = 0;
        for (size_t k = j0; k < j1; ++k) mt += d[k].ms;
        mt /= n;
        double vt = 0;
        for (size_t k = j0; k < j1; ++k) vt += (d[k].ms - mt) * (d[k].ms - mt);
        if (vt <= 0) continue;
        double res[4] = {}, slope[4] = {};
        for (int a = 0; a < 4; ++a) {
            auto val = [&](size_t k) -> double {
                if (a == 0) return d[k].loc.X;
                if (a == 1) return d[k].loc.Y;
                if (a == 2) return d[k].loc.Z;
                return ue_wrap::NormalizeAxis(d[k].yaw - d[i].yaw);  // unwrapped about this sample
            };
            double my = 0;
            for (size_t k = j0; k < j1; ++k) my += val(k);
            my /= n;
            double cov = 0;
            for (size_t k = j0; k < j1; ++k) cov += (d[k].ms - mt) * (val(k) - my);
            slope[a] = cov / vt;
            res[a] = val(i) - (my + slope[a] * (t - mt));
        }
        const double cm = std::sqrt(res[0] * res[0] + res[1] * res[1] + res[2] * res[2]);
        const double speed = std::sqrt(slope[0] * slope[0] + slope[1] * slope[1] + slope[2] * slope[2]) * 1000.0;
        if (speed >= 50.0) { fc += cm; fd += std::fabs(res[3]); ++r.fly; }
        else               { hc += cm; hd += std::fabs(res[3]); ++r.hover; }
    }
    if (r.fly)   { r.flyCm = fc / r.fly; r.flyDeg = fd / r.fly; }
    if (r.hover) { r.hoverCm = hc / r.hover; r.hoverDeg = hd / r.hover; }
    return r;
}

// "fly hover flyCm flyDeg hoverCm hoverDeg phys": the drone's path while it is active on this peer.
// The host's word on Active is the client's clock too, since the client's copy never runs its own flight.
std::string Measure(const char* me, bool host) {
    std::vector<Point> pts;
    pts.reserve(kMaxSamples);
    int phys = 0, idle = 0;
    const bool kick = coop::config::ReadEnv("VOTVCOOP_DRONE_HOVER_KICK") == "1";
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
            if (!pts.empty() && ++idle > 50) break;  // the flight is over
            continue;
        }
        idle = 0;
        if (c.phys) ++phys;
        // VOTVCOOP_DRONE_HOVER_KICK=1: a bump, as a player or a prop touching the hovering copy gives it -- the
        // client spins its copy's body at 720 deg/s and pushes it at 300 cm/s, at 70 s and again at 80 s.
        if (!host && kick && (pts.size() == 4600 || pts.size() == 5300)) {
            bool ok = false;
            OnGameThread([&] {
                if (void* d = DR::Find())
                    ok = ue_wrap::engine::SetActorRootPhysicsVelocity(d, ue_wrap::FVector{300.f, 0.f, 0.f},
                                                                     ue_wrap::FVector{0.f, 0.f, 720.f});
            });
            UE_LOGI("drone-hover: CLIENT bumped its hovering copy (call=%d)", ok ? 1 : 0);
        }
        pts.push_back({c.ms, c.loc, c.yaw});
    }
    // Every sample also goes to <VOTVCOOP_CAN_DIR>/drone_<role>.csv: ms, x, y, z, yaw.
    const std::string dir = coop::config::ReadEnv("VOTVCOOP_CAN_DIR");
    if (FILE* csv = dir.empty() ? nullptr : std::fopen((dir + (host ? "/drone_host.csv" : "/drone_client.csv")).c_str(), "w")) {
        for (const Point& p : pts)
            std::fprintf(csv, "%.2f,%.2f,%.2f,%.2f,%.3f\n", p.ms - pts.front().ms, p.loc.X, p.loc.Y, p.loc.Z, p.yaw);
        std::fclose(csv);
    }
    const Residual r = Score(pts);
    coop::drone_sync::MirrorStats ms{};
    if (!host) OnGameThread([&] { ms = coop::drone_sync::Stats(); });
    UE_LOGI("drone-hover: %hs samples=%d (fly %d, hover %d) offPath fly=%.2fcm/%.3fdeg hover=%.2fcm/%.3fdeg physSamples=%d "
            "mirror played=%u starved=%u delay=%.0fms",
            me, static_cast<int>(pts.size()), r.fly, r.hover, r.flyCm, r.flyDeg, r.hoverCm, r.hoverDeg, phys, ms.played,
            ms.starved, ms.delayMs);
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%d %d %.4f %.4f %.4f %.4f %d %u %u", r.fly, r.hover, r.flyCm, r.flyDeg, r.hoverCm,
                  r.hoverDeg, phys, ms.played, ms.starved);
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
    int hf = 0, hh = 0, hp = 0, cf = 0, ch = 0, cp = 0;
    unsigned hpl = 0, hst = 0, played = 0, starved = 0;
    double h[4] = {}, c[4] = {};
    if (std::sscanf(mine.c_str(), "%d %d %lf %lf %lf %lf %d %u %u", &hf, &hh, &h[0], &h[1], &h[2], &h[3], &hp, &hpl, &hst) != 9 ||
        std::sscanf(theirs.c_str(), "%d %d %lf %lf %lf %lf %d %u %u", &cf, &ch, &c[0], &c[1], &c[2], &c[3], &cp, &played,
                    &starved) != 9 ||
        hh < 100 || ch < 100 || played < 500) {
        coop::rig_verdict::Say(kTag, coop::rig_verdict::Outcome::Invalid,
                               "too few samples (host hover %d, client hover %d, client frames played %u)", hh, ch, played);
        return;
    }
    // Hovering, the client's copy sits off its path at most twice as far as the host's drone, plus 2 cm and 0.05
    // degrees; flying, its playback ran past the newest pose in at most 1 frame in 100. The samples' flight path is
    // said, not judged: a sample lands anywhere in a frame, so a moving copy's path is rough with the sampler alone.
    const bool still = c[2] <= 2.0 * h[2] + 2.0 && c[3] <= 2.0 * h[3] + 0.05;
    const bool fed = starved * 100u <= played;
    coop::rig_verdict::Say(kTag, still && fed ? coop::rig_verdict::Outcome::Pass : coop::rig_verdict::Outcome::Fail,
                           "hovering, off its path: host %.2f cm %.3f deg, client %.2f cm %.3f deg; the client's playback "
                           "held past the newest pose in %u of %u frames; flying, off its path as sampled: host %.2f cm, "
                           "client %.2f cm (physics samples host %d, client %d)",
                           h[2], h[3], c[2], c[3], starved, played, h[0], c[0], hp, cp);
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
