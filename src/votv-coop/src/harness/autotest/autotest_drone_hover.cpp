// harness/autotest/autotest_drone_hover.cpp -- TEST-ONLY: how steadily the delivery drone flies and hovers on each peer.
// The host runs its garage console's own verb (drone.triggerFly). While the drone is active each peer samples its
// location and yaw about once a frame until it has hovered a fixed number of samples past the last bump, and scores
// how far each sample sits off the straight path through its neighbours (see Score), over the flight and the hover
// apart; the client also reads how its mirror played the stream. A copy pulled by its own physics sits off its path
// while it hovers; a playback that runs out of poses holds still and then jumps. A copy that coasts sits on its own
// line, so each peer also keeps its hover-start pose and the farthest (cm) and most turned (deg) it sits from it.
// The host says `rig: VERDICT drone-hover PASS|FAIL|INVALID`: PASS when, hovering, the client's copy sits off its
// path at most twice as far as the host's, plus 2 cm and 0.05 deg, its hoverDrift is at most the host's plus 2 cm
// and 0.5 deg, and, flying, its playback held past the newest pose in at most 1 frame in 100. drone_hover_kick
// bumps the client's hovering copy twice (nothing on a copy that does not simulate): the green's evidence is
// physSamples=0 plus the client's drift within the host's (measured: host 34 cm, client 33 cm). drone_hover_body_red
// puts the copy's root back into the simulation first: the verdict FAILs on the drift, at the hover window's end.
// drone_playback_delay_red_ms pins the playback delay: with net.fakelag_ms and net.fakejitter_ms the verdict must FAIL.
// Gated by VOTVCOOP_RUN_DRONE_HOVER; the folder is VOTVCOOP_CAN_DIR.

