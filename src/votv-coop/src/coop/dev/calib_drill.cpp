// coop/dev/calib_drill.cpp -- see coop/dev/calib_drill.h.

#include "coop/dev/calib_drill.h"

#include "coop/config/config.h"
#include "coop/interactables/dish_calib_sync.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"  // HasAnnouncedWorldReady

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/desk/console_desk.h"
#include "ue_wrap/desk/dish.h"
#include "ue_wrap/desk/dish_writers.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <cmath>
#include <string>

namespace coop::dev::calib_drill {
namespace {

namespace CD = ue_wrap::console_desk;
namespace CS = coop::dish_calib_sync;
namespace D = ue_wrap::dish;
namespace DW = ue_wrap::dish_writers;
namespace E = ue_wrap::engine;

constexpr int32_t  kHeldDish     = 0;
constexpr int32_t  kUncalDish    = 1;
constexpr int32_t  kToolDish     = 2;
constexpr int32_t  kRefuseDish   = 3;
constexpr float    kHeld         = 0.3131f;  // the host's values, none a value a dish rests at
constexpr float    kUncalHeld    = 0.5757f;
constexpr float    kToolHeld     = 1.4242f;  // outside 0..1: the host's float must reach the client whole
constexpr float    kRefuseHeld   = 0.6262f;
constexpr float    kDeviation    = 0.8686f;  // the client's write into its own copy of dish 0
constexpr float    kToolValue    = 2.5f;     // outside 0..1: the old wire clamped it to 1
constexpr float    kNear         = 0.0005f;
constexpr float    kMeanNear     = 0.005f;   // the deviation moves a 24-dish average by 0.023
constexpr uint64_t kArmBoundMs   = 60000;    // the held values reach the client by a batch or its join seed
constexpr uint64_t kBackBoundMs  = 3000;     // the client's poll runs once a second
constexpr uint64_t kHoldMs       = 3000;     // three of the client's polls
constexpr uint64_t kCheckEveryMs = 250;

enum class Step : uint8_t { Arm, Back, Reader, Uncal, Tool, Refuse, Answer, Hold, Done };
Step     g_step = Step::Arm;
uint64_t g_stepMs = 0;
uint64_t g_nextCheckMs = 0;
uint64_t g_putBackAtDeviation = 0;
int      g_session = 1;
bool     g_hostHeld = false;
bool     g_hostDone = false;

const std::string& Mode() {
    static const std::string s = coop::config::ResolveString(::coop::config_registry::rows::calib_drill);
    return s;
}
bool Join() { return Mode() == "join"; }
bool Enabled() { return Mode() == "run" || Join(); }

bool Expired(uint64_t bound) { return ::GetTickCount64() - g_stepMs > bound; }

// The dish's precision on this copy, or NaN when unread.
float Precision(int32_t index) {
    D::DishCalibration rows[64];
    const int32_t n = D::ReadCalibrations(rows, 64);
    for (int32_t i = 0; i < n; ++i)
        if (rows[i].index == index) return rows[i].value;
    return NAN;
}

// The average of every dish's precision on this copy, setPrec's own sum; NaN when unread.
float MeanPrecision() {
    D::DishCalibration rows[64];
    const int32_t n = D::ReadCalibrations(rows, 64);
    if (n <= 0) return NAN;
    float sum = 0.f;
    for (int32_t i = 0; i < n; ++i) sum += rows[i].value;
    return sum / static_cast<float>(n);
}

bool Near(float v, float to) { return std::fabs(v - to) <= kNear; }

void Fail(const char* what) {
    UE_LOGW("[CALIB-DRILL] FAIL in session %d: %s (dishes 0/1/2/3 read %.4f/%.4f/%.4f/%.4f here)", g_session, what,
            Precision(kHeldDish), Precision(kUncalDish), Precision(kToolDish), Precision(kRefuseDish));
    g_step = Step::Done;
}

void DestroyIfLive(void* actor) {
    if (actor && ue_wrap::reflection::IsLive(actor)) E::DestroyActor(actor);
}

void Abandon(const char* why) {
    UE_LOGW("[CALIB-DRILL] ABANDONED on the client in session %d: %s", g_session, why);
    g_step = Step::Done;
}

void Next(Step s) {
    g_step = s;
    g_stepMs = ::GetTickCount64();
}

// The deviation, and the lane's put-back count as it was written: the leg passes only on a put-back of its own.
void Deviate() {
    g_putBackAtDeviation = CS::LaneCounts().putBack;
    D::WriteCalibration(kHeldDish, kDeviation);
}

// setPrec at once after a second deviation, before any poll can put it back: its average must be the host's -- read
// before the deviation, while the copy held the host's values.
void ReaderLeg() {
    const float held = MeanPrecision();
    Deviate();
    const float deviated = MeanPrecision();
    float mult = NAN;
    if (!D::CallSetPrec() || !CD::ReadPrecMult(mult)) {
        Abandon("setPrec could not be called, or the desk's precision multiplier read");
        return;
    }
    if (!Near(Precision(kHeldDish), kHeld) || std::fabs(mult - held) > kMeanNear) {
        UE_LOGW("[CALIB-DRILL] FAIL in session %d: setPrec averaged %.4f into the multiplier -- the host's average is "
                "%.4f, the deviated one %.4f -- and dish 0 reads %.4f after it", g_session, mult, held, deviated,
                Precision(kHeldDish));
        g_step = Step::Done;
        return;
    }
    UE_LOGI("[CALIB-DRILL] client: setPrec averaged the host's %.4f into the multiplier, not the deviated %.4f", mult,
            deviated);
    Next(Step::Uncal);
}

// The player's hit aimed at dish 1 and an uncalibrator's use run at once, before the next tick's trace replaces the
// hit. This copy's dish 1 is zeroed first, a deviation no poll has put back yet: the verb's entry must judge the body
// against the host's value, or a body that writes exactly the deviation sends nothing. The body zeroes the dish on
// this copy; the lane sends it to the host and holds it.
void UncalLeg(void* player) {
    void* dish = D::DishByIndex(kUncalDish);
    void* comp = D::HitComponent(kUncalDish);
    ue_wrap::FVector at{}, dishAt{};
    if (!dish || !comp || !E::TryGetActorLocation(player, at) || !E::TryGetActorLocation(dish, dishAt)) {
        Abandon("no dish 1 to aim at");
        return;
    }
    void* uncalibrator = DW::SpawnUncalibrator({at.X, at.Y, at.Z + 60.f});
    if (!uncalibrator) {
        Abandon("an uncalibrator could not be spawned");
        return;
    }
    D::WriteCalibration(kUncalDish, 0.f);
    const bool ran = E::WriteMainPlayerHitResult(player, dish, comp, dishAt) &&
                     DW::CallUncalibratorUse(uncalibrator, player);
    E::DestroyActor(uncalibrator);
    if (!ran) {
        Abandon("the player's hit could not be aimed at dish 1, or the uncalibrator's use could not be run");
        return;
    }
    if (!Near(Precision(kUncalDish), 0.f)) {
        Fail("dish 1 does not read 0 on this copy after the uncalibrator's use");
        return;
    }
    UE_LOGI("[CALIB-DRILL] client: the uncalibrator zeroed dish %d here", kUncalDish);
    Next(Step::Tool);
}

// A toolgun's calibration tool run on a dish by index as its RMB does. The tool destroys itself on that path; the
// toolgun, and a tool that took another path, go here.
bool RunTool(void* player, int32_t dish, float value) {
    ue_wrap::FVector at{};
    if (!E::TryGetActorLocation(player, at)) return false;
    void* toolgun = DW::SpawnToolgun({at.X, at.Y, at.Z + 60.f});
    void* tool = DW::SpawnTool(ue_wrap::object_index::ClassByName(DW::kToolClass), {at.X, at.Y, at.Z + 90.f});
    const bool ran = toolgun && tool && DW::CallToolInitByIndex(tool, toolgun, dish, value);
    DestroyIfLive(toolgun);
    DestroyIfLive(tool);
    return ran;
}

// The tool set to a value the old wire clamped away, on dish 2.
void ToolLeg(void* player) {
    if (!RunTool(player, kToolDish, kToolValue)) {
        Abandon("the toolgun's calibration tool could not be spawned or run");
        return;
    }
    if (!Near(Precision(kToolDish), kToolValue)) {
        Fail("the calibration tool did not set dish 2 on this copy");
        return;
    }
    UE_LOGI("[CALIB-DRILL] client: the calibration tool set dish %d to %.4f here", kToolDish, kToolValue);
    Next(Step::Refuse);
}

// The tool set to NaN on dish 3: the host refuses a value that is not finite, and only its answer can put this copy
// back -- the put-back holds the client's own sent value until the host says otherwise.
void RefuseLeg(void* player) {
    if (!RunTool(player, kRefuseDish, NAN)) {
        Abandon("the toolgun's calibration tool could not be spawned or run for the refusal");
        return;
    }
    if (D::Count() <= kRefuseDish || !std::isnan(Precision(kRefuseDish))) {
        Fail("the calibration tool did not write its NaN into dish 3 on this copy");
        return;
    }
    UE_LOGI("[CALIB-DRILL] client: the calibration tool wrote NaN into dish %d here; the host must refuse it",
            kRefuseDish);
    Next(Step::Answer);
}

void ClientTick() {
    const uint64_t now = ::GetTickCount64();
    if (g_step == Step::Done || now < g_nextCheckMs) return;
    g_nextCheckMs = now + kCheckEveryMs;
    void* player = coop::players::Registry::Get().Local();
    switch (g_step) {
    case Step::Arm:
        if (!coop::net_pump::HasAnnouncedWorldReady() ||
            coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle) {
            g_stepMs = now;
            return;
        }
        if (!Near(Precision(kHeldDish), kHeld) || !Near(Precision(kUncalDish), kUncalHeld) ||
            !Near(Precision(kToolDish), kToolHeld) || !Near(Precision(kRefuseDish), kRefuseHeld)) {
            if (Expired(kArmBoundMs)) Abandon("the host's values for dishes 0 to 3 did not reach this client in 60 s");
            return;
        }
        Deviate();
        UE_LOGI("[CALIB-DRILL] client: dishes 0 to 3 hold the host's values; wrote %.4f into this copy of dish %d",
                kDeviation, kHeldDish);
        Next(Step::Back);
        return;
    case Step::Back:
        if (Near(Precision(kHeldDish), kHeld) && CS::LaneCounts().putBack > g_putBackAtDeviation) {
            UE_LOGI("[CALIB-DRILL] client: the lane put dish %d back at the host's %.4f after %llu ms", kHeldDish,
                    kHeld, static_cast<unsigned long long>(now - g_stepMs));
            Next(Step::Reader);
        } else if (Expired(kBackBoundMs)) {
            Fail("dish 0 was not put back to the host's value within 3 s of the deviation");
        }
        return;
    case Step::Reader:
        ReaderLeg();
        return;
    case Step::Uncal:
        if (player) UncalLeg(player);
        return;
    case Step::Tool:
        if (player) ToolLeg(player);
        return;
    case Step::Refuse:
        if (player) RefuseLeg(player);
        return;
    case Step::Answer:
        if (Near(Precision(kRefuseDish), kRefuseHeld)) {
            UE_LOGI("[CALIB-DRILL] client: the host's answer put dish %d back at its %.4f after %llu ms", kRefuseDish,
                    kRefuseHeld, static_cast<unsigned long long>(now - g_stepMs));
            Next(Step::Hold);
        } else if (Expired(kBackBoundMs)) {
            Fail("the host's answer did not put dish 3 back within 3 s of the refused intent");
        }
        return;
    case Step::Hold:
        if (!Near(Precision(kUncalDish), 0.f)) {
            Fail("the uncalibrator's zero on dish 1 was put back");
            return;
        }
        if (!Near(Precision(kToolDish), kToolValue)) {
            Fail("the calibration tool's value on dish 2 was put back");
            return;
        }
        if (!Near(Precision(kHeldDish), kHeld)) {
            Fail("dish 0 left the host's value");
            return;
        }
        if (!Near(Precision(kRefuseDish), kRefuseHeld)) {
            Fail("dish 3 left the host's value after its answer");
            return;
        }
        if (!Expired(kHoldMs)) return;
        UE_LOGI("[CALIB-DRILL] client DONE in session %d (%s): the deviation went back, setPrec read the host's "
                "average, the uncalibrator's zero and the tool's %.1f held, and the refused NaN was answered -- PASS",
                g_session, Mode().c_str(), kToolValue);
        g_step = Step::Done;
        return;
    case Step::Done:
        return;
    }
}

// run: once a client is in its world. join: hosting, before any client connects -- the host's poll runs only once a
// peer is connected, so the values cross with the join itself.
void HostTick(coop::net::Session* s) {
    const uint64_t now = ::GetTickCount64();
    if (g_hostDone || now < g_nextCheckMs) return;
    g_nextCheckMs = now + kCheckEveryMs;
    if (!g_hostHeld) {
        if (Join() ? (!s->running() || s->connected()) : !s->AnyWorldReadyPeer()) {
            if (Join() && s->connected()) {
                g_hostDone = true;
                UE_LOGW("[CALIB-DRILL] ABANDONED on the host in session %d: a client connected before the host "
                        "held its dishes", g_session);
            }
            return;
        }
        if (!D::EnsureResolved() || D::Count() <= kRefuseDish) return;
        D::WriteCalibration(kHeldDish, kHeld);
        D::WriteCalibration(kUncalDish, kUncalHeld);
        D::WriteCalibration(kToolDish, kToolHeld);
        D::WriteCalibration(kRefuseDish, kRefuseHeld);
        g_hostHeld = true;
        UE_LOGI("[CALIB-DRILL] host (%s): dishes 0/1/2/3 held at %.4f/%.4f/%.4f/%.4f; the client's deviation must not "
                "reach dish 0", Mode().c_str(), kHeld, kUncalHeld, kToolHeld, kRefuseHeld);
        return;
    }
    if (Near(Precision(kHeldDish), kDeviation)) {
        g_hostDone = true;
        UE_LOGW("[CALIB-DRILL] FAIL in session %d: the host's dish %d took the client's %.4f", g_session, kHeldDish,
                kDeviation);
        return;
    }
    const CS::Counts c = CS::LaneCounts();
    if (c.intentsApplied < 2 || c.intentsRefused < 1 || !Near(Precision(kUncalDish), 0.f) ||
        !Near(Precision(kToolDish), kToolValue) || !Near(Precision(kRefuseDish), kRefuseHeld))
        return;
    g_hostDone = true;
    UE_LOGI("[CALIB-DRILL] host DONE in session %d (%s): performed the client's two verbs, dish %d = %.4f and dish %d "
            "= %.4f here, and refused its NaN on dish %d, which stays %.4f", g_session, Mode().c_str(), kUncalDish,
            Precision(kUncalDish), kToolDish, Precision(kToolDish), kRefuseDish, Precision(kRefuseDish));
}

}  // namespace

void Tick(coop::net::Session* s) {
    if (!Enabled() || !s) return;
    if (s->role() == coop::net::Role::Host) HostTick(s);
    else if (s->connected()) ClientTick();
}

void OnDisconnect() {
    if (!Enabled()) return;
    ++g_session;
    g_step = Step::Arm;
    g_stepMs = g_nextCheckMs = 0;
    g_hostHeld = g_hostDone = false;
}

}  // namespace coop::dev::calib_drill
