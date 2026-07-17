// coop/dish_sync.cpp -- see coop/interactables/dish_sync.h.

#include "coop/interactables/dish_sync.h"

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

std::atomic<coop::net::Session*> g_session{nullptr};

constexpr auto kPoseInterval = std::chrono::milliseconds(250);   // 4 Hz host sweep + arm poll
constexpr auto kSlowInterval = std::chrono::milliseconds(1000);  // 1 Hz calib poll + park latch
constexpr int32_t kSettleSweeps = 3;  // full-24 sweeps after MovingCount hits 0

Clock::time_point g_nextPose{};
Clock::time_point g_nextSlow{};

// Generation key: the desk instance (a mid-session level reload replaces the
// desk + gamemode + tickers together -- the signal_catch CheckDeskInstance
// shape). All module state resets on a generation change.
void* g_generation = nullptr;

// ---- host state ------------------------------------------------------------
bool g_prevMovingHost[coop::net::kMaxDishes] = {};
bool g_havePrevMovingHost = false;
int32_t g_settleLeft = 0;              // pending settle-tail sweeps
Clock::time_point g_nextSettle{};
// Arm poll baselines.
bool g_havePrevArm = false;
bool g_prevMeshValid = false;
uint64_t g_prevSignalKey = 0;
int32_t g_prevPolarity = -1;
// v116 root-3: log-once for the DISARM suppression inside the re-init window
// (see HostArmPoll -- state predicate, re-arms per episode).
bool g_reinitWindowLogged = false;

// ---- client state ----------------------------------------------------------
// The wire shadow: what OUR mirror last wrote (never engine-read). A live
// local loop is exactly "local isMoving && !shadow" -- on a parked client,
// isMoving has two writers only: the own ping loop and our raw mirror write
// (which starts no BP loop).
bool g_shadowMoving[coop::net::kMaxDishes] = {};
// Dishes whose cues WE touched (kill sweep / mirror edges) -- the 1 Hz cue
// reconciler only probes these (the movePow pending-latent leak class).
uint32_t g_cueWatch = 0;
// Park latch: the PARKED ticker instances (fresh instances re-park).
void* g_parkedDisher = nullptr;
void* g_parkedUncalib = nullptr;
// ---- shared ----------------------------------------------------------------
// Calibration diff-poll baseline (all peers; primed by snapshot/wire applies).
float g_prevCalib[coop::net::kMaxDishes] = {};
bool g_haveCalibBaseline = false;

void ResetModuleState() {
    g_havePrevMovingHost = false;
    g_settleLeft = 0;
    g_havePrevArm = false;
    g_prevMeshValid = false;
    g_prevSignalKey = 0;
    g_prevPolarity = -1;
    g_reinitWindowLogged = false;
    std::memset(g_shadowMoving, 0, sizeof(g_shadowMoving));
    g_cueWatch = 0;
    g_parkedDisher = nullptr;
    g_parkedUncalib = nullptr;
    g_haveCalibBaseline = false;
}

// True once per desk-instance change (boot + mid-session level reload).
bool CheckGeneration() {
    void* inst = CD::Instance();
    if (inst == g_generation) return false;
    g_generation = inst;
    ResetModuleState();
    return true;
}

uint16_t QuantCalib(float v) {
    if (v < 0.f) v = 0.f;
    if (v > 1.f) v = 1.f;
    return static_cast<uint16_t>(v * 65535.f + 0.5f);
}
float DequantCalib(uint16_t q) { return static_cast<float>(q) / 65535.f; }

// ---- the ONE row applier (stream rows AND snapshot rows) -------------------
// shadow -> pose -> raw isMoving -> activeDishes -> cue edges. GT only.
void ApplyDishRow(int32_t index, bool isMoving, float yawZ, float rollY) {
    if (index < 0 || index >= coop::net::kMaxDishes) return;
    const bool wasShadow = g_shadowMoving[index];
    g_shadowMoving[index] = isMoving;
    D::WritePose(index, yawZ, rollY);
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
}

