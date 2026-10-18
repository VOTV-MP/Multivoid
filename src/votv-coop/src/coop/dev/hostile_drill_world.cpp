// coop/dev/hostile_drill_world.cpp -- see coop/dev/hostile_drill_world.h.

#include "coop/dev/hostile_drill_world.h"

#include "coop/dispatch/event_feed.h"
#include "coop/interactables/laptop_sync.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/sleep_sync.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"
#include "coop/world/alarm_sync.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/devices/laptop.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>

namespace coop::dev::hostile_drill::world {
namespace {

using Clock = std::chrono::steady_clock;
using Kind = coop::net::ReliableKind;

// The flood: per kind, kAlternates messages alternating the client's own value A and the other B, then B. Each kind
// is a level with two values and the client's A is the host's (the alarm and the laptop are synced at the join, the
// client's bed is its own), so B is also the opposite of what the host holds at arm: an even count of applies at the
// first read leaves A applied and B owed; an odd count (a refill during the drain) leaves B applied.
constexpr int      kAlternates = 120;
constexpr uint32_t kPerKind    = kAlternates + 1;

// Besides the bedless flood, the bed gets one forged ClientRest per alternation, in rotation over three kinds (a
// character outside an identifier, a nonzero byte past the length, an Awake report carrying a bed): the host must
// refuse every one of each kind, so the bed's drained count is the flood's plus these.
constexpr int      kForgedKinds = 3;
constexpr uint32_t kForgedEach  = kAlternates / kForgedKinds;
static_assert(kAlternates % kForgedKinds == 0, "the forged kinds rotate evenly over the alternations");
constexpr coop::sleep_sync::BedRefusal kForgedReasons[kForgedKinds] = {coop::sleep_sync::BedRefusal::Charset,
                                                                       coop::sleep_sync::BedRefusal::Tail,
                                                                       coop::sleep_sync::BedRefusal::AwakeWithBed};
constexpr const char* kForgedNames[kForgedKinds] = {"charset", "tail", "awake-with-bed"};

uint32_t Expected(int kind) { return kPerKind + (kind == 1 ? static_cast<uint32_t>(kAlternates) : 0u); }

// The judge's numbers, literals written from the lanes' budgets (no symbol of the change itself): at the first read
// after the drain, a kind has applied at least two of its values and at most its burst, the refills of the span the
// judge measured from the first drained message to the last, and one for a refill inside the frame it did not see.
// Each kind whose B is still owed at that read must then hold B with its apply count grown, so by a payment, within
// its refill period plus 10 s. The laptop must then settle on B with no edge broadcast as the host's own.
struct Budget { float burst; float perSecond; };
constexpr Budget   kBudgets[3]   = {{2.f, 1.f}, {4.f, 1.f}, {2.f, 1.f}};  // alarm, bed, laptop
constexpr uint32_t kMinApplies   = 2;
constexpr auto     kLatestWindow = std::chrono::milliseconds(11000);      // one per 1 s, + 10 s
constexpr auto     kFailBound    = std::chrono::seconds(180);             // a failure bound, never a wait

constexpr Kind        kKinds[3]     = {Kind::AlarmState, Kind::SleepState, Kind::LaptopState};
constexpr const char* kKindNames[3] = {"alarm", "bed", "laptop"};

// ---- client (sender) ----
bool g_sent = false;

bool OwnJoinOver() {
    return coop::net_pump::HasAnnouncedWorldReady() &&
           coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle;
}

bool SendLevel(coop::net::Session* s, int i, bool value) {
    switch (i) {
    case 0: {
        coop::net::AlarmStatePayload p{};
        p.active = value ? 1 : 0;
        return s->SendReliableToSlot(0, Kind::AlarmState, &p, sizeof(p));
    }
    case 1: {
        coop::net::SleepStatePayload p{};
        p.op = static_cast<uint8_t>(coop::net::SleepOp::ClientRest);
        p.state = static_cast<uint8_t>(value ? coop::net::SleepPeerState::Resting
                                             : coop::net::SleepPeerState::Awake);
        return s->SendReliableToSlot(0, Kind::SleepState, &p, sizeof(p));
    }
    default: {
        coop::net::LaptopStatePayload p{};
        p.op = 0;  // the power edge
        p.isOpened = value ? 1 : 0;
        return s->SendReliableToSlot(0, Kind::LaptopState, &p, sizeof(p));
    }
    }
}

// One forged ClientRest of `forged` (0..2, kForgedReasons' order): the name a legal sender never writes.
bool SendForgedBed(coop::net::Session* s, int forged) {
    coop::net::SleepStatePayload p{};
    p.op = static_cast<uint8_t>(coop::net::SleepOp::ClientRest);
    p.state = static_cast<uint8_t>(forged == 2 ? coop::net::SleepPeerState::Awake
                                               : coop::net::SleepPeerState::Resting);
    p.bed.len = 3;
    p.bed.data[0] = 'a';
    p.bed.data[1] = forged == 0 ? '\n' : 'b';
    p.bed.data[2] = 'c';
    if (forged == 1) p.bed.data[10] = 1;
    return s->SendReliableToSlot(0, Kind::SleepState, &p, sizeof(p));
}

// ---- host (judge) ----
enum class Step { WaitArm, WaitDrained, WaitLatest, WaitSettle, Done };
Step              g_step = Step::WaitArm;
int               g_slot = -1;
Clock::time_point g_armAt{};
Clock::time_point g_firstDrainAt{};  // the first tick a message of the flood was seen drained; epoch until then
Clock::time_point g_readAt{};
uint32_t          g_drainedBase[3] = {};
uint32_t          g_appliedBase[3] = {};
uint32_t          g_appliedAtRead[3] = {};
bool              g_judged[3] = {};
bool              g_last[3] = {};  // B, per kind: the opposite of what the host held at arm
uint32_t          g_organicBase = 0;
uint32_t          g_refusedBase[kForgedKinds] = {};
std::string       g_forgedCounts;  // "charset 40, tail 40, awake-with-bed 40", for the verdict line

uint32_t Refused(int forged) { return coop::sleep_sync::BedRefusalsFrom(g_slot, kForgedReasons[forged]); }

uint32_t Applied(int i) {
    switch (i) {
    case 0:  return coop::alarm_sync::ClientApplies(g_slot);
    case 1:  return coop::sleep_sync::ReportsApplied(g_slot);
    default: return coop::laptop_sync::ClientEdgesApplied(g_slot);
    }
}

bool Read(int i, bool& out) {
    switch (i) {
    case 0:  return coop::alarm_sync::ReadLocalActive(out);
    case 1:  out = coop::sleep_sync::PeerState(g_slot) != coop::net::SleepPeerState::Awake; return true;
    default: return coop::laptop_sync::Target(out);
    }
}

void Verdict(Arm a, const char* what, const std::string& detail) {
    UE_LOGI("[HOSTILE-DRILL] %s %s -- %s", ArmName(a), what, detail.c_str());
    g_step = Step::Done;
}

void Fail(Arm a, const std::string& detail) {
    UE_LOGW("[HOSTILE-DRILL] %s FAIL: %s", ArmName(a), detail.c_str());
    g_step = Step::Done;
}

void ArmHost(coop::net::Session* s, Arm a) {
    for (int slot = 1; slot < static_cast<int>(coop::net::kMaxPeers) && g_slot < 0; ++slot)
        if (s->IsSlotWorldReady(slot)) g_slot = slot;
    if (g_slot < 0) return;
    bool held[3] = {};
    for (int i = 0; i < 3; ++i) {
        if (!Read(i, held[i])) {
            Verdict(a, "INCONCLUSIVE", std::string("the host's ") + kKindNames[i] + " is unreadable here");
            return;
        }
        g_last[i] = !held[i];
        g_drainedBase[i] = coop::event_feed::DrainedFrom(g_slot, kKinds[i]);
        g_appliedBase[i] = Applied(i);
    }
    ue_wrap::laptop::PowerState ps;
    if (!ue_wrap::laptop::ReadPower(ps) || !ps.powered) {
        Verdict(a, "INCONCLUSIVE", "the host's laptop has no power, so its toggle cannot run");
        return;
    }
    g_organicBase = coop::laptop_sync::OrganicEdgesSent();
    for (int f = 0; f < kForgedKinds; ++f) g_refusedBase[f] = Refused(f);
    g_armAt = Clock::now();
    g_step = Step::WaitDrained;
    UE_LOGI("[HOSTILE-DRILL] %s armed on slot %d (held alarm %d, bed %d, laptop %d; drained %u/%u/%u, applied "
            "%u/%u/%u)", ArmName(a), g_slot, held[0] ? 1 : 0, held[1] ? 1 : 0, held[2] ? 1 : 0, g_drainedBase[0],
            g_drainedBase[1], g_drainedBase[2], g_appliedBase[0], g_appliedBase[1], g_appliedBase[2]);
}

void WaitDrained(Arm a) {
    const Clock::time_point now = Clock::now();
    bool all = true;
    uint32_t drained[3];
    for (int i = 0; i < 3; ++i) {
        drained[i] = coop::event_feed::DrainedFrom(g_slot, kKinds[i]) - g_drainedBase[i];
        if (drained[i] > 0 && g_firstDrainAt == Clock::time_point{}) g_firstDrainAt = now;
        all = all && drained[i] >= Expected(i);
    }
    if (!all) {
        if (now - g_armAt > kFailBound)
            Fail(a, "timeout (drained alarm " + std::to_string(drained[0]) + ", bed " + std::to_string(drained[1]) +
                        ", laptop " + std::to_string(drained[2]) + " of " + std::to_string(kPerKind) + ")");
        return;
    }
    const float spanS = std::chrono::duration<float>(now - g_firstDrainAt).count();
    uint32_t applied[3], most[3];
    bool over = false, under = false;
    for (int i = 0; i < 3; ++i) {
        applied[i] = Applied(i) - g_appliedBase[i];
        most[i] = static_cast<uint32_t>(kBudgets[i].burst + std::ceil(kBudgets[i].perSecond * spanS)) + 1;
        over = over || applied[i] > most[i];
        under = under || applied[i] < kMinApplies;
    }
    UE_LOGI("[HOSTILE-DRILL] %s drained %u of each kind over %.0f ms; applied alarm %u, bed %u, laptop %u (at most "
            "%u, %u, %u)", ArmName(a), kPerKind, spanS * 1000.f, applied[0], applied[1], applied[2], most[0], most[1],
            most[2]);
    const std::string counts = "alarm " + std::to_string(applied[0]) + ", bed " + std::to_string(applied[1]) +
                               ", laptop " + std::to_string(applied[2]);
    uint32_t refused[kForgedKinds];
    bool forgedMissed = false;
    for (int f = 0; f < kForgedKinds; ++f) {
        refused[f] = Refused(f) - g_refusedBase[f];
        forgedMissed = forgedMissed || refused[f] != kForgedEach;
    }
    g_forgedCounts.clear();
    for (int f = 0; f < kForgedKinds; ++f)
        g_forgedCounts += std::string(f ? ", " : "") + kForgedNames[f] + " " + std::to_string(refused[f]);
    UE_LOGI("[HOSTILE-DRILL] %s forged beds refused: %s (of %u each)", ArmName(a), g_forgedCounts.c_str(),
            kForgedEach);
    if (forgedMissed) {
        Fail(a, "forged-bed-not-refused (" + g_forgedCounts + "; each kind sent " + std::to_string(kForgedEach) +
                    " and every one must be refused, so none is applied)");
        return;
    }
    if (under) {
        Fail(a, "none-applied (" + counts + "; at least " + std::to_string(kMinApplies) + " each)");
        return;
    }
    if (over) {
        Fail(a, "applied-every-change (" + counts + "; at most " + std::to_string(most[0]) + ", " +
                    std::to_string(most[1]) + ", " + std::to_string(most[2]) + ")");
        return;
    }
    // A kind whose B a refill applied on arrival holds it already: no payment is owed, so that leg proves nothing.
    std::string skipped;
    int judged = 0;
    for (int i = 0; i < 3; ++i) {
        bool v = false;
        g_judged[i] = !(Read(i, v) && v == g_last[i]);
        g_appliedAtRead[i] = Applied(i);
        if (g_judged[i]) { ++judged; continue; }
        skipped += skipped.empty() ? "" : ", ";
        skipped += kKindNames[i];
    }
    if (!skipped.empty())
        UE_LOGI("[HOSTILE-DRILL] %s already holds its last value at the read (%s): no payment owed, not judged",
                ArmName(a), skipped.c_str());
    if (judged == 0) {
        Verdict(a, "INCONCLUSIVE", "every kind's last value applied on arrival; no payment was exercised");
        return;
    }
    g_readAt = Clock::now();
    g_step = Step::WaitLatest;
}

void WaitLatest(Arm a) {
    bool all = true;
    for (int i = 0; i < 3; ++i) {
        if (!g_judged[i]) continue;
        // Owed at the read, so only a payment can apply it now: the flood sent nothing after it, and the apply count
        // growing tells a payment from another writer of the same world value.
        bool v = false;
        if (Read(i, v) && v == g_last[i] && Applied(i) > g_appliedAtRead[i]) continue;
        all = false;
        if (Clock::now() - g_readAt > kLatestWindow) {
            Fail(a, std::string("latest-lost (the ") + kKindNames[i] +
                        "'s last value was not applied by a payment within " + std::to_string(kLatestWindow.count()) +
                        " ms)");
            return;
        }
    }
    if (!all) return;
    UE_LOGI("[HOSTILE-DRILL] %s each last value owed at the read was paid %lld ms after it; the laptop settles next",
            ArmName(a),
            static_cast<long long>(
                std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - g_readAt).count()));
    g_step = Step::WaitSettle;
}

// The laptop has settled when no toggle runs, no target is pending and its lane's poll has seen the state, so an
// edge the poll would broadcast is already counted.
void WaitSettle(Arm a) {
    bool opened = false;
    if (!coop::laptop_sync::Settled(opened)) {
        if (Clock::now() - g_armAt > kFailBound) Fail(a, "timeout (the host's laptop did not settle)");
        return;
    }
    const uint32_t organic = coop::laptop_sync::OrganicEdgesSent() - g_organicBase;
    if (organic > 0) {
        Fail(a, "echoed-replay (the host broadcast " + std::to_string(organic) +
                    " laptop power edge(s) as its own while it only applied the client's)");
        return;
    }
    if (opened != g_last[2]) {
        Fail(a, std::string("settled-off-target (the host's laptop rests at isOpened=") + (opened ? "1" : "0") +
                    ", the client's last value was " + (g_last[2] ? "1" : "0") + ")");
        return;
    }
    Verdict(a, "PASS",
            "each kind applied no more than its budget pays over the drain, each last value owed at the read was paid, "
            "the host's laptop settled on the last value with no edge sent as its own, and every forged bed was "
            "refused (" + g_forgedCounts + " of " + std::to_string(kForgedEach) + " each)");
}

}  // namespace

