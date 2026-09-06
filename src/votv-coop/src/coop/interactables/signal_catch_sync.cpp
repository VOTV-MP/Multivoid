// coop/interactables/signal_catch_sync.cpp -- see coop/interactables/signal_catch_sync.h.

#include "coop/interactables/signal_catch_sync.h"

#include "coop/interactables/console_state_sync.h"
#include "coop/interactables/desk_input_sync.h"
#include "coop/interactables/dish_sync.h"
#include "coop/comms/peer_action_feed.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"

#include "ue_wrap/desk/console_desk.h"
#include "ue_wrap/desk/dish.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/desk/space_renderer.h"

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
// The recent-catch TTL: a same-identity re-catch needs a full ping cycle, measured at six
// seconds or more, plus the native satellites-moving re-enter block, so five seconds can
// never swallow a legitimate re-catch.
constexpr auto kRecentTTL = std::chrono::seconds(5);

struct Identity { float x = 0, y = 0, z = 0, frequency = 0; };

bool IdentityEq(const Identity& a, const Identity& b) {
    // Exact float equality: every non-catcher copy is wire-byte identical (never fuzzy-match
    // identities).
    return a.x == b.x && a.y == b.y && a.z == b.z && a.frequency == b.frequency;
}

Clock::time_point g_nextPoll{};
bool g_announced = false;

// The catch detector's baseline: the full identity of the desk's signal data at the last poll
// (the tuple and the name, the change-edge signature).
Identity g_prevId{};
std::wstring g_prevSigName;
bool g_havePrevSig = false;
// The desk instance the baselines were read from. A mid-session level reload spawns a fresh
// desk whose runtime-only signal data starts at None; a module-static baseline surviving
// that would read armed-versus-None as a player pressing the delete button and broadcast a
// spurious clear (resetting every peer's download). The same instance-tracking shape as the
// console state's suppression latch.
void* g_deskInst = nullptr;

// Recently caught identities (the catcher at send, the host and receivers at apply): filters
// stale in-flight sky-signal snapshot rows and detector re-fires.
struct Recent { Identity id; Clock::time_point at; };
std::vector<Recent> g_recent;

// Re-baseline the detectors against the current desk instance without firing anything (the
// fresh-instance prime and the explicit re-prime after a replay).
void PrimeBaselinesFromDesk() {
    CD::CoordSignal sig;
    if (CD::ReadCoordSignal(sig)) {
        g_prevId = { sig.x, sig.y, sig.z, sig.frequency };
        g_prevSigName = sig.objectName;
        g_havePrevSig = true;
    } else {
        g_prevId = {};
        g_prevSigName.clear();
        g_havePrevSig = false;
    }
}

// True once per desk-instance change (boot and a mid-session level reload): re-primes the
// baselines so stale state from the previous world can never read as a player action on the
// fresh one.
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
    // Alpha, lifetimes and direction stay zero: the row is being deleted; receivers only consume
    // the struct content.
    size_t n = 0;
    for (; n < sig.objectName.size() && n < sizeof(w.objectName); ++n)
        w.objectName[n] = static_cast<char>(sig.objectName[n]);  // rolled names are ASCII
    w.nameLen = static_cast<uint8_t>(n);
}