// Apply one incoming pose batch (client). Skips dishes with a live LOCAL loop.
void ApplyPoseBatch(const coop::net::DishPoseBody& body) {
    D::DishRow rows[coop::net::kMaxDishes];
    const int32_t n = D::ReadAllRows(rows, coop::net::kMaxDishes);
    bool localLive[coop::net::kMaxDishes] = {};
    for (int32_t i = 0; i < n; ++i) {
        const int32_t idx = rows[i].index;
        if (idx >= 0 && idx < coop::net::kMaxDishes)
            localLive[idx] = rows[i].isMoving && !g_shadowMoving[idx];
    }
    static Clock::time_point s_lastSkipLog{};
    for (int32_t i = 0; i < body.count && i < coop::net::kMaxDishes; ++i) {
        const auto& r = body.rows[i];
        if (r.index >= coop::net::kMaxDishes) continue;
        if (localLive[r.index]) {
            // Own-ping pre-kill window -- rate-limited decline log (dead-guard rule).
            const auto now = Clock::now();
            if (now - s_lastSkipLog > std::chrono::seconds(2)) {
                s_lastSkipLog = now;
                UE_LOGI("[dish] %d '%ls' pose row skipped (local live loop, pre-kill window)",
                        static_cast<int>(r.index), D::TechName(r.index).c_str());
            }
            continue;
        }
        ApplyDishRow(r.index, r.isMoving != 0,
                     coop::net::DequantDeg(r.yawCdeg), coop::net::DequantDeg(r.rollCdeg));
    }
}

// ---- host: pose sweep + settle tail ----------------------------------------
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
        // Host-side identity logs at the native slew edges (user rule).
        if (g_havePrevMovingHost && r.isMoving != was) {
            UE_LOGI("[dish] %d '%ls' host slew %s (yaw=%.1f roll=%.1f)",
                    r.index, D::TechName(r.index).c_str(),
                    r.isMoving ? "START" : "STOP", r.yawZ, r.rollY);
        }
        // Movers + falling-edge final rows ride the 4 Hz sweep.
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

    // Settle-tail arming: MovingCount just hit 0 -> 3 full-24 sweeps at 1 Hz
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

// ---- host: the ARM poll (all raw reads; impl-RE SS7 + design D4) -----------
void HostArmPoll(coop::net::Session* s) {
    const bool mesh = CD::DownloadMeshValid();
    uint64_t key = 0;
    float decoded = 0.f;
    int32_t polarity = -1;
    const bool haveKey = CD::ReadDLSignalKey(key);
    const bool haveProg = CD::ReadDownloadProgress(decoded, polarity);
    if (!g_havePrevArm) {
        g_havePrevArm = true;
        g_prevMeshValid = mesh;
        g_prevSignalKey = key;
        g_prevPolarity = polarity;
        return;
    }
    // v116 root-3: a machine RE-INIT (signal_catch catch replay ->
    // ResetDownloadMachine -> LATENT display respawn) makes the mesh
    // transiently invalid while the NEW signalData is already written. A real
    // disarm always deletes the signalData first (the @33832 chain / the
    // kind=1 replay's ClearCoordSignal-then-Reset order) -- so "mesh gone but
    // a signal key present" is the respawn window, not a disarm. Measured
    // cost of not distinguishing (2026-07-17 14:47:58): a false DISARM one
    // second after a client's successful catch reset every peer's machine and
    // deleted the catcher's signal actor. Hold prevMeshValid=TRUE through the
    // window: the eventual respawn then fires the real ARM edge (or, if the
    // mesh returns within one poll, the key change broadcasts arm-over-arm).
    const bool reinitWindow = !mesh && g_prevMeshValid && haveKey && key != 0;
    if (reinitWindow) {
        if (!g_reinitWindowLogged) {
            g_reinitWindowLogged = true;
            UE_LOGI("dish_sync: mesh down with live signalData (key=%llu) -- machine "
                    "re-init window, DISARM suppressed until the display respawns",
                    static_cast<unsigned long long>(key));
        }
        return;  // keep ALL baselines; the window resolves to an ARM edge
    }
    g_reinitWindowLogged = false;
    const bool meshEdge = mesh != g_prevMeshValid;
    const bool keyChange = mesh && haveKey && key != g_prevSignalKey;
    const bool polChange = mesh && haveProg && polarity != g_prevPolarity;
    if (meshEdge || keyChange || polChange) {
        coop::net::DishArmPayload p{};
        p.armed = mesh ? 1 : 0;
        p.decoded = decoded;
        p.polarity = polarity;
        s->SendReliable(coop::net::ReliableKind::DishArm, &p, sizeof(p));
        UE_LOGI("dish_sync: host %s broadcast (decoded=%.1f polarity=%d%s)",
                mesh ? "ARM" : "DISARM", decoded, polarity,
                (!meshEdge && (keyChange || polChange)) ? ", arm-over-arm" : "");
    }
    g_prevMeshValid = mesh;
    g_prevSignalKey = key;
    g_prevPolarity = polarity;
}

