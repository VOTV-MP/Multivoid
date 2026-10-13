// coop/interactables/dish_sync.cpp -- see coop/interactables/dish_sync.h.

#include "coop/interactables/dish_sync.h"

#include "coop/config/config.h"
#include "coop/element/lerp_window.h"
#include "coop/interactables/dish_calib_sync.h"
#include "coop/net/session.h"

#include "ue_wrap/desk/console_desk.h"
#include "ue_wrap/desk/dish.h"
#include "ue_wrap/core/log.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>

namespace coop::dish_sync {
namespace {

namespace CD = ue_wrap::console_desk;
namespace D = ue_wrap::dish;

using Clock = std::chrono::steady_clock;

uint64_t NowMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

// The shortest-arc delta in degrees, (-180, 180]: avoids the long-way-round spin (the MTA
// shape).
float OffsetDegrees(float fromDeg, float toDeg) {
    float d = std::fmod(toDeg - fromDeg, 360.f);
    if (d > 180.f)  d -= 360.f;
    if (d < -180.f) d += 360.f;
    return d;
}

bool ProbeLog() {
    static const bool s_enabled = ::coop::config::ResolveFlag(::coop::config_registry::rows::interactable_log);
    return s_enabled;
}

std::atomic<coop::net::Session*> g_session{nullptr};

constexpr auto kPoseInterval = std::chrono::milliseconds(250);   // 4 Hz host sweep
constexpr auto kSlowInterval = std::chrono::milliseconds(1000);  // 1 Hz calib poll + park latch
constexpr int32_t kSettleSweeps = 3;  // full-24 sweeps after MovingCount hits 0

// The interpolation window: 1.2 times the 250 ms host pose sweep interval. Bridges the packet
// cadence smoothly with a 50 ms jitter margin (the player, NPC and ATV streams use 1.5 times
// their cadence); the 75 ms default on a 250 ms stream would interpolate for 75 ms and
// freeze for 175 ms, the stepping bug.
constexpr int kInterpWindowMs = 300;

Clock::time_point g_nextPose{};
Clock::time_point g_nextSlow{};

// The generation key: the desk instance (a mid-session level reload replaces the desk, the
// gamemode and the tickers together, the same shape as the signal catch's desk-instance
// check). All module state resets on a generation change.
void* g_generation = nullptr;

// The host state.
bool g_prevMovingHost[coop::net::kMaxDishes] = {};
bool g_havePrevMovingHost = false;
int32_t g_settleLeft = 0;              // pending settle-tail sweeps
Clock::time_point g_nextSettle{};

// The client state. The wire shadow: what our mirror last wrote (never engine-read). On a
// parked client the moving flag has one writer, our raw mirror write, which starts no
// blueprint loop: a client's ping never succeeds on its own machine (desk_ping_sync).
bool g_shadowMoving[coop::net::kMaxDishes] = {};
// Dishes whose cues the mirror edges touched; the 1 Hz cue reconciler probes only these (the
// pending-latent cue leak class).
uint32_t g_cueWatch = 0;
// The park latch: the parked ticker instance (a fresh instance re-parks).
void* g_parkedDisher = nullptr;

// Per-dish receiver-side interpolation state (game thread only), the same window shape as
// the player and NPC mirrors (position plus angles).
struct DishInterp {
    coop::LerpWindow window;
    float curYaw = 0.f;
    float curRoll = 0.f;
    float targetYaw = 0.f;
    float targetRoll = 0.f;
    float errorYaw = 0.f;
    float errorRoll = 0.f;
    bool  primed = false;
    bool  dirty = false;
};
DishInterp g_dishInterp[coop::net::kMaxDishes] = {};

void ResetModuleState() {
    g_havePrevMovingHost = false;
    g_settleLeft = 0;
    std::memset(g_shadowMoving, 0, sizeof(g_shadowMoving));
    g_cueWatch = 0;
    g_parkedDisher = nullptr;
    for (int32_t i = 0; i < coop::net::kMaxDishes; ++i) {
        g_dishInterp[i] = DishInterp{};
    }
}

// True once per desk-instance change (boot and a mid-session level reload).
bool CheckGeneration() {
    void* inst = CD::Instance();
    if (inst == g_generation) return false;
    g_generation = inst;
    ResetModuleState();
    coop::dish_calib_sync::OnDeskReplaced();
    return true;
}

// Advance a single dish's interpolation window to `now`. Applies the fractional error (the
// MTA linear form); on arrival snaps current to target exactly; pushes to the engine if dirty.
void AdvanceDishInterpSingle(int32_t index, uint64_t now) {
    if (index < 0 || index >= coop::net::kMaxDishes) return;
    auto& d = g_dishInterp[index];
    if (!d.primed || !d.window.IsOpen()) return;

    bool arrived = false;
    const float dAlpha = d.window.Advance(now, &arrived);
    if (dAlpha > 0.f) {
        d.curYaw  += d.errorYaw  * dAlpha;
        d.curRoll += d.errorRoll * dAlpha;
        d.dirty = true;
    }
    if (arrived) {
        d.curYaw  = d.targetYaw;
        d.curRoll = d.targetRoll;
        d.dirty = true;
    }
    if (d.dirty) {
        D::WritePose(index, d.curYaw, d.curRoll);
        d.dirty = false;
    }
}

// The per-frame client drive: advance open dish windows.
void AdvanceDishInterp() {
    const uint64_t now = NowMs();
    for (int32_t i = 0; i < coop::net::kMaxDishes; ++i) AdvanceDishInterpSingle(i, now);
}

// The one row applier (stream rows and snapshot rows): the shadow, the raw moving flag, the
// active dish, the cue edges, then the target, the interpolation or the snap. Game thread
// only.
void ApplyDishRow(int32_t index, bool isMoving, float yawZ, float rollY, bool snap = false) {
    if (index < 0 || index >= coop::net::kMaxDishes) return;
    const bool wasShadow = g_shadowMoving[index];
    g_shadowMoving[index] = isMoving;
    D::WriteIsMoving(index, isMoving);
    D::WriteActiveDish(index, isMoving);
    if (isMoving && !wasShadow) {
        D::ActivateMoveCue(index);
        g_cueWatch |= (1u << index);
        UE_LOGI("[dish] %d '%ls' mirror slew START (yaw=%.1f roll=%.1f)",
                index, D::TechName(index).c_str(), yawZ, rollY);
    } else if (!isMoving && wasShadow) {
        D::DeactivateCues(index);
        UE_LOGI("[dish] %d '%ls' mirror slew STOP (yaw=%.1f roll=%.1f)",
                index, D::TechName(index).c_str(), yawZ, rollY);
    }

    auto& d = g_dishInterp[index];
    const uint64_t now = NowMs();

    // Snap on the first packet, an explicit snap, or steady rest (was not moving and is still not
    // moving); stationary dishes close the window and snap so they do not drift between packets.
    // The falling edge glides into the final pose via advance-before-rebase.
    if (snap || !d.primed || (!isMoving && !wasShadow)) {
        d.curYaw = d.targetYaw = yawZ;
        d.curRoll = d.targetRoll = rollY;
        d.errorYaw = d.errorRoll = 0.f;
        d.window.Close();
        d.primed = true;
        d.dirty = false;
        D::WritePose(index, yawZ, rollY);
        return;
    }

    // Advance before rebase (the MTA set-target-position shape, load-bearing): bring the current
    // values to now using the still-open window's cached error before overwriting the target and
    // recomputing the error.
    AdvanceDishInterpSingle(index, now);

    d.targetYaw = yawZ;
    d.targetRoll = rollY;
    d.errorYaw  = OffsetDegrees(d.curYaw, yawZ);
    d.errorRoll = OffsetDegrees(d.curRoll, rollY);
    d.window.Open(now, kInterpWindowMs);
    d.primed = true;
}

// Apply one incoming pose batch (client).
void ApplyPoseBatch(const coop::net::DishPoseBody& body) {
    for (int32_t i = 0; i < body.count && i < coop::net::kMaxDishes; ++i) {
        const auto& r = body.rows[i];
        if (r.index >= coop::net::kMaxDishes) continue;
        ApplyDishRow(r.index, r.isMoving != 0,
                     coop::net::DequantDeg(r.yawCdeg), coop::net::DequantDeg(r.rollCdeg));
    }
}

// The host: the pose sweep and the settle tail.
void HostPoseSweep(coop::net::Session* s) {
    D::DishRow rows[coop::net::kMaxDishes];
    const int32_t n = D::ReadAllRows(rows, coop::net::kMaxDishes);
    if (n <= 0) return;

    int32_t movingNow = 0;
    coop::net::DishPoseBody body{};
    for (int32_t i = 0; i < n; ++i) {
        const auto& r = rows[i];
        if (r.index < 0 || r.index >= coop::net::kMaxDishes) continue;
        const bool was = g_havePrevMovingHost && g_prevMovingHost[r.index];
        if (r.isMoving) ++movingNow;
        // Host-side identity logs at the native slew edges.
        if (g_havePrevMovingHost && r.isMoving != was) {
            UE_LOGI("[dish] %d '%ls' host slew %s (yaw=%.1f roll=%.1f)",
                    r.index, D::TechName(r.index).c_str(),
                    r.isMoving ? "START" : "STOP", r.yawZ, r.rollY);
        }
        // Movers and falling-edge final rows ride the 4 Hz sweep.
        if ((r.isMoving || was) && body.count < coop::net::kMaxDishes) {
            auto& w = body.rows[body.count++];
            w.index = static_cast<uint8_t>(r.index);
            w.isMoving = r.isMoving ? 1 : 0;
            w.yawCdeg = coop::net::QuantDeg(r.yawZ);
            w.rollCdeg = coop::net::QuantDeg(r.rollY);
        }
        g_prevMovingHost[r.index] = r.isMoving;
    }
    const bool hadPrev = g_havePrevMovingHost;
    g_havePrevMovingHost = true;

    // The settle-tail arming: the moving count just hit zero, so three full sweeps at 1 Hz
    // (self-heals lost falling-edge rows; bounds any stuck client shadow).
    if (hadPrev && movingNow == 0 && body.count > 0) {
        g_settleLeft = kSettleSweeps;
        g_nextSettle = Clock::now() + kSlowInterval;
    }

    if (body.count > 0) s->SetHostDishPose(body);

    if (g_settleLeft > 0 && Clock::now() >= g_nextSettle) {
        --g_settleLeft;
        g_nextSettle = Clock::now() + kSlowInterval;
        coop::net::DishPoseBody full{};
        for (int32_t i = 0; i < n && full.count < coop::net::kMaxDishes; ++i) {
            const auto& r = rows[i];
            if (r.index < 0 || r.index >= coop::net::kMaxDishes) continue;
            auto& w = full.rows[full.count++];
            w.index = static_cast<uint8_t>(r.index);
            w.isMoving = r.isMoving ? 1 : 0;
            w.yawCdeg = coop::net::QuantDeg(r.yawZ);
            w.rollCdeg = coop::net::QuantDeg(r.rollY);
        }
        if (full.count > 0) s->SetHostDishPose(full);
    }
}

// The client: the park latch and the cue reconciler.
void ClientParkLatch() {
    if (void* disher = D::DisherInstance()) {
        if (disher != g_parkedDisher) {
            if (D::ParkDisher(disher)) {
                g_parkedDisher = disher;
                UE_LOGI("dish_sync: client ticker_disher parked (timer cleared)");
            }
        }
    }
    // The cue reconciler: a kill during the phase delay lets the pending latent's one extra pass
    // run the cue after our deactivation, so probe only the dishes we touched.
    if (g_cueWatch == 0) return;
    D::DishRow rows[coop::net::kMaxDishes];
    const int32_t n = D::ReadAllRows(rows, coop::net::kMaxDishes);
    for (int32_t i = 0; i < n; ++i) {
        const auto& r = rows[i];
        if (r.index < 0 || r.index >= coop::net::kMaxDishes) continue;
        if (!(g_cueWatch & (1u << r.index))) continue;
        if (r.isMoving || g_shadowMoving[r.index]) continue;  // legitimately audible
        bool ok = false;
        if (D::AnyCueActive(r.index, ok) && ok) {
            D::DeactivateCues(r.index);
            UE_LOGI("[dish] %d '%ls' cue reconciler: leaked cue deactivated "
                    "(movePow pending-latent class)",
                    r.index, D::TechName(r.index).c_str());
        } else if (ok) {
            g_cueWatch &= ~(1u << r.index);  // clean -- stop watching
        }
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;
    if (!D::EnsureResolved()) return;
    const bool cdUp = CD::EnsureResolved() && CD::Instance();
    if (!cdUp) return;
    if (CheckGeneration()) {
        UE_LOGI("dish_sync: generation change (fresh desk/gamemode) -- state reset");
        return;  // baselines prime next tick
    }

    const auto now = Clock::now();
    const bool host = s->role() == coop::net::Role::Host;

    if (now >= g_nextPose) {
        g_nextPose = now + kPoseInterval;
        if (host && s->connected()) HostPoseSweep(s);
    }

    if (!host && s->connected()) {
        // Drain and apply the latest pose batch (newest wins; apply on new).
        coop::net::DishPoseBody body{};
        bool isNew = false;
        if (s->TryGetHostDishPose(body, &isNew) && isNew) ApplyPoseBatch(body);

        // Drive the mirror pose interpolation unconditionally every frame.
        AdvanceDishInterp();

        // The diagnostic probe log (one aggregate line at most once a second while any dish is
        // moving or interpolating).
        static Clock::time_point s_nextPoseDiag{};
        if (ProbeLog() && now >= s_nextPoseDiag) {
            s_nextPoseDiag = now + std::chrono::seconds(1);
            int32_t interpolating = 0;
            int32_t moving = 0;
            float maxErr = 0.f;
            for (int32_t i = 0; i < coop::net::kMaxDishes; ++i) {
                const auto& d = g_dishInterp[i];
                if (g_shadowMoving[i]) ++moving;
                if (d.primed && d.window.IsOpen()) {
                    ++interpolating;
                    const float errYaw = std::abs(OffsetDegrees(d.curYaw, d.targetYaw));
                    const float errRoll = std::abs(OffsetDegrees(d.curRoll, d.targetRoll));
                    if (errYaw > maxErr) maxErr = errYaw;
                    if (errRoll > maxErr) maxErr = errRoll;
                }
            }
            if (moving > 0 || interpolating > 0) {
                UE_LOGI("[dish] pose-diag: %d interpolating, %d moving (maxErr=%.1f deg)",
                        interpolating, moving, maxErr);
            }
        }
    }

    if (now >= g_nextSlow) {
        g_nextSlow = now + kSlowInterval;
        if (!host) ClientParkLatch();
        if (s->connected()) coop::dish_calib_sync::Poll(s);
    }
}

void SettleForArm() {
    if (!D::EnsureResolved()) return;
    // The host arms only once its dishes stopped. A mirrored mover still moving here is a gate-bypass arm (defence in
    // depth): its flags, its cues and its interpolation window settle, and a warning says so.
    int32_t cleared = 0;
    for (int32_t i = 0; i < coop::net::kMaxDishes; ++i) {
        if (g_shadowMoving[i]) {
            g_shadowMoving[i] = false;
            D::WriteIsMoving(i, false);
            D::DeactivateCues(i);
            ++cleared;
        }
        // Every dish inactive, as on the host: checkFordDishes' gate returns while any dish is active.
        D::WriteActiveDish(i, false);
        auto& d = g_dishInterp[i];
        if (d.primed && d.window.IsOpen()) {
            d.curYaw = d.targetYaw;
            d.curRoll = d.targetRoll;
            d.errorYaw = d.errorRoll = 0.f;
            d.window.Close();
            d.dirty = false;
            D::WritePose(i, d.curYaw, d.curRoll);
        }
    }
    if (cleared > 0)
        UE_LOGW("dish_sync: the arm settled %d mirrored mover(s) still moving -- a gate-bypass arm?", cleared);
}

void OnDishSnapshot(const coop::net::DishSnapshotPayload& p) {
    // From the host alone: the host's receive edge refuses a client's copy (client_authorship.h).
    if (!g_session.load(std::memory_order_acquire)) return;
    if (!D::EnsureResolved()) {
        UE_LOGW("dish_sync: DishSnapshot dropped -- dish surface not yet resolved "
                "(join-window ordering)");
        return;
    }
    const int32_t n = p.count <= coop::net::kMaxDishes ? p.count : coop::net::kMaxDishes;
    for (int32_t i = 0; i < n; ++i) {
        const auto& r = p.rows[i];
        ApplyDishRow(i, r.isMoving != 0,
                     coop::net::DequantDeg(r.yawCdeg), coop::net::DequantDeg(r.rollCdeg),
                     /*snap*/ true);
        // The snapshot's active dishes may differ from the moving flag mid-transition on the host;
        // trust the explicit mask over the row-apply default.
        D::WriteActiveDish(i, r.activeDish != 0);
    }
    UE_LOGI("dish_sync: snapshot applied (%d dishes)", n);
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    if (!D::EnsureResolved()) return;
    D::DishRow rows[coop::net::kMaxDishes];
    const int32_t n = D::ReadAllRows(rows, coop::net::kMaxDishes);
    if (n > 0) {
        coop::net::DishSnapshotPayload p{};
        // Rows are keyed by the gamemode index (the row read compacts over dead entries, so a row's
        // index can exceed its position): fill by index and size the count to the highest filled
        // index plus one.
        int32_t maxIdx = -1;
        for (int32_t i = 0; i < n; ++i) {
            const auto& r = rows[i];
            if (r.index < 0 || r.index >= coop::net::kMaxDishes) continue;
            if (r.index > maxIdx) maxIdx = r.index;
            auto& w = p.rows[r.index];
            w.yawCdeg = coop::net::QuantDeg(r.yawZ);
            w.rollCdeg = coop::net::QuantDeg(r.rollY);
            w.isMoving = r.isMoving ? 1 : 0;
            bool active = false;
            D::ReadActiveDish(r.index, active);
            w.activeDish = active ? 1 : 0;
        }
        p.count = static_cast<uint8_t>(maxIdx + 1);
        if (p.count > 0) {
            s->SendReliableToSlot(peerSlot, coop::net::ReliableKind::DishSnapshot, &p, sizeof(p));
            UE_LOGI("dish_sync: connect snapshot -> slot %d (%u dishes)", peerSlot,
                    static_cast<unsigned>(p.count));
        }
    }
}

void OnDisconnect() {
    // The wire-residue sweep first: clear our mirrored writes (the mirror starts no blueprint
    // loops, so this is state cleanup, not a latent-chain kill).
    if (D::EnsureResolved()) {
        for (int32_t i = 0; i < coop::net::kMaxDishes; ++i) {
            if (!g_shadowMoving[i]) continue;
            D::WriteIsMoving(i, false);
            D::WriteActiveDish(i, false);
            D::DeactivateCues(i);
        }
        // The ticker restore (the suppression loan comes due; every session-end path funnels
        // through this fan-out). Re-look-up instead of trusting the parked pointer: the instance
        // getters run the live-checked cache, so a ticker destroyed in the session-end race
        // declines instead of dispatching into freed memory.
        if (g_parkedDisher) {
            if (void* d = D::DisherInstance()) {
                if (D::RestoreDisher(d))
                    UE_LOGI("dish_sync: ticker_disher restored (native BeginPlay re-arm)");
                else
                    UE_LOGW("dish_sync: ticker_disher restore failed -- its ReceiveBeginPlay could not be called");
            } else {
                UE_LOGW("dish_sync: ticker_disher restore declined -- no live instance");
            }
        }
    }
    g_generation = nullptr;
    ResetModuleState();
    coop::dish_calib_sync::OnSessionEnd();
}

}  // namespace coop::dish_sync