const char* ArmName(Arm a) { return a == Arm::WorldLatest ? "world_latest" : "off"; }

void TickClient(coop::net::Session* s, Arm a) {
    if (g_sent || !OwnJoinOver()) return;
    const uint8_t self = coop::players::Registry::Get().LocalPeerId();
    if (self == 0 || self >= coop::players::kMaxPeers) return;  // no slot yet; wait
    g_sent = true;
    bool own[3] = {};
    if (!coop::alarm_sync::ReadLocalActive(own[0])) {
        UE_LOGW("[HOSTILE-DRILL] %s INCONCLUSIVE: this peer's alarm trigger is not bound", ArmName(a));
        return;
    }
    own[1] = coop::sleep_sync::LocalInBed();
    // The laptop's target, not its state: a laptop still replaying the join's state reads the value it goes to.
    if (!coop::laptop_sync::Target(own[2])) {
        UE_LOGW("[HOSTILE-DRILL] %s INCONCLUSIVE: this peer's laptop is not resolved", ArmName(a));
        return;
    }
    bool ok = true;
    for (int n = 0; n < kAlternates && ok; ++n) {
        for (int i = 0; i < 3 && ok; ++i) ok = SendLevel(s, i, (n % 2) == 0 ? own[i] : !own[i]);
        if (ok) ok = SendForgedBed(s, n % kForgedKinds);
    }
    for (int i = 0; i < 3 && ok; ++i) ok = SendLevel(s, i, !own[i]);
    if (!ok) {
        UE_LOGW("[HOSTILE-DRILL] %s INCONCLUSIVE: a message of the flood was not sent", ArmName(a));
        return;
    }
    UE_LOGI("[HOSTILE-DRILL] %s sent %u of each kind and %u forged beds of each of three kinds for slot %u (own "
            "alarm %d, bed %d, laptop %d; each ending on the other)", ArmName(a), kPerKind, kForgedEach,
            static_cast<unsigned>(self), own[0] ? 1 : 0, own[1] ? 1 : 0,
            own[2] ? 1 : 0);
}

void TickHostJudge(coop::net::Session* s, Arm a) {
    if (g_step == Step::WaitArm) { ArmHost(s, a); return; }
    if (g_step == Step::Done) return;
    if (!s->HasPeerConn(g_slot)) {
        Verdict(a, "INCONCLUSIVE", "the judged slot left before the verdict");
        return;
    }
    if (g_step == Step::WaitDrained)     WaitDrained(a);
    else if (g_step == Step::WaitLatest) WaitLatest(a);
    else if (g_step == Step::WaitSettle) WaitSettle(a);
}

void OnDisconnect() {
    g_sent = false;
    g_step = Step::WaitArm;
    g_slot = -1;
    g_armAt = {};
    g_firstDrainAt = {};
    g_readAt = {};
    for (int i = 0; i < 3; ++i) {
        g_drainedBase[i] = g_appliedBase[i] = g_appliedAtRead[i] = 0;
        g_judged[i] = g_last[i] = false;
    }
    g_organicBase = 0;
    for (uint32_t& r : g_refusedBase) r = 0;
    g_forgedCounts.clear();
}

}  // namespace coop::dev::hostile_drill::world