// ---- all peers: the symmetric calibration diff-poll (design D5) ------------
void CalibPoll(coop::net::Session* s) {
    D::DishRow rows[coop::net::kMaxDishes];
    const int32_t n = D::ReadAllRows(rows, coop::net::kMaxDishes);
    if (n <= 0) return;
    if (!g_haveCalibBaseline) {
        for (int32_t i = 0; i < n; ++i)
            if (rows[i].index >= 0 && rows[i].index < coop::net::kMaxDishes)
                g_prevCalib[rows[i].index] = rows[i].calibration;
        g_haveCalibBaseline = true;
        return;
    }
    coop::net::DishCalibPayload p{};
    for (int32_t i = 0; i < n; ++i) {
        const auto& r = rows[i];
        if (r.index < 0 || r.index >= coop::net::kMaxDishes) continue;
        if (r.calibration != g_prevCalib[r.index] && p.count < coop::net::kMaxDishes) {
            auto& e = p.entries[p.count++];
            e.index = static_cast<uint8_t>(r.index);
            e.valueQ = QuantCalib(r.calibration);
            g_prevCalib[r.index] = r.calibration;
        }
    }
    if (p.count > 0) {
        s->SendReliable(coop::net::ReliableKind::DishCalib, &p, sizeof(p));
    }
}

// ---- client: park latch + cue reconciler (design D1) -----------------------
void ClientParkLatch() {
    if (void* disher = D::DisherInstance()) {
        if (disher != g_parkedDisher) {
            if (D::ParkDisher(disher)) {
                g_parkedDisher = disher;
                UE_LOGI("dish_sync: client ticker_disher parked (timer cleared)");
            }
        }
    }
    if (void* uncalib = D::UncalibInstance()) {
        if (uncalib != g_parkedUncalib) {
            if (D::ParkUncalib(uncalib)) {
                g_parkedUncalib = uncalib;
                UE_LOGI("dish_sync: client ticker_dishUncalib parked (tick off)");
            }
        }
    }
    // Cue reconciler: a kill during the phase delay lets the pending latent's
    // one extra pass run movePow AFTER our deactivation (impl-RE SS2) -- probe
    // only the dishes WE touched.
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
        if (host && s->connected()) {
            HostPoseSweep(s);
            HostArmPoll(s);
        }
    }

    if (!host && s->connected()) {
        // Drain + apply the latest pose batch (newest-wins; apply on isNew).
        coop::net::DishPoseBody body{};
        bool isNew = false;
        if (s->TryGetHostDishPose(body, &isNew) && isNew) ApplyPoseBatch(body);
    }

    if (now >= g_nextSlow) {
        g_nextSlow = now + kSlowInterval;
        if (!host) ClientParkLatch();
        if (s->connected()) CalibPoll(s);
    }
}

