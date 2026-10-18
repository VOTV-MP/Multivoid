// coop/dev/laptop_tab_drill.cpp -- see coop/dev/laptop_tab_drill.h.

#include "laptop_tab_drill_steps.h"  // co-located private header (src tree, not include/)

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/interactables/device_occupancy.h"
#include "coop/interactables/laptop_tab.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"  // HasAnnouncedWorldReady

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/desk/device_screen.h"
#include "ue_wrap/facts/function_row.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace coop::dev::laptop_tab_drill {
namespace {

namespace DO = coop::device_occupancy;
namespace DS = ue_wrap::device_screen;
namespace LT = coop::laptop_tab;
namespace sg = ue_wrap::script_gate;
using coop::dev::laptop_tab_drill_steps::Abandon;
using coop::dev::laptop_tab_drill_steps::ArmBegin;
using coop::dev::laptop_tab_drill_steps::ArmReset;
using coop::dev::laptop_tab_drill_steps::ArmStarted;
using coop::dev::laptop_tab_drill_steps::ClearEndWhy;
using coop::dev::laptop_tab_drill_steps::Done;
using coop::dev::laptop_tab_drill_steps::Fail;
using coop::dev::laptop_tab_drill_steps::HeldReceived;
using coop::dev::laptop_tab_drill_steps::HolderCancelWalk;
using coop::dev::laptop_tab_drill_steps::HolderDone;
using coop::dev::laptop_tab_drill_steps::HolderIdle;
using coop::dev::laptop_tab_drill_steps::HolderReset;
using coop::dev::laptop_tab_drill_steps::HolderStart;
using coop::dev::laptop_tab_drill_steps::HolderTick;
using coop::dev::laptop_tab_drill_steps::LocalSlot;
using coop::dev::laptop_tab_drill_steps::MarkDone;
using coop::dev::laptop_tab_drill_steps::NowMs;
using coop::dev::laptop_tab_drill_steps::Pass;
using coop::dev::laptop_tab_drill_steps::ReadOwnTab;
using coop::dev::laptop_tab_drill_steps::ReaderActive;
using coop::dev::laptop_tab_drill_steps::ReaderReset;
using coop::dev::laptop_tab_drill_steps::ReaderStart;
using coop::dev::laptop_tab_drill_steps::ReaderStep;
using coop::dev::laptop_tab_drill_steps::ReaderVerdict;
using coop::dev::laptop_tab_drill_steps::ReadState;
using coop::dev::laptop_tab_drill_steps::ReceivedSince;
using coop::dev::laptop_tab_drill_steps::RefusedSince;
using coop::dev::laptop_tab_drill_steps::SetEndWhy;
using coop::dev::laptop_tab_drill_steps::VerdictOnDisconnect;
using coop::dev::laptop_tab_drill_steps::VerdictReset;
using coop::dev::laptop_tab_drill_steps::kCamerasTab;
using coop::dev::laptop_tab_drill_steps::kClaim;
using coop::dev::laptop_tab_drill_steps::kFindBoundMs;
using coop::dev::laptop_tab_drill_steps::kFloppyTab;
using coop::dev::laptop_tab_drill_steps::kRangeMark;
using coop::dev::laptop_tab_drill_steps::kRangeTab;
using coop::dev::laptop_tab_drill_steps::kSubgameMark;

// The viewer's createRC counter (tab-norc): a Declared POST watch, so a body our drill or the game runs is counted.
UE_FACT_FN(kRowCreateRc, "dev.laptop_tab_drill", L"ui_laptop_C", L"createRC", Declared);
constexpr int kTagCreateRc = 0x4C544352;  // 'LTCR'

constexpr int      kForgedTab     = 4;
constexpr int      kJoinTab       = 4;

enum class Arm : uint8_t {
    Off, Tab, TabHost, TabHandover, TabNorc, TabForged, TabSlot, TabJoin, TabSubgame, TabRange, TabRelay
};
Arm ArmOf() {
    static const Arm arm = [] {
        const std::string& v = coop::config::ResolveString(::coop::config_registry::rows::laptop_tab_drill);
        return v == "tab" ? Arm::Tab : v == "tab-host" ? Arm::TabHost : v == "tab-handover" ? Arm::TabHandover
             : v == "tab-norc" ? Arm::TabNorc : v == "tab-forged" ? Arm::TabForged : v == "tab-slot" ? Arm::TabSlot
             : v == "tab-join" ? Arm::TabJoin : v == "tab-subgame" ? Arm::TabSubgame
             : v == "tab-range" ? Arm::TabRange : v == "tab-relay" ? Arm::TabRelay : Arm::Off;
    }();
    return arm;
}
const char* ArmName() {
    switch (ArmOf()) {
    case Arm::Tab:         return "tab";
    case Arm::TabHost:     return "tab-host";
    case Arm::TabHandover: return "tab-handover";
    case Arm::TabNorc:     return "tab-norc";
    case Arm::TabForged:   return "tab-forged";
    case Arm::TabSlot:     return "tab-slot";
    case Arm::TabJoin:     return "tab-join";
    case Arm::TabSubgame:  return "tab-subgame";
    case Arm::TabRange:    return "tab-range";
    case Arm::TabRelay:    return "tab-relay";
    default:               return "off";
    }
}

// tab-join (client): the tab of the FIRST state this process received, which is the connect snapshot when it joined after
// the host set its tab. -1 until a state arrives; kFirstUnknown when several arrived before the drill looked.
constexpr int kFirstUnknown = -2;
int  g_firstIdx = -1;
bool g_liveSaid = false;

bool OwnJoinOver() {
    return coop::net_pump::HasAnnouncedWorldReady() &&
           coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle;
}

// ---- the createRC counter (tab-norc) ---------------------------------------------------------------------

uint32_t g_createRc = 0;
bool     g_rcRegistered = false;
uint64_t g_rcAskedMs = 0;
void OnCreateRcPost(const sg::Call&) { ++g_createRc; }

// True once the watch is live on this peer; registers it on the first ask. A watch the gate refused, or never made
// live, ends the arm: there would be nothing to count with.
bool CreateRcWatchLive() {
    if (g_rcAskedMs == 0) g_rcAskedMs = NowMs();
    if (!g_rcRegistered) g_rcRegistered = sg::WatchName(kRowCreateRc, kTagCreateRc, nullptr, &OnCreateRcPost);
    if (g_rcRegistered && sg::NameWatchLive(kRowCreateRc, kTagCreateRc)) return true;
    if (NowMs() - g_rcAskedMs > kFindBoundMs) Abandon("the createRC watch never went live on this peer");
    return false;
}

// ---- the arms ----------------------------------------------------------------------------------------------

constexpr int kTab3[]    = {3, kFloppyTab};
constexpr int kTab5[]    = {kCamerasTab};
constexpr int kTab3o[]   = {3};
constexpr int kTab6[]    = {6};
constexpr int kTab4[]    = {kJoinTab};
constexpr int kSubSeq[]  = {3, kSubgameMark};
constexpr int kRangeSeq[] = {kRangeMark};

// Host: reading, then waiting for the PC to be free, then holding it. Client: holding, leaving, reading.
enum class Phase : uint8_t { Start, Holding, Leaving, Reading, Watching };
Phase g_phase = Phase::Start;

int g_baseIdx = -1;

// The refusal arms (tab-forged, tab-range, tab-slot): the message carrying `marker` is to be refused for `want`, and
// this peer's own tab is to stay where it was. Judged on arrival of the message, not on a clock.
void JudgeRefusal(const char* who, LT::Cause want, int marker, const char* passWhat) {
    if (g_baseIdx < 0) {
        int idx = -1;
        if (!ReadOwnTab(idx)) return;
        g_baseIdx = idx;
        SetEndWhy("the session ended before the refused message arrived and was counted");
        UE_LOGI("laptop_tab_drill: %s READY on tab %d", who, idx);
    }
    if (ReceivedSince() == 0 || LT::LastReceivedIndex() != marker) return;
    int idx = -1;
    if (!ReadOwnTab(idx)) return;
    if (idx != g_baseIdx) {
        char what[160];
        std::snprintf(what, sizeof what, "%s applied the message it had to refuse (tab %d -> %d)", who, g_baseIdx,
                      idx);
        Fail(what);
        return;
    }
    if (RefusedSince(want) > 0) { Pass(passWhat); return; }
    Fail("the message arrived and no refusal of its cause was counted");
}

// A holder that ran its whole sequence says so: the viewer's line is the verdict.
void HolderDoneLine() {
    UE_LOGI("[LAPTOP-TAB-DRILL] %s DONE holder: switched through its tabs", ArmName());
    MarkDone();
}

void HostTick(coop::net::Session* s) {
    if (!s->running()) return;
    void* me = coop::players::Registry::Get().Local();
    if (!me) return;
    const Arm arm = ArmOf();
    switch (arm) {
    case Arm::Tab:
    case Arm::TabNorc:
    case Arm::TabSubgame: {
        if (arm == Arm::TabNorc && !CreateRcWatchLive()) return;
        if (!ReaderActive()) {
            if (!s->AnyWorldReadyPeer()) return;
            if (!ReaderStart(arm == Arm::TabNorc ? kCamerasTab : arm == Arm::TabSubgame ? kSubgameMark : kFloppyTab))
                return;
            UE_LOGI("laptop_tab_drill: host READY to read the client's tabs");
            return;
        }
        if (arm != Arm::TabNorc) {
            ReaderVerdict(arm == Arm::Tab ? "the host's widget reached tab 10 (the floppy tab), the client's last"
                                          : "the host's widget reached the sub-game tab, the client's last");
            return;
        }
        char why[160] = {};
        const ReadState r = ReaderStep(why, sizeof why);
        if (r == ReadState::Failed) { Fail(why); return; }
        if (r != ReadState::Reached) return;
        if (g_createRc != 0) {
            char what[128];
            std::snprintf(what, sizeof what, "the viewer's createRC ran %u time(s) for the cameras tab",
                          static_cast<unsigned>(g_createRc));
            Fail(what);
            return;
        }
        Pass("the viewer read the cameras tab (5) and its createRC ran 0 times");
        return;
    }
    case Arm::TabHost:
    case Arm::TabJoin:
        if (HolderIdle()) {
            if (arm == Arm::TabHost && !s->AnyWorldReadyPeer()) return;
            if (arm == Arm::TabHost) HolderStart(kTab3, 2, -1); else HolderStart(kTab4, 1, -1);
            return;
        }
        HolderTick(s);
        if (HolderDone()) {
            if (arm == Arm::TabJoin)
                UE_LOGI("[LAPTOP-TAB-DRILL] tab-join host READY on tab %d: a client may join now", kJoinTab);
            HolderDoneLine();
        }
        return;
    case Arm::TabHandover:
        if (g_phase == Phase::Start) {
            if (!s->AnyWorldReadyPeer()) return;
            if (!ReaderStart(3)) return;
            g_phase = Phase::Reading;
            return;
        }
        if (g_phase == Phase::Reading) {
            char why[160] = {};
            const ReadState r = ReaderStep(why, sizeof why);
            if (r == ReadState::Failed) { Fail(why); return; }
            if (r == ReadState::Reached) {
                ClearEndWhy();  // the host's reading is judged; what follows is the holder's leg
                g_phase = Phase::Leaving;
            }
            return;
        }
        if (g_phase == Phase::Leaving) {
            // The client left the PC when its claim is released; entering before that is a deny, not a handover.
            if (DO::HolderOf(kClaim) != 0xFF) return;
            UE_LOGI("laptop_tab_drill: host read tab 3 and the PC is free; walking to it");
            HolderStart(kTab6, 1, 3);
            g_phase = Phase::Holding;
            return;
        }
        HolderTick(s);
        if (HolderDone()) Pass("the host entered on the shared tab 3 and switched to tab 6");
        return;
    case Arm::TabForged:
        if (!s->AnyWorldReadyPeer()) return;
        JudgeRefusal("host", LT::Cause::NotHolder, kForgedTab,
                     "the host dropped and counted a report from a peer that does not hold the PC, and kept its tab");
        return;
    case Arm::TabRange:
        if (!s->AnyWorldReadyPeer()) return;
        JudgeRefusal("host", LT::Cause::BadIndex, kRangeTab,
                     "the host dropped and counted a holder's report past the switcher's last tab, and kept its tab");
        return;
    case Arm::TabSlot: {
        if (g_phase != Phase::Start || !s->AnyWorldReadyPeer()) return;
        for (int slot = 1; slot < static_cast<int>(coop::net::kMaxPeers); ++slot) {
            if (!s->IsSlotWorldReady(slot)) continue;
            coop::net::LaptopTabPayload p{};
            p.index = static_cast<uint8_t>(kForgedTab);
            // The header's origin is the host's to stamp: the late-join replay and the relay use it. Slot 2 is a peer.
            if (!s->SendReliableToSlot(slot, coop::net::ReliableKind::LaptopTabState, &p, sizeof(p), 2)) {
                Abandon("the host could not send the stamped state");
                return;
            }
            UE_LOGI("[LAPTOP-TAB-DRILL] tab-slot DONE host: sent a LaptopTabState for tab %d stamped slot 2 to slot %d",
                    kForgedTab, slot);
            g_phase = Phase::Watching;
            MarkDone();
            return;
        }
        return;
    }
    default:
        return;
    }
}

// The tab-join client's look at what it receives, from the first tick: the snapshot reaches a joiner when its world is
// ready, which is before its own join is over. A host tab that arrives after another first state was seen live, not at
// the join: the rig's rejoin marker, and the next life's first state is the snapshot.
void JoinWatch() {
    const uint64_t got = ReceivedSince();
    if (got == 0) return;
    if (g_firstIdx == -1) {
        g_firstIdx = got == 1 ? LT::LastReceivedIndex() : kFirstUnknown;
        UE_LOGI("laptop_tab_drill: the first state this process received carries tab %d", g_firstIdx);
    }
    if (g_firstIdx != kJoinTab && !g_liveSaid && LT::LastReceivedIndex() == kJoinTab) {
        g_liveSaid = true;
        UE_LOGI("[LAPTOP-TAB-DRILL] tab-join saw the host's tab live; rejoin");
    }
}

void ClientTick(coop::net::Session* s) {
    if (ArmOf() == Arm::TabJoin) JoinWatch();
    if (ArmOf() == Arm::TabNorc && !CreateRcWatchLive()) return;
    void* me = coop::players::Registry::Get().Local();
    if (!me) return;
    const Arm arm = ArmOf();
    // The stamped state reaches a joiner as soon as its world is ready, which is before its own join is over.
    if (arm == Arm::TabSlot) {
        JudgeRefusal("client", LT::Cause::BadSlot, kForgedTab,
                     "the client dropped and counted a state stamped with another sender's slot, and kept its tab");
        return;
    }
    if (!OwnJoinOver()) return;
    switch (arm) {
    case Arm::Tab:
    case Arm::TabNorc:
    case Arm::TabSubgame:
    case Arm::TabRange:
        if (HolderIdle()) {
            if (arm == Arm::TabNorc) HolderStart(kTab5, 1, -1);
            else if (arm == Arm::TabSubgame) HolderStart(kSubSeq, 2, -1);
            else if (arm == Arm::TabRange) HolderStart(kRangeSeq, 1, -1);
            else HolderStart(kTab3, 2, -1);
            return;
        }
        HolderTick(s);
        if (!HolderDone()) return;
        if (arm == Arm::TabNorc) {
            // The control: the holder's own switch ran createRC, so a watch that counted nothing could judge nothing.
            if (g_createRc == 0) { Fail("the holder's own createRC never ran, so the watch cannot judge the viewer"); return; }
            UE_LOGI("[LAPTOP-TAB-DRILL] tab-norc control: the holder's createRC ran %u time(s)",
                    static_cast<unsigned>(g_createRc));
        }
        HolderDoneLine();
        return;
    case Arm::TabHost:
    case Arm::TabJoin:
        if (arm == Arm::TabJoin && g_firstIdx != kJoinTab) return;  // only a snapshot of tab 4 proves a join
        if (!ReaderActive()) {
            if (!ReaderStart(arm == Arm::TabHost ? kFloppyTab : kJoinTab)) return;
            UE_LOGI("laptop_tab_drill: client READY to read the host's tab");
            return;
        }
        ReaderVerdict(arm == Arm::TabHost ? "the client's widget reached tab 10 (the floppy tab), the host's last"
                                          : "the joiner's widget read the host's tab 4");
        return;
    case Arm::TabHandover:
        if (g_phase == Phase::Start) {
            if (HolderIdle()) { HolderStart(kTab3o, 1, -1); return; }
            HolderTick(s);
            if (!HolderDone()) return;
            // Leave at once: the host reads tab 3 from the report, which the lane carries ahead of the claim's release.
            DS::ForceExitInterface(me);
            g_phase = Phase::Leaving;
            return;
        }
        if (g_phase == Phase::Leaving) {
            if (DO::LocalHolds(kClaim) || DO::HolderOf(kClaim) != 0xFF) return;
            UE_LOGI("laptop_tab_drill: client left the PC; reading the host's next tab");
            if (!ReaderStart(6)) return;
            g_phase = Phase::Reading;
            return;
        }
        ReaderVerdict("the client, a viewer again, reached the host's tab 6");
        return;
    case Arm::TabForged: {
        if (DO::LocalHolds(kClaim)) { Abandon("this client holds the PC, so its report would not be forged"); return; }
        coop::net::LaptopTabPayload p{};
        p.index = static_cast<uint8_t>(kForgedTab);
        if (!s->SendReliableToSlot(0, coop::net::ReliableKind::LaptopTabReport, &p, sizeof(p))) {
            Abandon("the forged report could not be sent");
            return;
        }
        UE_LOGI("[LAPTOP-TAB-DRILL] tab-forged DONE client: sent a LaptopTabReport for tab %d holding nothing",
                kForgedTab);
        MarkDone();
        return;
    }
    case Arm::TabRelay:
        // Three peers (the rig's --observer): the first client holds, the second reads, and the host relays between
        // them. The holder's own count of states is the proof that the relay left the source out.
        if (LocalSlot() != 1) {
            if (!ReaderActive()) {
                if (!ReaderStart(kFloppyTab)) return;
                UE_LOGI("laptop_tab_drill: client READY to read the other client's tabs");
                return;
            }
            ReaderVerdict("the second client's widget reached tab 10 (the floppy tab), the first client's last");
            return;
        }
        if (g_phase == Phase::Start) {
            if (HolderIdle()) {
                // The other client is here once the host relays its pose to this one: the session's own word that
                // it is in the world. A relay with no one to relay to would prove nothing.
                if (!coop::players::Registry::Get().Puppet(2)) return;
                HolderStart(kTab3, 2, -1);
                return;
            }
            HolderTick(s);
            if (!HolderDone()) return;
            // Leave at once: the host's broadcast of the release comes behind any state it sent this holder.
            DS::ForceExitInterface(me);
            g_phase = Phase::Leaving;
            return;
        }
        if (DO::LocalHolds(kClaim) || DO::HolderOf(kClaim) != 0xFF) return;
        if (LT::ReceivedCount() != HeldReceived()) {
            char what[128];
            std::snprintf(what, sizeof what, "the relay sent the source %u state(s) for its own reports",
                          static_cast<unsigned>(LT::ReceivedCount() - HeldReceived()));
            Fail(what);
            return;
        }
        Pass("the first client held the PC, switched twice, and was sent no state for its own reports");
        return;
    default:
        return;
    }
}

}  // namespace

void Tick(coop::net::Session* s) {
    if (ArmOf() == Arm::Off || !s || Done()) return;
    if (!ArmStarted()) ArmBegin(ArmName());
    if (s->role() == coop::net::Role::Host) {
        HostTick(s);
        return;
    }
    if (!s->connected()) return;
    ClientTick(s);
}

void OnDisconnect() {
    if (ArmOf() == Arm::Off) return;
    VerdictOnDisconnect();
    HolderCancelWalk();
    HolderReset();
    ReaderReset();
    g_phase = Phase::Start;
    ArmReset();
    g_baseIdx = -1;
    g_firstIdx = -1;
    g_liveSaid = false;
    VerdictReset();
}

}  // namespace coop::dev::laptop_tab_drill