#include "harness/autotest.h"
#include "disc_drills.h"   // co-located private header (src tree, not include/)
#include "harness/autotest/peer_signal.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
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
constexpr int kMaxSamples = 8000;  // loop iterations: the INVALID backstop only, a run that never completed its window
// The window is counted in samples from the hover's start: the bumps land at kBumpAt[], and the window ends
// kAfterLastBump samples after the last one. 500 samples is 8 s on the host and 13 s on the client, and the starved
// count that tells the arms apart (16 green, 4025 red, over a whole run) is far past its threshold by then.
constexpr int kSpeedWindow = 20;       // samples the hover test looks back over
constexpr double kHoverSpeedCms = 50;  // below this the drone hovers (the same line Score draws)
constexpr size_t kBumpAt[2] = {200, 500};
constexpr size_t kAfterLastBump = 500;

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
    int phys = 0, kicks = 0, slow = 0;
    bool flown = false;      // the drone has been seen flying
    bool complete = false;   // the window ran to its end, not to the iteration cap
    size_t hoverStart = 0;   // index of the first hover sample, once the hover is latched
    bool hovering = false;
    const bool kick = coop::config::ResolveFlag(coop::config_registry::rows::drone_hover_kick);
    const bool redArm = !host && coop::config::ResolveFlag(coop::config_registry::rows::drone_hover_body_red);
    if (redArm) {
        bool ok = false;
        OnGameThread([&] {
            if (void* d = DR::Find()) ok = ue_wrap::engine::SetActorSimulatePhysics(d, true);
        });
        UE_LOGI("drone-hover: RED the client's copy is back in the physics simulation (call=%d)", ok ? 1 : 0);
    }
    for (int i = 0; i < kMaxSamples; ++i) {
        ::Sleep(kSampleMs);
        const Sample c = Read();
        bool active = c.active;
        if (!host) {
            int ha = -1;
            OnGameThread([&] { ha = coop::drone_sync::HostActive(); });
            active = ha == 1;
        }
        if (!c.ok || !active) continue;
        if (c.phys) ++phys;
        pts.push_back({c.ms, c.loc, c.yaw});
        // The drone hovers once it has flown and then stays under kHoverSpeedCms across kSpeedWindow samples.
        if (!hovering && pts.size() > static_cast<size_t>(kSpeedWindow)) {
            const Point& o = pts[pts.size() - 1 - kSpeedWindow];
            const double dx = c.loc.X - o.loc.X, dy = c.loc.Y - o.loc.Y, dz = c.loc.Z - o.loc.Z;
            const double speed = std::sqrt(dx * dx + dy * dy + dz * dz) * 1000.0 / (c.ms - o.ms);
            if (speed >= kHoverSpeedCms) { flown = true; slow = 0; }
            else if (flown && ++slow >= kSpeedWindow) {
                hovering = true;
                hoverStart = pts.size() - kSpeedWindow;
                UE_LOGI("drone-hover: %hs hover began at sample %d", me, static_cast<int>(hoverStart));
            }
        }
        if (!hovering) continue;
        const size_t at = pts.size() - hoverStart;
        // drone_hover_kick: a bump, as a player or a prop touching the hovering copy gives it -- the
        // client spins its copy's body at 720 deg/s and pushes it at 300 cm/s, at the hover's samples kBumpAt.
        if (!host && kick && (at == kBumpAt[0] || at == kBumpAt[1])) {
            bool ok = false;
            OnGameThread([&] {
                if (void* d = DR::Find())
                    ok = ue_wrap::engine::SetActorRootPhysicsVelocity(d, ue_wrap::FVector{300.f, 0.f, 0.f},
                                                                     ue_wrap::FVector{0.f, 0.f, 720.f});
            });
            if (ok) ++kicks;
            UE_LOGI("drone-hover: CLIENT bumped its hovering copy (call=%d)", ok ? 1 : 0);
        }
        if (at >= kBumpAt[1] + kAfterLastBump) { complete = true; break; }
    }
    // Every sample also goes to <VOTVCOOP_CAN_DIR>/drone_<role>.csv: ms, x, y, z, yaw.
    const std::string dir = coop::config::ReadEnv("VOTVCOOP_CAN_DIR");
    FILE* csv = nullptr;
    if (!dir.empty()) fopen_s(&csv, (dir + (host ? "/drone_host.csv" : "/drone_client.csv")).c_str(), "w");
    if (csv) {
        for (const Point& p : pts)
            std::fprintf(csv, "%.2f,%.2f,%.2f,%.2f,%.3f\n", p.ms - pts.front().ms, p.loc.X, p.loc.Y, p.loc.Z, p.yaw);
        std::fclose(csv);
    }
    const Residual r = Score(pts);
    // The farthest this peer's drone sat from its pose at the hover's start, and the most it turned from it.
    double driftCm = -1, driftDeg = -1;
    if (hovering) {
        driftCm = 0;
        driftDeg = 0;
        const Point& hold = pts[hoverStart];
        for (size_t k = hoverStart; k < pts.size(); ++k) {
            const double dx = pts[k].loc.X - hold.loc.X, dy = pts[k].loc.Y - hold.loc.Y, dz = pts[k].loc.Z - hold.loc.Z;
            driftCm = std::fmax(driftCm, std::sqrt(dx * dx + dy * dy + dz * dz));
            driftDeg = std::fmax(driftDeg, std::fabs(ue_wrap::NormalizeAxis(pts[k].yaw - hold.yaw)));
        }
    }
    coop::drone_sync::MirrorStats ms{};
    if (!host) OnGameThread([&] { ms = coop::drone_sync::Stats(); });
    UE_LOGI("drone-hover: %hs samples=%d (fly %d, hover %d) offPath fly=%.2fcm/%.3fdeg hover=%.2fcm/%.3fdeg hoverDrift=%.2fcm/%.3fdeg "
            "physSamples=%d mirror played=%u starved=%u delay=%.0fms",
            me, static_cast<int>(pts.size()), r.fly, r.hover, r.flyCm, r.flyDeg, r.hoverCm, r.hoverDeg, driftCm, driftDeg,
            phys, ms.played, ms.starved, ms.delayMs);
    // The body red puts the copy back in the simulation: a window in which no sample read it simulating never had the red.
    const int redUnapplied = redArm && phys == 0 ? 1 : 0;
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%d %d %.4f %.4f %.4f %.4f %d %u %u %d %d %d %.4f %.4f", r.fly, r.hover, r.flyCm,
                  r.flyDeg, r.hoverCm, r.hoverDeg, phys, ms.played, ms.starved, kicks, complete ? 1 : 0, redUnapplied,
                  driftCm, driftDeg);
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
    int hf = 0, hh = 0, hp = 0, cf = 0, ch = 0, cp = 0, hk = 0, ck = 0, hDone = 0, cDone = 0, hRed = 0, cRed = 0;
    unsigned hpl = 0, hst = 0, played = 0, starved = 0;
    double h[4] = {}, c[4] = {}, hDrift[2] = {}, cDrift[2] = {};
    if (sscanf_s(mine.c_str(), "%d %d %lf %lf %lf %lf %d %u %u %d %d %d %lf %lf", &hf, &hh, &h[0], &h[1], &h[2], &h[3], &hp,
                 &hpl, &hst, &hk, &hDone, &hRed, &hDrift[0], &hDrift[1]) != 14 ||
        sscanf_s(theirs.c_str(), "%d %d %lf %lf %lf %lf %d %u %u %d %d %d %lf %lf", &cf, &ch, &c[0], &c[1], &c[2], &c[3],
                 &cp, &played, &starved, &ck, &cDone, &cRed, &cDrift[0], &cDrift[1]) != 14 ||
        hh < 100 || ch < 100 || played < 500) {
        coop::rig_verdict::Say(kTag, coop::rig_verdict::Outcome::Invalid,
                               "too few samples (host hover %d, client hover %d, client frames played %u)", hh, ch, played);
        return;
    }
    if (cRed) {
        coop::rig_verdict::Say(kTag, coop::rig_verdict::Outcome::Invalid,
                               "the body red was not applied: no sample of the client's copy read it simulating");
        return;
    }
    // The measurement is complete on a peer when its hover window ran out (the last bump landed and kAfterLastBump
    // samples followed), and judged at once; a peer that hit the iteration cap first, or a bump that did not land,
    // leaves nothing to judge.
    if (!hDone || !cDone ||
        (coop::config::ResolveFlag(coop::config_registry::rows::drone_hover_kick) && ck < 2)) {
        coop::rig_verdict::Say(kTag, coop::rig_verdict::Outcome::Invalid,
                               "the window was not completed (host %d, client %d; bumps landed %d)", hDone, cDone,
                               ck);
        return;
    }
    // Hovering, the client's copy sits off its path at most twice as far as the host's drone, plus 2 cm and 0.05
    // degrees; flying, its playback ran past the newest pose in at most 1 frame in 100. The samples' flight path is
    // said, not judged: a sample lands anywhere in a frame, so a moving copy's path is rough with the sampler alone.
    const bool still = c[2] <= 2.0 * h[2] + 2.0 && c[3] <= 2.0 * h[3] + 0.05;
    // A copy that coasts sits on its own straight line, so its drift from the hover's start is judged beside it: at
    // most the host's drift plus 2 cm and 0.5 degrees.
    const bool held = cDrift[0] <= hDrift[0] + 2.0 && cDrift[1] <= hDrift[1] + 0.5;
    const bool fed = starved * 100u <= played;
    coop::rig_verdict::Say(kTag,
                           still && held && fed ? coop::rig_verdict::Outcome::Pass : coop::rig_verdict::Outcome::Fail,
                           "hovering, off its path: host %.2f cm %.3f deg, client %.2f cm %.3f deg; hover drift from the "
                           "hover's start: host %.2f cm %.3f deg, client %.2f cm %.3f deg; the client's playback "
                           "held past the newest pose in %u of %u frames; flying, off its path as sampled: host %.2f cm, "
                           "client %.2f cm (physics samples host %d, client %d)",
                           h[2], h[3], c[2], c[3], hDrift[0], hDrift[1], cDrift[0], cDrift[1], starved, played, h[0],
                           c[0], hp, cp);
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