void OnDishArm(const coop::net::DishArmPayload& p, uint8_t senderSlot) {
    (void)senderSlot;  // host-originated; clients trust the transport
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() == coop::net::Role::Host) return;
    if (!D::EnsureResolved() || !CD::EnsureResolved() || !CD::Instance()) {
        UE_LOGW("dish_sync: DishArm dropped -- dish/desk surface not yet resolved "
                "(join-window ordering)");
        return;
    }
    if (!p.armed) {
        // DISARM: the native un-arm parity (reset + display-actor delete).
        CD::ResetDownloadMachine();
        CD::DeleteSignalActor();
        UE_LOGI("dish_sync: DISARM applied (machine reset + signal actor deleted)");
        return;
    }
    // ARM. (1) Pre-clear all mirrored moving state -- the host arm PROVES its
    // dishes settled (the Contains gate guards every formDownload path); a
    // WARN here = a gate-bypass arm (design D4 defense-in-depth).
    int32_t cleared = 0;
    for (int32_t i = 0; i < coop::net::kMaxDishes; ++i) {
        if (!g_shadowMoving[i]) continue;
        g_shadowMoving[i] = false;
        D::WriteIsMoving(i, false);
        D::WriteActiveDish(i, false);
        D::DeactivateCues(i);
        ++cleared;
    }
    if (cleared > 0)
        UE_LOGW("dish_sync: ARM pre-clear wiped %d mirrored mover(s) -- gate-bypass arm?",
                cleared);
    // (2) The native display tail: camera aim + objectRenderer.begin() +
    // signalFound(). Its inner dishesStop -> formDownload(0,-1) rolls a
    // transient local polarity that step (3) overwrites in this same GT task.
    CD::CoordSignal sig;
    if (!CD::ReadCoordSignal(sig) || sig.objectName.empty() || sig.objectName == L"None") {
        UE_LOGW("dish_sync: ARM with no local coord_signalData -- identity row not yet "
                "applied? arming without the display tail");
    } else if (!D::CallCheckFordDishes()) {
        UE_LOGW("dish_sync: checkFordDishes reflected call failed -- arming without the "
                "display tail");
    }
    // (3) The HOST polarity is THE polarity (per-peer RNG at arm -- T2-5d).
    CD::ArmDownloadFromSignal(p.decoded, p.polarity);
    UE_LOGI("dish_sync: ARM applied (decoded=%.1f polarity=%d)", p.decoded, p.polarity);
}

void OnDishSnapshot(const coop::net::DishSnapshotPayload& p, uint8_t senderSlot) {
    (void)senderSlot;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() == coop::net::Role::Host) return;
    if (!D::EnsureResolved()) {
        UE_LOGW("dish_sync: DishSnapshot dropped -- dish surface not yet resolved "
                "(join-window ordering)");
        return;
    }
    const int32_t n = p.count <= coop::net::kMaxDishes ? p.count : coop::net::kMaxDishes;
    for (int32_t i = 0; i < n; ++i) {
        const auto& r = p.rows[i];
        ApplyDishRow(i, r.isMoving != 0,
                     coop::net::DequantDeg(r.yawCdeg), coop::net::DequantDeg(r.rollCdeg));
        // Snapshot activeDishes may differ from isMoving mid-transition on the
        // host -- trust the explicit mask over the row-apply default.
        D::WriteActiveDish(i, r.activeDish != 0);
        const float calib = DequantCalib(r.calibQ);
        D::WriteCalibration(i, calib);
        g_prevCalib[i] = calib;  // prime -- a wire apply must never re-diff
    }
    g_haveCalibBaseline = true;
    UE_LOGI("dish_sync: snapshot applied (%d dishes)", n);
}

