// coop/dev/container_intent_drill_put2.cpp -- the container intent drill's put arms that need a red or an interleave
// (puthover, putfull, putfullhand, putdepart, putdeadline, putowed), the CLIENT's half, and the reds of both peers; see
// container_intent_drill_put.cpp for the machine they run on and container_intent_drill_put_host.cpp for the host.
// A red is a dev row beside container_act_no_escrow, read once and armed on the peer it breaks. An arm that interleaves
// holds the replay of its grant (HoldReplay) and does its work while it is parked: the machine hands its tick to
// Put2Hook after the ask and takes it back at the settle.
// Lines are tagged [CI-DRILL] and end in the machine's own `client <arm> PASS` or `client <arm> FAIL: <why>`.

#include "container_intent_drill_put.h"
#include "container_route_drill_detail.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/drill_chat.h"
#include "coop/net/protocol.h"
#include "coop/player/roster.h"
#include "coop/props/container_act_host.h"
#include "coop/props/container_act_request.h"

#include "ue_wrap/actors/container_view.h"
#include "ue_wrap/core/log.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>

namespace coop::dev::container_intent_drill::detail {
namespace {

namespace AR = coop::props::container_act_request;
namespace AH = coop::props::container_act_host;
namespace CV = ue_wrap::container_view;
namespace DC = coop::dev::drill_chat;
namespace RD = coop::dev::container_route_drill::detail;

constexpr uint64_t kParkBoundMs    = 15000;   // the press to the grant being parked
constexpr uint64_t kActBoundMs     = 15000;   // the release (or the press) to the act's end
constexpr uint64_t kSettleMs       = 1500;    // the late done to the host having judged it
constexpr uint64_t kDeadlineHoldMs = 32000;   // putdeadline: parked this long, past the host's 30 s hold deadline
constexpr uint64_t kPhantomBoundMs = 90000;   // putowed: the host's 30 tries at 1 Hz, then the truth it sends
constexpr const char* kNoSpaceLine = "Not enough space.";
constexpr const char* kLapsedLine  = "Try again in a moment.";
constexpr uint64_t kGoBoundMs      = 240000;  // putjoin: the host's go (the second client is yet to connect)
constexpr uint64_t kReleaseBoundMs = 35000;   // putjoin: the park to the host's release; its reservation lapses at 30 s
constexpr uint64_t kAgreeBoundMs   = 20000;   // putjoin: both commits to the mirror agreeing with the host
constexpr uint64_t kOpenBoundMs    = 10000;   // putowed phase 2: the screen showing the target again
constexpr uint64_t kRestoreBoundMs = 75000;   // putowed phase 2: the knob's 35 s, then the restore landing
constexpr uint64_t kPollEveryMs    = 250;

// The hook's steps: waiting for the park, the park held (putdeadline), the released replay's end, the next put
// (putdeadline), the phantom record leaving the mirror (putowed), and nothing more to do; then putjoin's go, release and
// agreement, and putowed's second phase (the take asked, parked, aborted, and its record restored).
enum class K : uint8_t { Park, Hold, Ended, Second, Phantom, Idle, Go, Release, Agree, TakeAsk, TakePark, TakeEnd, Restore };
K        g_k = K::Park;
uint64_t g_kMs = 0;
bool     g_asked2 = false;
bool     g_rejoinRead = false, g_rejoined = false;   // once per process: a relaunch is a new process
bool     g_joinerKnown = false, g_joiner = false;     // putjoin: this client joined after another one
bool     g_goSeen = false, g_releaseSeen = false;     // putjoin: the host's lines, latched when seen
uint64_t g_nextPollMs = 0;
bool     g_reopened = false, g_sawDrop = false;       // putowed phase 2
uint32_t g_take0 = 0;                                 // and the take commits sent before it asked
bool     g_notReadyReleased = false;                  // the drill put insert_notready back to 0

void KNext(K s) {
    g_k = s;
    g_kMs = ::GetTickCount64();
}
bool KExpired(uint64_t bound) { return ::GetTickCount64() - g_kMs > bound; }

// The grant is parked: each arm does its work under the parked replay.
void StepPark() {
    if (!AR::dev::ReplayParked()) {
        if (KExpired(kParkBoundMs))
            PutFail("the grant did not park within %llu s (pending id=%u)", static_cast<unsigned long long>(kParkBoundMs / 1000),
                    static_cast<unsigned>(AR::dev::PendingId()));
        return;
    }
    switch (ArmOf()) {
    case Arm::PutHover: {
        // A slot of the target is hovered after the ask: the replay must see it and abort.
        void* const ui = RD::Screen();
        void* const slot = ui ? RD::SlotAt(ui, 0) : nullptr;
        CV::PutState st{};
        const char* failed = "";
        if (!slot || !PutHoverSlot(ui, slot) || !CV::ReadPutState(ui, st, &failed) || !st.hovered) {
            PutFail("the target's first slot could not be hovered under the parked replay (%s)", failed);
            return;
        }
        UE_LOGI("[CI-DRILL] client puthover: a slot is hovered under the parked replay; the replay is released");
        AR::dev::ReleaseReplay();
        AR::dev::HoldReplay(false);
        KNext(K::Ended);
        return;
    }
    case Arm::PutDepart:
        // The rig drops this client on the next line and relaunches it; nothing more runs in this life.
        DepartMarkParked();
        UE_LOGI("[CI-DRILL] client putdepart parked");
        KNext(K::Idle);
        return;
    case Arm::PutDeadline:
        AR::dev::DoubleDone(true);
        UE_LOGI("[CI-DRILL] client putdeadline grant parked; the replay is released in %llu s",
                static_cast<unsigned long long>(kDeadlineHoldMs / 1000));
        KNext(K::Hold);
        return;
    case Arm::PutJoin:
        UE_LOGI("[CI-DRILL] client putjoin: the grant is parked; the host releases it once the joiner has asked");
        KNext(K::Release);
        return;
    default:
        PutFail("the hook ran for an arm that has none");
        return;
    }
}

// putdeadline: the replay is released once the host's hold deadline has passed; its done is sent twice.
void StepHold() {
    if (!KExpired(kDeadlineHoldMs)) return;
    UE_LOGI("[CI-DRILL] client putdeadline releases the parked replay after %llu ms",
            static_cast<unsigned long long>(::GetTickCount64() - g_kMs));
    AR::dev::ReleaseReplay();
    AR::dev::HoldReplay(false);
    AR::dev::DoubleDone(false);
    KNext(K::Ended);
}

void StepEnded() {
    if (!PutActEnded()) {
        if (KExpired(kActBoundMs)) {
            const AR::dev::Put p = AR::dev::LastPut();
            PutFail("no put act ended within %llu s of the release (last put id=%u outcome=%s)",
                    static_cast<unsigned long long>(kActBoundMs / 1000), static_cast<unsigned>(p.id),
                    AR::dev::PutOutcomeName(p.outcome));
        }
        return;
    }
    if (ArmOf() == Arm::PutHover || ArmOf() == Arm::PutJoin) {   // the verdict is the settle's
        PutToSettle();
        KNext(K::Idle);
        return;
    }
    const AR::dev::Put p = AR::dev::LastPut();
    UE_LOGI("[CI-DRILL] client putdeadline late put: act -> %s", AR::dev::PutOutcomeName(p.outcome));
    if (p.outcome != AR::dev::PutOutcome::Done) {
        PutFail("the late put ended %s, expected done", AR::dev::PutOutcomeName(p.outcome));
        return;
    }
    KNext(K::Second);
}

// putdeadline: the next put, asked once the host has judged the double done; the host answers it Lapsed.
void StepSecond() {
    const PutBase b = PutBaseOf();
    if (!g_asked2) {
        if (!KExpired(kSettleMs)) return;
        void* const ui = RD::Screen();
        if (!ui || CV::Viewed() != b.pack) {
            PutFail("the screen does not show the target for the next put");
            return;
        }
        g_asked2 = true;
        if (!PutAskR2(ui)) return;   // the FAIL is printed
        g_kMs = ::GetTickCount64();
        UE_LOGI("[CI-DRILL] client putdeadline asks the next put");
        return;
    }
    if (!PutActEnded()) {
        if (KExpired(kActBoundMs)) PutFail("the next put was not answered within %llu s", static_cast<unsigned long long>(kActBoundMs / 1000));
        return;
    }
    const AR::dev::Put p = AR::dev::LastPut();
    const uint8_t reason = AR::dev::LastRefusal();
    const bool line = DC::ChatHasLive(kLapsedLine);
    UE_LOGI("[CI-DRILL] client putdeadline next put: act -> %s refusal=%u line=%d", AR::dev::PutOutcomeName(p.outcome),
            static_cast<unsigned>(reason), line ? 1 : 0);
    if (p.outcome == AR::dev::PutOutcome::Refused && reason == static_cast<uint8_t>(coop::net::ContainerActReason::Lapsed) &&
        line) {
        PutPass(PutStrayCount(RD::kDriveClass, b.pack));
        return;
    }
    PutFail("the next put was not answered Lapsed: act -> %s, refusal %u, line %d", AR::dev::PutOutcomeName(p.outcome),
            static_cast<unsigned>(reason), line ? 1 : 0);
}

// putowed: the host gave the put up and sent its truth, so the mirror's phantom record leaves.
void StepPhantom() {
    const PutBase b = PutBaseOf();
    const int count = CountOf(b.packEid);
    if (count == b.count0) {
        UE_LOGI("[CI-DRILL] client putowed: the phantom record left the mirror %llu ms after the put (container %d)",
                static_cast<unsigned long long>(::GetTickCount64() - g_kMs), count);
        if (PutNotReadyMode() >= 2) {   // the second phase: an aborted take's restore under the same knob
            KNext(K::TakeAsk);
            return;
        }
        PutPass(PutStrayCount(RD::kDriveClass, b.pack));
        return;
    }
    if (KExpired(kPhantomBoundMs))
        PutFail("the mirror still holds %d record(s) of the host's %d, %llu s after the put", count, b.count0,
                static_cast<unsigned long long>(kPhantomBoundMs / 1000));
}

// putjoin, the first client: the host says go once a second client is connecting, and the put is asked then.
void StepGo() {
    if (!g_goSeen) {
        if (KExpired(kGoBoundMs))
            PutFail("the host never said go within %llu s (no second client connected)", static_cast<unsigned long long>(kGoBoundMs / 1000));
        return;
    }
    void* const ui = RD::Screen();
    if (!ui || CV::Viewed() != PutBaseOf().pack) {
        PutFail("the screen does not show the target when the host said go");
        return;
    }
    UE_LOGI("[CI-DRILL] client putjoin: the host said go; the put is asked");
    KNext(K::Park);
    PutAskR2(ui);   // the FAIL is printed when it cannot
}

// putjoin, the first client: the parked replay runs once the host releases it, after the joiner has asked.
void StepRelease() {
    if (!g_releaseSeen) {
        if (KExpired(kReleaseBoundMs))
            PutFail("the host never released the parked grant within %llu s (the host's 30 s reservation deadline)",
                    static_cast<unsigned long long>(kReleaseBoundMs / 1000));
        return;
    }
    UE_LOGI("[CI-DRILL] client putjoin: the host released the parked replay after %llu ms",
            static_cast<unsigned long long>(::GetTickCount64() - g_kMs));
    AR::dev::ReleaseReplay();
    AR::dev::HoldReplay(false);
    KNext(K::Ended);
}

// putjoin, both clients: the mirror agrees with the host's two puts.
void StepAgree() {
    const PutBase b = PutBaseOf();
    const int count = CountOf(b.packEid);
    if (count == b.count0 + 2) {
        UE_LOGI("[CI-DRILL] client putjoin: the mirror agrees with the host after %llu ms (container %d -> %d)",
                static_cast<unsigned long long>(::GetTickCount64() - g_kMs), b.count0, count);
        PutPass(PutStrayCount(RD::kDriveClass, b.pack));
        return;
    }
    if (KExpired(kAgreeBoundMs))
        PutFail("the mirror holds %d record(s), expected %d (two puts on %d), %llu s after the put", count, b.count0 + 2, b.count0,
                static_cast<unsigned long long>(kAgreeBoundMs / 1000));
}

// putowed phase 2: a record of the target is taken with the replay parked, the screen closed and the replay released, so
// the take aborts and the host's restore goes owed.
void StepTakeAsk() {
    const PutBase b = PutBaseOf();
    if (CV::Viewed() != b.pack) {
        if (!g_reopened) {
            g_reopened = true;
            if (!CV::Open(b.pack)) {
                PutFail("openPropInv did not dispatch for the take");
                return;
            }
        }
        if (KExpired(kOpenBoundMs)) PutFail("the screen does not show the target for the take %llu s after openPropInv", static_cast<unsigned long long>(kOpenBoundMs / 1000));
        return;
    }
    void* const ui = RD::Screen();
    void* const slot = ui ? RD::SlotAt(ui, 0) : nullptr;
    if (!slot) {
        PutFail("the target's first slot does not resolve for the take");
        return;
    }
    g_take0 = AR::dev::LastTake().count;
    AR::dev::HoldReplay(true);
    const bool pressed = CV::PressSlot(slot);
    if (!pressed || (!AR::dev::PendingId() && AR::dev::LastTake().count == g_take0)) {
        PutFail("the take's press opened no act (pressed %d)", pressed ? 1 : 0);
        return;
    }
    UE_LOGI("[CI-DRILL] client putowed phase 2: the first slot is pressed, the replay is held");
    KNext(K::TakePark);
}

void StepTakePark() {
    if (!AR::dev::ReplayParked()) {
        if (KExpired(kParkBoundMs)) PutFail("the take's grant did not park within %llu s", static_cast<unsigned long long>(kParkBoundMs / 1000));
        return;
    }
    CV::Close();
    AR::dev::ReleaseReplay();
    AR::dev::HoldReplay(false);
    UE_LOGI("[CI-DRILL] client putowed phase 2: the screen is closed under the parked replay, the replay is released");
    KNext(K::TakeEnd);
}

void StepTakeEnd() {
    const AR::dev::Take t = AR::dev::LastTake();
    if (t.count == g_take0) {
        if (KExpired(kActBoundMs)) PutFail("the take's commit was not sent within %llu s", static_cast<unsigned long long>(kActBoundMs / 1000));
        return;
    }
    UE_LOGI("[CI-DRILL] client putowed phase 2: the take committed=%u (0 is the abort)", static_cast<unsigned>(t.committed));
    if (t.committed != 0) {
        PutFail("the take committed %u, expected the abort", static_cast<unsigned>(t.committed));
        return;
    }
    KNext(K::Restore);
}

// putowed phase 2: the record leaves the mirror with the take and returns once the host's restore lands.
void StepRestore() {
    const PutBase b = PutBaseOf();
    const int count = CountOf(b.packEid);
    if (count == b.count0 - 1) g_sawDrop = true;
    if (g_sawDrop && count == b.count0) {
        UE_LOGI("[CI-DRILL] client putowed phase 2: the record is back in the mirror %llu ms after the abort (container %d)",
                static_cast<unsigned long long>(::GetTickCount64() - g_kMs), count);
        PutPass(PutStrayCount(RD::kDriveClass, b.pack));
        return;
    }
    if (KExpired(kRestoreBoundMs))
        PutFail("the mirror holds %d of the host's %d record(s) (drop seen %d) %llu s after the abort", count, b.count0,
                g_sawDrop ? 1 : 0, static_cast<unsigned long long>(kRestoreBoundMs / 1000));
}

}  // namespace

void PutArmClientKnobs() {
    static const bool noHover = coop::config::ResolveFlag(::coop::config_registry::rows::container_act_no_hover_guard);
    static const bool noTable = coop::config::ResolveFlag(::coop::config_registry::rows::container_act_no_hand_table);
    if (noHover) AR::dev::NoHoverGuard(true);
    if (noTable) AR::dev::NoHandTable(true);
}

void PutArmHostKnobs() {
    static const bool noReservation = coop::config::ResolveFlag(::coop::config_registry::rows::container_act_no_reservation);
    static const bool noLapse = coop::config::ResolveFlag(::coop::config_registry::rows::container_act_no_lapse);
    const int notReady = PutNotReadyMode();
    if (noReservation) AH::dev::NoReservation(true);
    if (noLapse) AH::dev::NoLapse(true);
    if (notReady > 0 && !g_notReadyReleased) AH::dev::InsertNotReady(notReady);
}

bool Put2Idle() {
    if (ArmOf() != Arm::PutDepart) return false;
    if (!g_rejoinRead) {
        g_rejoinRead = true;
        g_rejoined = DepartRejoined();
        if (g_rejoined) UE_LOGI("[CI-DRILL] client putdepart: rejoined, idle");
    }
    return g_rejoined;
}

// putjoin: the second client to join is the joiner (a client with a lower slot is connected when it first asks).
bool Put2IsJoiner() {
    if (g_joinerKnown) return g_joiner;
    coop::roster::Snapshot r;
    coop::roster::GetSnapshot(r);
    int mine = -1;
    for (int i = 0; i < r.count; ++i)
        if (r.rows[i].isLocal) mine = r.rows[i].slot;
    if (mine < 0) return false;   // the roster has not caught up: asked again
    for (int i = 0; i < r.count; ++i) {
        const coop::roster::Row& row = r.rows[i];
        if (row.connected && !row.isHost && !row.isLocal && row.slot >= 1 && row.slot < mine) g_joiner = true;
    }
    g_joinerKnown = true;
    UE_LOGI("[CI-DRILL] client putjoin: slot %d is the %s client", mine, g_joiner ? "joining" : "first");
    return g_joiner;
}

bool Put2Hooked() {
    const Arm a = ArmOf();
    return a == Arm::PutHover || a == Arm::PutDepart || a == Arm::PutDeadline || (a == Arm::PutJoin && !Put2IsJoiner());
}

int Put2Pockets() { return ArmOf() == Arm::PutDeadline ? 2 : 1; }

void Put2BeforeAsk() {
    if (!Put2Hooked()) return;
    if (ArmOf() == Arm::PutDeadline && g_k == K::Second) return;   // the next put is not parked
    AR::dev::HoldReplay(true);
}

void Put2Hook() {
    if (!g_kMs) g_kMs = ::GetTickCount64();
    switch (g_k) {
    case K::Park:    StepPark(); return;
    case K::Hold:    StepHold(); return;
    case K::Ended:   StepEnded(); return;
    case K::Second:  StepSecond(); return;
    case K::Phantom: StepPhantom(); return;
    case K::Idle:    return;
    case K::Go:      StepGo(); return;
    case K::Release: StepRelease(); return;
    case K::Agree:   StepAgree(); return;
    case K::TakeAsk: StepTakeAsk(); return;
    case K::TakePark: StepTakePark(); return;
    case K::TakeEnd: StepTakeEnd(); return;
    case K::Restore: StepRestore(); return;
    }
}

// The verdict of an arm whose end is not putrecord's: what the act did, what the player's inventory and the mirror
// kept, and the player's line.
bool Put2Judge(const PutSettle& s) {
    const Arm arm = ArmOf();
    const PutBase b = PutBaseOf();
    const int count = CountOf(b.packEid);
    const char* const outcome = AR::dev::PutOutcomeName(s.p.outcome);
    switch (arm) {
    case Arm::PutHover: {
        UE_LOGI("[CI-DRILL] client puthover: act -> %s, own %d -> %d, container %d -> %d", outcome, b.own0, s.own, b.count0,
                count);
        if (s.p.outcome == AR::dev::PutOutcome::Aborted && s.own == b.own0 && count == b.count0 && s.stray == 0) {
            PutPass(s.stray);
            return true;
        }
        PutFail("the hovered slot's record was %s: act -> %s, own %d -> %d, container %d -> %d, stray %d",
                s.own > b.own0 || count < b.count0 ? "taken unjudged" : "not as expected", outcome, b.own0, s.own, b.count0,
                count, s.stray);
        return true;
    }
    case Arm::PutFull:
    case Arm::PutFullHand: {
        const uint8_t reason = AR::dev::LastRefusal();
        const bool line = DC::ChatHasLive(kNoSpaceLine);
        const bool kept = arm == Arm::PutFull ? s.own == b.own0 : s.held;
        UE_LOGI("[CI-DRILL] client %s: act -> %s refusal=%u line=%d item %s, container %d -> %d", ArmName(), outcome,
                static_cast<unsigned>(reason), line ? 1 : 0, kept ? "kept" : "gone", b.count0, count);
        if (s.p.outcome == AR::dev::PutOutcome::Refused &&
            reason == static_cast<uint8_t>(coop::net::ContainerActReason::NoSpace) && line && kept && count == b.count0 &&
            s.stray == 0) {
            PutPass(s.stray);
            return true;
        }
        PutFail("the full target did not refuse cleanly: act -> %s, refusal %u, line %d, item %s, container %d -> %d, stray %d",
                outcome, static_cast<unsigned>(reason), line ? 1 : 0, kept ? "kept" : "gone", b.count0, count, s.stray);
        return true;
    }
    case Arm::PutJoin: {
        UE_LOGI("[CI-DRILL] client putjoin: act -> %s, own %d -> %d, mirror %d -> %d", outcome, b.own0, s.own, b.count0, count);
        if (s.p.outcome != AR::dev::PutOutcome::Done || s.own != b.own0 - 1 || s.stray != 0) {
            PutFail("the put did not commit: act -> %s, own %d -> %d, mirror %d -> %d, stray %d", outcome, b.own0, s.own,
                    b.count0, count, s.stray);
            return true;
        }
        KNext(K::Agree);   // both clients' puts land: the mirror must come to the host's two
        PutEnterHook();
        return true;
    }
    case Arm::PutOwed: {
        UE_LOGI("[CI-DRILL] client putowed: act -> %s, own %d -> %d, mirror %d -> %d", outcome, b.own0, s.own, b.count0, count);
        if (s.p.outcome != AR::dev::PutOutcome::Done || s.own != b.own0 - 1 || count != b.count0 + 1 || s.stray != 0) {
            PutFail("the put did not store the record in the mirror: act -> %s, own %d -> %d, mirror %d -> %d, stray %d",
                    outcome, b.own0, s.own, b.count0, count, s.stray);
            return true;
        }
        KNext(K::Phantom);   // the host's insert is owed; its give-up sends the truth that takes the phantom away
        PutEnterHook();
        return true;
    }
    default:
        return false;
    }
}

const char* Put2WaitPrefix() { return ArmOf() == Arm::PutJoin && Put2IsJoiner() ? kPutJoinLine : kPutFixtureLine; }
const char* Put2DonePrefix() { return ArmOf() == Arm::PutJoin && Put2IsJoiner() ? kPutJoinDoneLine : kPutDoneLine; }

// The joiner's seed is what its mirror of the target holds the moment the fixture's mirrors are here: the host judges it
// against the contents before any put.
void Put2OnMirrors() {
    if (ArmOf() != Arm::PutJoin || !Put2IsJoiner()) return;
    const int n = CountOf(PutBaseOf().packEid);
    char say[64];
    std::snprintf(say, sizeof(say), "%sn=%d", kPutSeedLine, n);
    DC::Say(say);
    UE_LOGI("[CI-DRILL] client putjoin: the joiner's seed shows %d record(s) of the target", n);
}

void Put2Poll() {
    if (ArmOf() != Arm::PutJoin) return;
    const uint64_t now = ::GetTickCount64();
    if (now < g_nextPollMs) return;
    g_nextPollMs = now + kPollEveryMs;
    if (!g_goSeen && DC::ChatHasLive(kPutGoLine)) g_goSeen = true;
    if (!g_releaseSeen && DC::ChatHasLive(kPutReleaseLine)) g_releaseSeen = true;
}

bool Put2AskGated() {
    if (ArmOf() != Arm::PutJoin || Put2IsJoiner()) return false;
    KNext(K::Go);
    return true;
}

int PutNotReadyMode() {
    static const int mode = static_cast<int>(coop::config::ResolveInt(::coop::config_registry::rows::container_act_insert_notready));
    return mode;
}

void PutKnobRelease() { g_notReadyReleased = true; }

void Put2Reset() {
    g_k = K::Park;
    g_kMs = 0;
    g_asked2 = false;
    g_goSeen = g_releaseSeen = false;
    g_nextPollMs = 0;
    g_reopened = g_sawDrop = false;
    g_take0 = 0;
    g_notReadyReleased = false;
}

}  // namespace coop::dev::container_intent_drill::detail