// The wire row's ASCII name widened for feed and log lines.
std::wstring WireName(const coop::net::WireSkySignal& w) {
    const size_t n = w.nameLen <= sizeof(w.objectName) ? w.nameLen : sizeof(w.objectName);
    return std::wstring(w.objectName, w.objectName + n);
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

// Send a built payload toward the rest of the session. A client sends to the host (slot 0)
// for validation; the host sends to every ready client except `exceptSlot` (the origin),
// stamping the logical sender so receivers see who acted.
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

// The receiver-side replay of the game's consume chain's identity half. The dish theatre is
// host-only (the host replays the start-moving fan-out and streams poses via the dish sync;
// a client never slews from the wire) and the arm rides the host-authored dish-arm lane.
void ApplyReplay(coop::net::Session* s, const coop::net::SkySignalCatchPayload& p) {
    if (p.kind == 1) {
        // The game's signal-data-deleted chain: clear the signal, reset the download machine.
        CD::ClearCoordSignal();
        CD::ResetDownloadMachine();
        g_prevId = {};
        g_prevSigName = L"None";  // prime: the wire apply must not re-fire the edge
        g_havePrevSig = true;
        coop::desk_input_sync::PrimeBaselines();
        UE_LOGI("signal_catch: cleared replay applied (download machine reset)");
        return;
    }
    const CD::CoordSignal sig = CoordSignalFromRow(p.row);
    CD::WriteCoordSignal(sig);
    SR::RemoveSignalByIdentity(sig.x, sig.y, sig.z, sig.frequency);
    CD::ResetDownloadMachine();
    // No ping-success beep replay here: the pinging peer's organic ping sound crosses on the desk
    // sound-effects lane.
    if (s->role() == coop::net::Role::Host) {
        if (p.slewValid) {
            const int32_t n = D::StartMovingAll(
                ue_wrap::FVector{ p.slewX, p.slewY, p.slewZ });  // the native fan-out
            UE_LOGI("signal_catch: catch replay applied ('%ls', %d dish(es) slewing)",
                    sig.objectName.c_str(), n);
        } else {
            // Unreachable from a live catch (the coordinate write and the slew fan-out are one
            // synchronous chain); log the decline.
            UE_LOGW("signal_catch: host catch replay with slewValid=0 ('%ls') -- "
                    "no theater started", sig.objectName.c_str());
        }
    } else {
        // A client applies the identity only. Poses arrive on the dish pose stream; the arm arrives
        // on the dish-arm lane with the host polarity.
        UE_LOGI("signal_catch: catch identity applied ('%ls') -- host owns the theater",
                sig.objectName.c_str());
    }
    g_prevId = { sig.x, sig.y, sig.z, sig.frequency };  // prime the FULL tuple
    g_prevSigName = sig.objectName;
    g_havePrevSig = true;
    coop::desk_input_sync::PrimeBaselines();
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

// The catch and cleared detector body, shared by the 1 Hz tick and the snapshot-arrival race
// check. The signature is the signal data's identity (tuple or name) change edge: no sky-row
// corroboration, no dish edge (the field has exactly two writers, the native ping-success
// chain and our wire appliers, and the wire appliers prime the baselines).
void RunDetectors(coop::net::Session* s, const CD::CoordSignal& sig, bool haveSig) {
    if (s && s->connected() && haveSig && g_havePrevSig) {
        // Cleared (any peer; unclaimed trust, like the physical button).
        if (!IsNoneName(g_prevSigName) && IsNoneName(sig.objectName)) {
            coop::net::SkySignalCatchPayload p{};
            BuildCatchPayload(sig, 1, p);
            SendOut(s, p, /*exceptSlot*/ -1);  // client -> host; host -> all clients
            UE_LOGI("signal_catch: local 'Signal data deleted' relayed");
        }
        // Catch: the identity change edge to non-None. No claim gate: the successful ping's own
        // completion releases the desk FSM-hold within the same second as the edge, so a
        // claim-gated 1 Hz detector lost the race and the roll-forward below ate the catch
        // permanently. The unprimed change edge itself proves local authorship, since the only
        // writers of the field are the native ping-success chain and our wire appliers, and every
        // wire applier primes these baselines.
        if (!IsNoneName(sig.objectName)) {
            const Identity id{ sig.x, sig.y, sig.z, sig.frequency };
            const bool changed = !IdentityEq(id, g_prevId) ||
                                 sig.objectName != g_prevSigName;
            if (changed && !IsRecent(id)) {
                coop::net::SkySignalCatchPayload p{};
                BuildCatchPayload(sig, 0, p);
                RegisterRecent(id);
                SendOut(s, p, /*exceptSlot*/ -1);  // host: all clients; client: the host
                UE_LOGI("signal_catch: local catch detected ('%ls' at %.0f,%.0f,%.0f; "
                        "slewValid=%u) -- relayed",
                        sig.objectName.c_str(), sig.x, sig.y, sig.z,
                        static_cast<unsigned>(p.slewValid));
                // The catcher's own activity-feed line (the same line everyone sees, no "You");
                // receivers announce at their receive sites. The raw local peer id passes through:
                // the announce resolves the local slot to the local nickname itself (forcing an
                // unassigned slot to 0 would misattribute a pre-assignment catch to the host).
                coop::peer_action_feed::Announce(
                    coop::players::Registry::Get().LocalPeerId(),
                    L"caught signal '" + sig.objectName + L"'");
                // The client's own ping slews are unpreventable (the dispatch is invisible to the
                // hook), so kill them now, after the payload (built from a still-moving dish) is on
                // the wire. The host keeps its native theatre: it is the pose authority.
                coop::dish_sync::KillOwnPingSlews();
            }
        }
    }
    // Roll the previous-poll state forward.
    if (haveSig) {
        g_prevId = { sig.x, sig.y, sig.z, sig.frequency };
        g_prevSigName = sig.objectName;
        g_havePrevSig = true;
    }
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
    if (!g_announced) {
        g_announced = true;
        UE_LOGI("signal_catch: installed (desk surface resolved; L4 tuple detector)");
    }
    if (CheckDeskInstance()) return;  // fresh desk: baselines primed, detect next poll

    CD::CoordSignal sig;
    const bool haveSig = CD::ReadCoordSignal(sig);
    RunDetectors(s, sig, haveSig);
    PruneRecent();
}

void OnReliable(const coop::net::SkySignalCatchPayload& p, uint8_t senderSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    if (p.kind > 2 || !PayloadFinite(p)) return;
    if (!CD::EnsureResolved() || !CD::Instance()) return;

    const Identity id{ p.row.x, p.row.y, p.row.z, p.row.frequency };
    if (s->role() == coop::net::Role::Host) {
        // kind 2 (the connect state seed) is host-authored only; receiving one here is a protocol
        // violation, never a legitimate edge.
        if (p.kind == 2) {
            UE_LOGW("signal_catch: kind=2 seed arrived AT the host from slot %u -- "
                    "protocol violation, dropped", static_cast<unsigned>(senderSlot));
            return;
        }
        if (p.kind == 0) {
            // No holder validation: the FSM-hold releases in the same second as the catch edge, so
            // a holder-equals-sender check raced exactly like the detector's claim gate did. The
            // client's unprimed edge is the authority; transport is trusted. The recent-dedup below
            // bounds duplicates and protects an in-progress download from a duplicate's machine
            // reset.
            if (IsRecent(id)) {
                UE_LOGI("signal_catch: duplicate catch ('%.*s') within recent-TTL -- dropped",
                        static_cast<int>(p.row.nameLen), p.row.objectName);
                return;
            }
            RegisterRecent(id);
        }
        // kind 1 is deliberately not gated: the delete button is a physical desk button, so any
        // peer can legitimately clear. The replay is idempotent and primes the receivers'
        // detectors, so repeats are bounded no-ops.
        ApplyReplay(s, p);
        SendOut(s, p, /*exceptSlot*/ senderSlot);  // fan out to the other clients
        // The host's own 1 Hz sky poll sees the row removal and broadcasts a fresh snapshot; the
        // recent-catch filter covers any stale one in flight.
        if (p.kind == 0)
            coop::peer_action_feed::Announce(senderSlot,
                L"caught signal '" + WireName(p.row) + L"'");
    } else {
        // A client is transport-trusted (we only receive from the host; the sender slot carries the
        // logical catcher via the relay stamp).
        if (p.kind == 0 || p.kind == 2) RegisterRecent(id);
        ApplyReplay(s, p);
        // kind 2 (the connect seed) is state adoption, not a live action: no feed line.
        if (p.kind == 0)
            coop::peer_action_feed::Announce(senderSlot,
                L"caught signal '" + WireName(p.row) + L"'");
    }
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    if (!CD::EnsureResolved() || !CD::Instance()) return;
    CD::CoordSignal sig;
    if (!CD::ReadCoordSignal(sig) || IsNoneName(sig.objectName)) return;
    coop::net::SkySignalCatchPayload p{};
    // kind 2 is the connect state seed: applied exactly like a catch but never announced to the
    // activity feed (a joiner must not see a stale host-attributed "caught signal" line).
    BuildCatchPayload(sig, 2, p);
    s->SendReliableToSlot(peerSlot, coop::net::ReliableKind::SkySignalCatch, &p, sizeof(p));
    UE_LOGI("signal_catch: connect catch seed (kind=2) -> slot %d ('%ls', slewValid=%u)",
            peerSlot, sig.objectName.c_str(), static_cast<unsigned>(p.slewValid));
}

void NoteIncomingSnapshot(std::vector<SR::SignalRow>& rows) {
    auto* s = g_session.load(std::memory_order_acquire);
    // Run the catch detector now (fresh reads) so an in-flight local catch outranks the stale
    // snapshot row about to apply; any peer with an unprimed edge is the catch authority.
    if (s && CD::EnsureResolved() && CD::Instance() && !CheckDeskInstance()) {
        CD::CoordSignal sig;
        const bool haveSig = CD::ReadCoordSignal(sig);
        RunDetectors(s, sig, haveSig);
    }
    // Strip recently caught identities: a snapshot sent before the host processed the catch must
    // not resurrect the row.
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

void OnDisconnect() {
    g_prevId = {};
    g_prevSigName.clear();
    g_havePrevSig = false;
    g_deskInst = nullptr;
    g_recent.clear();
}

}  // namespace coop::signal_catch_sync
