// coop/signal_catch_sync.cpp -- see coop/signal_catch_sync.h.

#include "coop/signal_catch_sync.h"

#include "coop/console_state_sync.h"
#include "coop/device_occupancy.h"
#include "coop/net/session.h"

#include "ue_wrap/console_desk.h"
#include "ue_wrap/dish.h"
#include "ue_wrap/log.h"
#include "ue_wrap/space_renderer.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace coop::signal_catch_sync {
namespace {

namespace CD = ue_wrap::console_desk;
namespace SR = ue_wrap::space_renderer;
namespace D = ue_wrap::dish;

using Clock = std::chrono::steady_clock;

std::atomic<coop::net::Session*> g_session{nullptr};

constexpr auto kPoll = std::chrono::milliseconds(1000);
constexpr auto kRecentTTL = std::chrono::seconds(5);
const wchar_t* const kDeskClaim = L"desk";

struct Identity { float x = 0, y = 0, z = 0, frequency = 0; };

bool IdentityEq(const Identity& a, const Identity& b) {
    // Exact float equality: every non-catcher copy is wire-byte-identical
    // (the v52 eid lesson -- never fuzzy-match identities).
    return a.x == b.x && a.y == b.y && a.z == b.z && a.frequency == b.frequency;
}

Clock::time_point g_nextPoll{};
bool g_announced = false;

// Last-poll detector inputs.
std::vector<Identity> g_prevRows;
bool g_havePrevRows = false;
int32_t g_prevMoving = -1;
std::wstring g_prevSigName;   // coord_signalData.objectName at the last poll
bool g_havePrevSig = false;
// The desk instance the baselines were read from. A mid-session level reload
// spawns a FRESH desk whose runtime-only coord_signalData starts at 'None' --
// a module-static baseline surviving that would read armed-vs-None as a
// player pressing the delete button and broadcast a spurious kind=1 (clears
// every peer's download; correctness audit 2026-06-12 finding 1). Same
// instance-tracking shape as console_state's g_suppressedOn.
void* g_deskInst = nullptr;

// Recently-caught identities (catcher at send, host + receivers at apply):
// filters stale in-flight SkySignalState snapshot rows + detector re-fires.
struct Recent { Identity id; Clock::time_point at; };
std::vector<Recent> g_recent;

// The joiner's download-progress catch-up (see the header).
struct PendingAdopt {
    bool armed = false;
    float decoded = 0;
    int32_t polarity = -1;
    float resDetect = 0;
    Clock::time_point at{};  // set time -- see the kind=0 staleness void below
};
PendingAdopt g_pending;

// Re-baseline the detectors against the CURRENT desk instance without firing
// anything (fresh-instance prime + the explicit re-prime after a replay).
void PrimeBaselinesFromDesk() {
    CD::CoordSignal sig;
    if (CD::ReadCoordSignal(sig)) {
        g_prevSigName = sig.objectName;
        g_havePrevSig = true;
    } else {
        g_prevSigName.clear();
        g_havePrevSig = false;
    }
    g_prevRows.clear();
    g_havePrevRows = false;
    g_prevMoving = -1;
}

// True once per desk-instance change (boot + mid-session level reload):
// re-primes the baselines so stale state from the previous world can never
// read as a player action on the fresh one.
bool CheckDeskInstance() {
    void* inst = CD::Instance();
    if (inst == g_deskInst) return false;
    g_deskInst = inst;
    PrimeBaselinesFromDesk();
    return true;
}

bool IsNoneName(const std::wstring& n) { return n.empty() || n == L"None"; }

void RegisterRecent(const Identity& id) {
    g_recent.push_back({ id, Clock::now() });
}

bool IsRecent(const Identity& id) {
    for (const auto& r : g_recent)
        if (IdentityEq(r.id, id)) return true;
    return false;
}

void PruneRecent() {
    const auto now = Clock::now();
    for (auto it = g_recent.begin(); it != g_recent.end();) {
        if (now - it->at > kRecentTTL) it = g_recent.erase(it);
        else ++it;
    }
}

void FillRowFromCoordSignal(const CD::CoordSignal& sig, coop::net::WireSkySignal& w) {
    std::memset(&w, 0, sizeof(w));
    w.x = sig.x; w.y = sig.y; w.z = sig.z;
    w.type = sig.type;
    w.strength = sig.strength;
    w.frequency = sig.frequency;
    w.frequencySpread = sig.frequencySpread;
    w.polarity = sig.polarity;
    w.polaritySpread = sig.polaritySpread;
    // alpha/lifetimes/direction stay zero -- the row is being deleted;
    // receivers only consume the struct content.
    size_t n = 0;
    for (; n < sig.objectName.size() && n < sizeof(w.objectName); ++n)
        w.objectName[n] = static_cast<char>(sig.objectName[n]);  // rolled names are ASCII
    w.nameLen = static_cast<uint8_t>(n);
}

CD::CoordSignal CoordSignalFromRow(const coop::net::WireSkySignal& w) {
    CD::CoordSignal sig;
    sig.x = w.x; sig.y = w.y; sig.z = w.z;
    sig.type = w.type;
    sig.strength = w.strength;
    sig.frequency = w.frequency;
    sig.frequencySpread = w.frequencySpread;
    sig.polarity = w.polarity;
    sig.polaritySpread = w.polaritySpread;
    const size_t n = w.nameLen <= sizeof(w.objectName) ? w.nameLen : sizeof(w.objectName);
    sig.objectName.assign(w.objectName, w.objectName + n);
    return sig;
}

// Send a built payload toward the rest of the session. CLIENT -> the host
// (slot 0) for validation; HOST -> every ready client except `exceptSlot`
// (the origin), stamping the logical sender so receivers see who acted.
void SendOut(coop::net::Session* s, const coop::net::SkySignalCatchPayload& p,
             int exceptSlot) {
    if (s->role() == coop::net::Role::Client) {
        s->SendReliableToSlot(0, coop::net::ReliableKind::SkySignalCatch, &p, sizeof(p));
        return;
    }
    for (int slot = 1; slot < static_cast<int>(coop::net::kMaxPeers); ++slot) {
        if (slot == exceptSlot || !s->IsSlotReady(slot)) continue;
        s->SendReliableToSlot(slot, coop::net::ReliableKind::SkySignalCatch, &p, sizeof(p),
                              exceptSlot > 0 ? static_cast<uint8_t>(exceptSlot) : 0);
    }
}

// The receiver-side replay of the SP consume chain (everything the catcher's
// native run did, minus the log line -- DeskLogLine carries that from the
// catcher's own terminal diff).
void ApplyReplay(const coop::net::SkySignalCatchPayload& p) {
    if (p.kind == 1) {
        // The @33832-34222 'Signal data deleted' chain.
        CD::ClearCoordSignal();
        CD::ResetDownloadMachine();
        g_pending.armed = false;  // a cleared signal voids any queued adopt
        g_prevSigName = L"None";  // prime: the wire apply must not re-fire the edge
        g_havePrevSig = true;
        coop::console_state_sync::PrimeDeskEdgeDetector();
        UE_LOGI("signal_catch: cleared replay applied (download machine reset)");
        return;
    }
    const CD::CoordSignal sig = CoordSignalFromRow(p.row);
    // A pending adopt belongs to the signal armed at JOIN time; the join's
    // own kind=0 follows its desk adopt within the same connect replay
    // (milliseconds), so a pending older than this is a leftover whose arm
    // never happened -- void it rather than true-up a NEW catch with stale
    // progress (correctness audit 2026-06-12 finding 4).
    if (g_pending.armed && Clock::now() - g_pending.at > std::chrono::seconds(60))
        g_pending = {};
    CD::WriteCoordSignal(sig);                                  // @79303
    SR::RemoveSignalByIdentity(sig.x, sig.y, sig.z, sig.frequency);  // @9337
    CD::ResetDownloadMachine();                                 // @10050/@10244
    CD::PlayPingSuccess();                                      // @10027 (presence)
    if (p.slewValid) {
        const int32_t n = D::StartMovingAll(
            ue_wrap::FVector{ p.slewX, p.slewY, p.slewZ });     // @9494 fan-out
        UE_LOGI("signal_catch: catch replay applied ('%ls', %d dish(es) slewing)",
                sig.objectName.c_str(), n);
    } else {
        // No dish carried the slew (joiner replay after the dishes settled):
        // skip the theater, arm the downloader directly -- the native
        // dishesStop arm is formDownload(0, -1).
        CD::ArmDownloadFromSignal(0.f, -1);
        UE_LOGW("signal_catch: catch replay with slewValid=0 ('%ls') -- "
                "downloader armed directly, dish theater skipped",
                sig.objectName.c_str());
    }
    g_prevSigName = sig.objectName;  // prime the cleared detector
    g_havePrevSig = true;
    coop::console_state_sync::PrimeDeskEdgeDetector();
}

bool BuildCatchPayload(const CD::CoordSignal& sig, uint8_t kind,
                       coop::net::SkySignalCatchPayload& p) {
    std::memset(&p, 0, sizeof(p));
    p.kind = kind;
    if (kind == 1) return true;  // cleared: row + slew ignored
    FillRowFromCoordSignal(sig, p.row);
    ue_wrap::FVector slew{};
    if (D::ReadSlewFromMovingDish(slew)) {
        p.slewX = slew.X; p.slewY = slew.Y; p.slewZ = slew.Z;
        p.slewValid = 1;
    }
    return true;
}

// The catch detector body, shared by the 1 Hz Tick and the snapshot-arrival
// race check. Takes the CURRENT poll inputs, compares against the previous
// poll's, fires the relay, and rolls the previous-poll state forward.
void RunDetectors(coop::net::Session* s, const CD::CoordSignal& sig, bool haveSig,
                  const std::vector<Identity>& curRows, bool haveRows,
                  int32_t curMoving) {
    if (s && s->connected() && haveSig) {
        // ---- CLEARED (any peer; unclaimed trust like the physical button) --
        if (g_havePrevSig && !IsNoneName(g_prevSigName) && IsNoneName(sig.objectName)) {
            coop::net::SkySignalCatchPayload p{};
            BuildCatchPayload(sig, 1, p);
            SendOut(s, p, /*exceptSlot*/ -1);  // client -> host; host -> all clients
            UE_LOGI("signal_catch: local 'Signal data deleted' relayed");
        }
        // ---- CATCH (claim holder only; dish rising edge = success signature)
        if (haveRows && g_havePrevRows && !IsNoneName(sig.objectName) &&
            coop::device_occupancy::LocalHolds(kDeskClaim)) {
            const bool dishEdge = curMoving >= 0 && g_prevMoving >= 0 &&
                                  curMoving > g_prevMoving;
            if (dishEdge) {
                const Identity id{ sig.x, sig.y, sig.z, sig.frequency };
                bool inPrev = false, inCur = false;
                for (const auto& r : g_prevRows)
                    if (IdentityEq(r, id)) { inPrev = true; break; }
                for (const auto& r : curRows)
                    if (IdentityEq(r, id)) { inCur = true; break; }
                if (inPrev && !inCur && !IsRecent(id)) {
                    coop::net::SkySignalCatchPayload p{};
                    BuildCatchPayload(sig, 0, p);
                    RegisterRecent(id);
                    SendOut(s, p, /*exceptSlot*/ -1);  // host: all clients; client: the host
                    UE_LOGI("signal_catch: local catch detected ('%ls' at %.0f,%.0f,%.0f; "
                            "slewValid=%u) -- relayed",
                            sig.objectName.c_str(), sig.x, sig.y, sig.z,
                            static_cast<unsigned>(p.slewValid));
                }
            }
        }
    }
    // Roll the previous-poll state forward.
    if (haveSig) { g_prevSigName = sig.objectName; g_havePrevSig = true; }
    if (haveRows) { g_prevRows = curRows; g_havePrevRows = true; }
    if (curMoving >= 0) g_prevMoving = curMoving;
}

std::vector<Identity> RowsToIdentities(const std::vector<SR::SignalRow>& rows) {
    std::vector<Identity> ids;
    ids.reserve(rows.size());
    for (const auto& r : rows) ids.push_back({ r.x, r.y, r.z, r.frequency });
    return ids;
}

bool PayloadFinite(const coop::net::SkySignalCatchPayload& p) {
    const float vals[] = { p.row.x, p.row.y, p.row.z, p.row.strength, p.row.frequency,
                           p.row.frequencySpread, p.row.polarity, p.row.polaritySpread,
                           p.slewX, p.slewY, p.slewZ };
    for (float v : vals)
        if (!std::isfinite(v)) return false;
    return true;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;
    const auto now = Clock::now();
    if (now < g_nextPoll) return;
    g_nextPoll = now + kPoll;

    const bool cdUp = CD::EnsureResolved() && CD::Instance();
    if (!cdUp) return;
    const bool srUp = SR::EnsureResolved() && SR::Instance();
    const bool dishUp = D::EnsureResolved();
    if (!g_announced && srUp && dishUp) {
        g_announced = true;
        UE_LOGI("signal_catch: installed (desk + sky + dish surfaces resolved)");
    }
    if (CheckDeskInstance()) return;  // fresh desk: baselines primed, detect next poll

    CD::CoordSignal sig;
    const bool haveSig = CD::ReadCoordSignal(sig);

    // The dish/row reads only matter for the catch detector (claim holder);
    // the cleared detector needs just the struct. Keep idle peers at one
    // struct read + one moving-count walk.
    std::vector<Identity> curRows;
    bool haveRows = false;
    if (srUp && coop::device_occupancy::LocalHolds(kDeskClaim)) {
        std::vector<SR::SignalRow> rows;
        if (SR::ReadSignals(rows)) {
            curRows = RowsToIdentities(rows);
            haveRows = true;
        }
    } else {
        // NOT holding: the row baseline is no longer maintained -- drop it.
        // A stale baseline surviving a release/re-claim could pair an ancient
        // row set with a fresh dish edge (another peer's catch in flight at
        // re-claim time) and false-fire a catch for an already-consumed
        // identity past the recent-TTL, re-zeroing everyone's download
        // progress (self-review 2026-06-12). The detector re-arms on the
        // second poll after re-claiming.
        g_prevRows.clear();
        g_havePrevRows = false;
    }
    const int32_t moving = dishUp ? D::MovingCount() : -1;

    RunDetectors(s, sig, haveSig, curRows, haveRows, moving);

    // Joiner download catch-up: apply once OUR machine armed (mesh valid).
    if (g_pending.armed && CD::DownloadMeshValid()) {
        CD::ArmDownloadFromSignal(g_pending.decoded, g_pending.polarity);
        CD::WriteResDetect(g_pending.resDetect);
        UE_LOGI("signal_catch: pending download adopt applied (decoded=%.1f polarity=%d "
                "resDetect=%.2f)", g_pending.decoded, g_pending.polarity, g_pending.resDetect);
        g_pending = {};
    }

    PruneRecent();
}

void OnReliable(const coop::net::SkySignalCatchPayload& p, uint8_t senderSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    if (p.kind > 1 || !PayloadFinite(p)) return;
    if (!CD::EnsureResolved() || !CD::Instance()) return;

    const Identity id{ p.row.x, p.row.y, p.row.z, p.row.frequency };
    if (s->role() == coop::net::Role::Host) {
        if (p.kind == 0) {
            // Only the desk-claim holder legitimately catches (v63 occupancy).
            const uint8_t holder = coop::device_occupancy::HolderOf(kDeskClaim);
            if (holder != senderSlot) {
                UE_LOGW("signal_catch: catch from slot %u but desk held by %u -- dropping",
                        static_cast<unsigned>(senderSlot), static_cast<unsigned>(holder));
                return;
            }
            RegisterRecent(id);
        }
        // kind=1 is deliberately NOT holder-gated (audit 2026-06-12 finding 2
        // REJECTED): the delete button is a PHYSICAL desk button -- the v63
        // claim gates ENTERING the screen, not pressing panel buttons, so any
        // peer can legitimately clear without holding the claim (the same
        // trust level as the unclaimed DeskState button edges). The replay is
        // idempotent and primes the receivers' detectors, so repeats are
        // bounded no-ops.
        ApplyReplay(p);
        SendOut(s, p, /*exceptSlot*/ senderSlot);  // fan out to the other clients
        // The host's own 1 Hz sky poll sees the row removal and broadcasts a
        // fresh snapshot; the recent-catch filter covers any stale one in flight.
    } else {
        // Client: transport-trusted (we only receive from the host).
        if (p.kind == 0) RegisterRecent(id);
        ApplyReplay(p);
    }
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    if (!CD::EnsureResolved() || !CD::Instance()) return;
    CD::CoordSignal sig;
    if (!CD::ReadCoordSignal(sig) || IsNoneName(sig.objectName)) return;
    coop::net::SkySignalCatchPayload p{};
    BuildCatchPayload(sig, 0, p);
    s->SendReliableToSlot(peerSlot, coop::net::ReliableKind::SkySignalCatch, &p, sizeof(p));
    UE_LOGI("signal_catch: connect catch replay -> slot %d ('%ls', slewValid=%u)",
            peerSlot, sig.objectName.c_str(), static_cast<unsigned>(p.slewValid));
}

void NoteIncomingSnapshot(std::vector<SR::SignalRow>& rows) {
    auto* s = g_session.load(std::memory_order_acquire);
    // Run the catch detector NOW (fresh reads) so an in-flight local catch
    // outranks the stale snapshot row about to apply.
    if (s && CD::EnsureResolved() && CD::Instance() && !CheckDeskInstance() &&
        coop::device_occupancy::LocalHolds(kDeskClaim)) {
        CD::CoordSignal sig;
        const bool haveSig = CD::ReadCoordSignal(sig);
        std::vector<SR::SignalRow> liveRows;
        bool haveRows = false;
        std::vector<Identity> ids;
        if (SR::EnsureResolved() && SR::Instance() && SR::ReadSignals(liveRows)) {
            ids = RowsToIdentities(liveRows);
            haveRows = true;
        }
        const int32_t moving = D::EnsureResolved() ? D::MovingCount() : -1;
        RunDetectors(s, sig, haveSig, ids, haveRows, moving);
    }
    // Strip recently-caught identities -- a snapshot sent before the host
    // processed the catch must not resurrect the row.
    for (auto it = rows.begin(); it != rows.end();) {
        if (IsRecent({ it->x, it->y, it->z, it->frequency })) {
            UE_LOGI("signal_catch: filtered recently-caught row (%.0f, %.0f, %.0f) "
                    "from an incoming snapshot", it->x, it->y, it->z);
            it = rows.erase(it);
        } else {
            ++it;
        }
    }
}

void SetPendingDownloadAdopt(float decoded, int32_t polarity, float resDetect) {
    if (!std::isfinite(decoded) || !std::isfinite(resDetect)) return;
    g_pending.armed = true;
    g_pending.decoded = decoded;
    g_pending.polarity = polarity;
    g_pending.resDetect = resDetect;
    g_pending.at = Clock::now();
}

void OnDisconnect() {
    g_prevRows.clear();
    g_havePrevRows = false;
    g_prevMoving = -1;
    g_prevSigName.clear();
    g_havePrevSig = false;
    g_deskInst = nullptr;
    g_recent.clear();
    g_pending = {};
}

}  // namespace coop::signal_catch_sync
