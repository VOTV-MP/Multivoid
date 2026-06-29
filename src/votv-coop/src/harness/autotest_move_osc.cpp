// harness/autotest_move_osc.cpp -- TEST-ONLY autonomous local-player movement.
//
// PURPOSE: drive the LOCAL player around a small horizontal circle so the OTHER
// peer's RemotePlayer interpolation has a MOVING source to track. This is the
// verification rig for the interp-starvation fix (2026-06-06): with a STATIC
// source the receiver's `pose-diag[slot N] ... trail=` line is trivially ~0 cm
// (which is exactly why the bug hid in every prior autonomous smoke -- the host
// never moved). With this oscillator the observer's trail reveals the real
// steady-state lag:
//   - BROKEN (pre-fix): the puppet crawls at ~0.68x and trails the moving source
//     by ~1-2.5 s of motion -> trail grows to HUNDREDS of cm (the user's real
//     hand-walked logs showed 69-204 cm).
//   - FIXED (advance-before-rebase): the trail is bounded at ~ speed * window.
//     At the constants below (~235 cm/s on a 75 ms window) that is a CONSTANT
//     ~18 cm -- small and steady, a clean ~10x separation from the broken case.
//
// A CIRCLE (not a back-and-forth) is deliberate: constant tangential speed gives
// a constant trail (easy to read at the 1 Hz pose-diag cadence), and it sweeps
// all directions so it never repeatedly slams one wall.
//
// TEST-ONLY. Never ships -- gated by env VOTVCOOP_RUN_MOVE_OSC, exactly like every
// other VOTVCOOP_RUN_*_TEST probe (RULE 3: dev/test scaffolding, not shipped
// behaviour). Role-agnostic: it just moves whichever peer has the env set; enable
// it on ONE peer and read the OTHER peer's pose-diag. Motion is applied as small
// per-tick absolute SetActorLocation writes (small moves stick; VOTV's player
// constraints silently revert large teleports -- see coop/dev/teleport_client.cpp).
// Game thread only (every engine touch goes through GT::Post).

#include "harness/autotest.h"

#include "coop/player/players_registry.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"
#include "ue_wrap/types.h"

#include <atomic>
#include <cmath>
#include <memory>

namespace harness::autotest {

namespace GT = ue_wrap::game_thread;
namespace E = ue_wrap::engine;

namespace {

// Circle radius + period chosen so the tangential speed (~235 cm/s, a brisk jog)
// is a realistic locomotion speed AND each per-tick step at kHz is small (~5 cm)
// so VOTV never reverts the move. Tangential speed = 2*pi*r / period.
constexpr float kRadiusCm   = 150.f;
constexpr float kPeriodSec  = 4.0f;
constexpr int   kSettleS    = 30;    // wait for the OBSERVER peer to connect + spawn our puppet first
constexpr int   kDurationS  = 45;    // long enough for ~45 pose-diag samples on the observer
constexpr int   kHz         = 50;    // SetActorLocation writes per second
constexpr float kTwoPi      = 6.28318530718f;

}  // namespace

void RunAutonomousMoveOsc() {
    UE_LOGI("move_osc: TEST-ONLY local-player oscillator -- waiting %d s for settle "
            "(session connect + the observer spawning this peer's puppet)", kSettleS);
    ::Sleep(kSettleS * 1000);

    // --- Capture the base position on the game thread (release/acquire handoff:
    // the GT writes base.* THEN flips ready, the worker reads ready THEN base.*) ---
    struct Vec3 { float x, y, z; };
    auto base  = std::make_shared<Vec3>(Vec3{0.f, 0.f, 0.f});
    auto ready = std::make_shared<std::atomic<int>>(0);  // 0 pending, 1 ok, -1 no-local
    GT::Post([base, ready] {
        void* local = coop::players::Registry::Get().Local();
        if (!local) { ready->store(-1, std::memory_order_release); return; }
        const ue_wrap::FVector loc = E::GetActorLocation(local);
        base->x = loc.X; base->y = loc.Y; base->z = loc.Z;
        ready->store(1, std::memory_order_release);
    });
    while (ready->load(std::memory_order_acquire) == 0) ::Sleep(5);
    if (ready->load(std::memory_order_acquire) < 0) {
        UE_LOGW("move_osc: no local mainPlayer_C (not in gameplay yet?) -- aborting");
        return;
    }

    const float speed = kTwoPi * kRadiusCm / kPeriodSec;
    UE_LOGI("move_osc: base=(%.0f,%.0f,%.0f) -- circling r=%.0f cm, period=%.1f s "
            "(~%.0f cm/s) for %d s. Read the OTHER peer's `pose-diag[slot N] ... trail=` line: "
            "bounded ~%.0f cm == FIXED, hundreds of cm == still starved.",
            base->x, base->y, base->z, kRadiusCm, kPeriodSec, speed, kDurationS,
            speed * 0.075f /* trail ~= speed * kInterpWindowMs */);

    // --- Drive the circle: absolute SetActorLocation each tick at base + (r*cos, r*sin). ---
    const int totalTicks = kDurationS * kHz;
    for (int i = 0; i < totalTicks; ++i) {
        const float t     = static_cast<float>(i) / static_cast<float>(kHz);  // seconds
        const float phase = t * (kTwoPi / kPeriodSec);
        const float x = base->x + kRadiusCm * std::cos(phase);
        const float y = base->y + kRadiusCm * std::sin(phase);
        const float z = base->z;
        GT::Post([x, y, z] {
            void* local = coop::players::Registry::Get().Local();
            if (local) E::SetActorLocation(local, ue_wrap::FVector{x, y, z});
        });
        ::Sleep(1000 / kHz);
    }

    UE_LOGI("move_osc: DONE (%d s) -- the observing peer's pose-diag trail over this window "
            "is the verdict for the interp-starvation fix.", kDurationS);
}

DWORD WINAPI MoveOscThread(LPVOID /*arg*/) {
    RunAutonomousMoveOsc();
    return 0;
}

}  // namespace harness::autotest