void OnDishCalib(const coop::net::DishCalibPayload& p, uint8_t senderSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    if (!D::EnsureResolved()) return;
    const int32_t n = p.count <= coop::net::kMaxDishes ? p.count : coop::net::kMaxDishes;
    for (int32_t i = 0; i < n; ++i) {
        const auto& e = p.entries[i];
        if (e.index >= coop::net::kMaxDishes) continue;
        const float v = DequantCalib(e.valueQ);
        D::WriteCalibration(e.index, v);
        g_prevCalib[e.index] = v;  // apply + prime, GT-atomic (echo-proof)
    }
    if (s->role() == coop::net::Role::Host) {
        // Relay in arrival order = the lane's total order (design D5).
        for (int slot = 1; slot < static_cast<int>(coop::net::kMaxPeers); ++slot) {
            if (slot == senderSlot || !s->IsSlotReady(slot)) continue;
            s->SendReliableToSlot(slot, coop::net::ReliableKind::DishCalib, &p, sizeof(p),
                                  senderSlot > 0 ? senderSlot : 0);
        }
    }
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    if (!D::EnsureResolved()) return;
    D::DishRow rows[coop::net::kMaxDishes];
    const int32_t n = D::ReadAllRows(rows, coop::net::kMaxDishes);
    if (n > 0) {
        coop::net::DishSnapshotPayload p{};
        // Rows are keyed by GAMEMODE INDEX (ReadAllRows compacts over dead
        // entries, so rows[k].index can exceed k) -- fill p.rows[r.index] and
        // size count to the highest filled index + 1.
        int32_t maxIdx = -1;
        for (int32_t i = 0; i < n; ++i) {
            const auto& r = rows[i];
            if (r.index < 0 || r.index >= coop::net::kMaxDishes) continue;
            if (r.index > maxIdx) maxIdx = r.index;
            auto& w = p.rows[r.index];
            w.yawCdeg = coop::net::QuantDeg(r.yawZ);
            w.rollCdeg = coop::net::QuantDeg(r.rollY);
            w.calibQ = QuantCalib(r.calibration);
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
    // The joiner's ARM delivery (replaces the v70 pending-adopt -- RULE 2):
    // ordered AFTER the desk rows + the kind=0 catch row on the same lane.
    if (CD::EnsureResolved() && CD::Instance() && CD::DownloadMeshValid()) {
        coop::net::DishArmPayload a{};
        a.armed = 1;
        CD::ReadDownloadProgress(a.decoded, a.polarity);
        s->SendReliableToSlot(peerSlot, coop::net::ReliableKind::DishArm, &a, sizeof(a));
        UE_LOGI("dish_sync: connect ARM row -> slot %d (decoded=%.1f polarity=%d)",
                peerSlot, a.decoded, a.polarity);
    }
}

void KillOwnPingSlews() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() == coop::net::Role::Host) return;  // host slews natively
    if (!D::EnsureResolved()) return;
    D::DishRow rows[coop::net::kMaxDishes];
    const int32_t n = D::ReadAllRows(rows, coop::net::kMaxDishes);
    int32_t killed = 0, skipped = 0;
    for (int32_t i = 0; i < n; ++i) {
        const auto& r = rows[i];
        if (r.index < 0 || r.index >= coop::net::kMaxDishes) continue;
        if (!r.isMoving) continue;
        if (g_shadowMoving[r.index]) {
            // Mirror-written flag, not a live local loop -- decline + log
            // (dead-guard rule: every exit is visible).
            UE_LOGI("[dish] %d '%ls' kill sweep skip (mirror-moving, not a local loop)",
                    r.index, D::TechName(r.index).c_str());
            ++skipped;
            continue;
        }
        D::StopDish(r.index);          // clean latent-chain death at the @15 gate
        D::DeactivateCues(r.index);    // the stop() stale set (impl-RE SS2)
        D::WriteActiveDish(r.index, false);
        g_cueWatch |= (1u << r.index); // the pending-latent movePow leak class
        ++killed;
    }
    UE_LOGI("dish_sync: own-ping kill sweep -- %d slew(s) killed, %d skipped "
            "(host owns the theater)", killed, skipped);
}

void OnDisconnect() {
    // Wire-residue sweep FIRST: clear OUR mirrored writes (the mirror starts
    // no BP loops, so this is state cleanup, not a latent-chain kill).
    if (D::EnsureResolved()) {
        for (int32_t i = 0; i < coop::net::kMaxDishes; ++i) {
            if (!g_shadowMoving[i]) continue;
            D::WriteIsMoving(i, false);
            D::WriteActiveDish(i, false);
            D::DeactivateCues(i);
        }
        // Ticker restores (the suppression loan comes due -- every session-end
        // path funnels through this fanout). RE-LOOKUP instead of trusting the
        // parked pointer: the instance getters run the live-checked cache
        // (IsLiveByIndex + re-walk on miss), so a ticker destroyed in the
        // session-end race declines instead of UAF-dispatching (the serverbox
        // audit-2026-07-10 restore shape; correctness audit 2026-07-16 F1).
        if (g_parkedDisher) {
            if (void* d = D::DisherInstance()) {
                if (D::RestoreDisher(d))
                    UE_LOGI("dish_sync: ticker_disher restored (native BeginPlay re-arm)");
            } else {
                UE_LOGW("dish_sync: ticker_disher restore declined -- no live instance");
            }
        }
        if (g_parkedUncalib) {
            if (void* u = D::UncalibInstance()) {
                if (D::RestoreUncalib(u))
                    UE_LOGI("dish_sync: ticker_dishUncalib restored (tick on)");
            } else {
                UE_LOGW("dish_sync: ticker_dishUncalib restore declined -- no live instance");
            }
        }
    }
    g_generation = nullptr;
    ResetModuleState();
}

}  // namespace coop::dish_sync
