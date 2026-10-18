// coop/interactables/laptop_tab.cpp -- see coop/interactables/laptop_tab.h.

#include "coop/interactables/laptop_tab.h"

#include "coop/interactables/device_occupancy.h"
#include "coop/net/intent_bucket.h"
#include "coop/net/latest_wins.h"
#include "coop/net/net_clock.h"
#include "coop/net/protocol.h"
#include "coop/net/refusal_tally.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/roster_ledger.h"  // PerSlotState

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/desk/device_screen.h"
#include "ue_wrap/devices/laptop_widget.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cstdint>

namespace coop::laptop_tab {
namespace {

namespace sg = ue_wrap::script_gate;
namespace UL = ue_wrap::laptop_widget;
namespace DO = coop::device_occupancy;
namespace DS = ue_wrap::device_screen;

// The three bodies that move the switcher's index: the game's own tab verb, and the two tab buttons that write the
// switcher directly (the floppy tab at the ubergraph's entry 22796, the sub-game tab at 28169). Declared on the
// widget's class, so another class's function of the name is never asked. The handlers' names keep the K2Node_ the
// pseudo-C++ drops.
UE_FACT_FN(kRowSwitched, "laptop_tab", L"ui_laptop_C", L"switcherSwitched", Declared);
UE_FACT_FN(kRowFloppyTab, "laptop_tab", L"ui_laptop_C",
           L"BndEvt__ui_laptop_button_tab_floppy_K2Node_ComponentBoundEvent_54_OnButtonClickedEvent__DelegateSignature",
           Declared);
UE_FACT_FN(kRowSubgameTab, "laptop_tab", L"ui_laptop_C",
           L"BndEvt__ui_laptop_Button_305_K2Node_ComponentBoundEvent_57_OnButtonClickedEvent__DelegateSignature",
           Declared);

constexpr int kTagSwitched = 0x4C545357;  // 'LTSW'
constexpr int kTagFloppy   = 0x4C54464C;  // 'LTFL'
constexpr int kTagSubgame  = 0x4C545347;  // 'LTSG'

struct Watch { const ue_wrap::facts::Function* row; int tag; };
const Watch kWatches[] = {
    { &kRowSwitched,   kTagSwitched },
    { &kRowFloppyTab,  kTagFloppy },
    { &kRowSubgameTab, kTagSubgame },
};
constexpr int kWatchCount = static_cast<int>(sizeof(kWatches) / sizeof(kWatches[0]));

const wchar_t* const kClaim = L"laptop";

// A tab is a click: eight at once and four a second bound only a stalled or flooding sender. The newest index is all
// that matters (coop/net/latest_wins.h), on the client's side of the wire and on the host's.
constexpr coop::net::IntentBudget kBudget{8.0f, 4.0f};

std::atomic<coop::net::Session*> g_session{nullptr};
// Each watch registers once: a refusal (a full gate table) is final, and said once. The set settles once every watch
// is live, or once no name is left to resolve and the rest are dead for good.
enum class Reg : uint8_t { Pending, Registered, Refused };
Reg  g_reg[kWatchCount] = {};
bool g_settled = false;

// What this peer, as the PC's user, last told the others: the index of its last report (a client) or of the last state
// it sent (the host). It is the holder's last word and not a copy of the canonical, which is the host's own widget; it
// only keeps a watch that fired for a call that left the tab where it was from saying the same thing twice. Cleared
// when the claim is gained or lost, when the widget changes (a world change) and at the session's end. -1: nothing said.
int   g_lastReported = -1;
void* g_reportedWidget = nullptr;  // the widget the last word was about

// CLIENT: the newest index the holder owes the host.
struct Owed {
    int idx = -1;
    coop::net::LatestWins pace;
};
Owed g_owed;
bool g_wasHolding = false;  // the claim as the last tick saw it

// HOST: each sender's newest report waiting for its pace.
struct Pending {
    int idx = -1;
    coop::net::LatestWins pace;
};
coop::roster_ledger::PerSlotState<Pending> g_pending;

// The refusals of what a peer sent, said at the first after a quiet spell and summed per window. A refusal sends the
// source no value back, as MTA's protected data sends none
// (reference/mtasa-blue/Server/mods/deathmatch/logic/CGame.cpp:2715-2796).
coop::net::RefusalTally g_refusals{"laptop_tab"};

// The drills' instruments, process-lifetime: refusals by cause, and what reached OnReport / OnState.
constexpr int kCauseCount = 4;
static_assert(static_cast<int>(Cause::NoWidget) + 1 == kCauseCount, "one counter per Cause");
uint64_t g_refused[kCauseCount] = {};
uint64_t g_received = 0;
int      g_lastReceived = -1;
bool     g_noWidgetSaid = false;

void Refuse(int sender, Cause cause, const char* why) {
    ++g_refused[static_cast<int>(cause)];
    g_refusals.Note(static_cast<uint8_t>(sender), why, coop::net::RefusalTally::Clock::now());
}

// A state that reaches a client with no widget to show it on is this peer's lack and not the sender's fault: counted,
// not charged to the sender, and said once a session.
void NoWidgetHere(const char* what) {
    ++g_refused[static_cast<int>(Cause::NoWidget)];
    if (g_noWidgetSaid) return;
    g_noWidgetSaid = true;
    UE_LOGW("laptop_tab: a tab state arrived and %s", what);
}

uint8_t LocalSlot() { return coop::players::Registry::Get().LocalPeerId(); }

// HOST: the state to every ready client but `exceptSlot` (-1: none excepted). How many it went to.
// MTA relays the client's own packet; here a new host-authored kind
// (reference/mtasa-blue/Server/mods/deathmatch/logic/CGame.cpp:2715-2796).
int FanState(coop::net::Session& s, int idx, int exceptSlot) {
    coop::net::LaptopTabPayload out{};
    out.index = static_cast<uint8_t>(idx);
    int sent = 0;
    for (int slot = 1; slot < static_cast<int>(coop::net::kMaxPeers); ++slot) {
        if (slot == exceptSlot || !s.IsSlotReady(slot)) continue;
        if (s.SendReliableToSlot(slot, coop::net::ReliableKind::LaptopTabState, &out, sizeof(out))) ++sent;
    }
    return sent;
}

void SendReport(coop::net::Session& s, int idx) {
    coop::net::LaptopTabPayload out{};
    out.index = static_cast<uint8_t>(idx);
    if (s.SendReliableToSlot(0, coop::net::ReliableKind::LaptopTabReport, &out, sizeof(out))) g_lastReported = idx;
}

// The post of a body that may have moved the tab. A viewer's widget is moved only by the apply below, which calls no
// body, so a watch that fires on a peer that does not hold the claim is not the user's and reports nothing.
void OnPost(const sg::Call& call) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;
    if (!DO::LocalHolds(kClaim)) return;
    int idx = 0;
    if (!UL::ReadTab(call.object, idx) || idx > 255) return;
    if (call.object != g_reportedWidget) {  // another widget (a world change): the last word was about the old one
        g_reportedWidget = call.object;
        g_lastReported = -1;
    }
    if (!s->connected()) return;
    if (s->role() == coop::net::Role::Host) {
        if (idx == g_lastReported) return;
        UE_LOGI("laptop_tab: slot 0 on tab %d", idx);
        if (FanState(*s, idx, -1) > 0) g_lastReported = idx;
        return;
    }
    // While an older choice waits for its token the newest is owed even when it equals the last word: the host is
    // still to be told the choice the user ended on.
    if (idx == g_lastReported && !g_owed.pace.Owed()) return;
    g_owed.idx = idx;
    if (g_owed.pace.Offer(kBudget, coop::net::NowMs())) SendReport(*s, idx);
}

void RegisterWatches() {
    for (int i = 0; i < kWatchCount; ++i) {
        if (g_reg[i] != Reg::Pending) continue;
        if (sg::WatchName(*kWatches[i].row, kWatches[i].tag, nullptr, &OnPost)) {
            g_reg[i] = Reg::Registered;
            continue;
        }
        g_reg[i] = Reg::Refused;
        UE_LOGE("laptop_tab: the script-body gate refused the watch on '%ls' -- for the rest of this process the PC "
                "user's tab is not shown to the others", kWatches[i].row->Name());
    }
}

void SettleWatches() {
    sg::ResolvePendingNames();
    if (g_settled) return;
    int live = 0, pending = 0;
    for (int i = 0; i < kWatchCount; ++i) {
        if (g_reg[i] == Reg::Pending) ++pending;
        else if (g_reg[i] == Reg::Registered && sg::NameWatchLive(*kWatches[i].row, kWatches[i].tag)) ++live;
    }
    if (live == kWatchCount) {
        g_settled = true;
        UE_LOGI("laptop_tab: the tab watches are live: switcherSwitched, the floppy tab and the sub-game tab");
    } else if (pending == 0 && sg::PendingNameCount() == 0) {
        g_settled = true;
        UE_LOGE("laptop_tab: %d of %d tab watches are dead (refused, or resolved into a full table) -- the PC user's "
                "tab is not shown to the others", kWatchCount - live, kWatchCount);
    }
}

// HOST: a report that the holder check and the pace admitted, judged against the host's own widget (resolved here, an
// index that is a tab of its switcher), written to it and sent on. The same judgement serves a report paid later,
// since the widget may have changed while it waited.
bool Apply(coop::net::Session& s, int idx, int senderSlot) {
    void* w = UL::Widget();
    int n = 0;
    if (!w || !UL::TabCount(w, n)) {
        Refuse(senderSlot, Cause::NoWidget, "the laptop widget is unresolved here");
        return false;
    }
    if (idx >= n) {
        Refuse(senderSlot, Cause::BadIndex, "a tab past the switcher's last");
        return false;
    }
    if (!UL::WriteTab(w, idx)) {
        Refuse(senderSlot, Cause::NoWidget, "the switcher refused the tab");
        return false;
    }
    UE_LOGI("laptop_tab: slot %d on tab %d", senderSlot, idx);
    FanState(s, idx, senderSlot);
    return true;
}

void PayHostOwed(coop::net::Session& s) {
    uint64_t now = 0;  // read once, and only when a report is owed
    for (int slot = 1; slot < static_cast<int>(coop::net::kMaxPeers); ++slot) {
        Pending& pend = g_pending[slot];
        if (!pend.pace.Owed()) continue;
        if (DO::HolderOf(kClaim) != static_cast<uint8_t>(slot)) {
            // It left the PC while the report waited. The report was its choice as the holder, so it is the last
            // tab it chose and it stands.
            pend.pace.Withdraw();
            Apply(s, pend.idx, slot);
            continue;
        }
        if (now == 0) now = coop::net::NowMs();
        if (!pend.pace.Due(kBudget, now)) continue;
        Apply(s, pend.idx, slot);
    }
}

// True when the local player has left the PC's screen and device_occupancy has not yet sent the claim's release: its
// poll sees the exit on the pump tick after this lane's, so the claim still reads held for this one.
bool LeavingPc() {
    void* local = coop::players::Registry::Get().Local();
    if (!local) return true;  // the world is down: the release goes with this tick's poll
    void* w = DS::ReadActiveInterface(local);
    return !w || DS::ClassifyWidgetClaimKey(w) != kClaim;
}

void PayClientOwed(coop::net::Session& s) {
    if (!g_owed.pace.Owed()) return;
    if (!DO::LocalHolds(kClaim) || !s.connected()) {  // the release has gone, or there is nobody to tell
        g_owed.pace.Withdraw();
        return;
    }
    if (LeavingPc()) {
        // The last tab the user chose goes now, ahead of the release on the same lane, so the host still reads the
        // sender as the holder when it arrives.
        SendReport(s, g_owed.idx);
        g_owed.pace.Withdraw();
        return;
    }
    if (!g_owed.pace.Due(kBudget, coop::net::NowMs())) return;
    SendReport(s, g_owed.idx);
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    RegisterWatches();
}

void Tick(coop::net::Session& session) {
    SettleWatches();
    g_refusals.Flush(coop::net::RefusalTally::Clock::now());
    if (!session.running()) return;
    const bool holds = DO::LocalHolds(kClaim);
    if (holds != g_wasHolding) {  // a term as the PC's user began or ended: the last word belongs to the one before
        g_wasHolding = holds;
        g_lastReported = -1;
    }
    if (session.role() == coop::net::Role::Host) PayHostOwed(session);
    else PayClientOwed(session);
}

void OnReport(coop::net::Session& session, const coop::net::LaptopTabPayload& p, uint8_t senderSlot) {
    if (session.role() != coop::net::Role::Host) return;
    if (senderSlot < 1 || senderSlot >= coop::net::kMaxPeers) return;
    ++g_received;
    g_lastReceived = p.index;
    // MTA lets any client author element data; here only the holder does
    // (reference/mtasa-blue/Server/mods/deathmatch/logic/CGame.cpp:2715-2796).
    if (DO::HolderOf(kClaim) != senderSlot) {
        Refuse(senderSlot, Cause::NotHolder, "a tab report from a peer that does not hold the PC");
        return;
    }
    // The widget and the index are judged once, in Apply, after the pace has admitted the report: a holder's flood
    // costs the host no engine call beyond what the budget admits.
    Pending& pend = g_pending[senderSlot];
    pend.idx = p.index;
    if (pend.pace.Offer(kBudget, coop::net::NowMs())) Apply(session, pend.idx, senderSlot);
}

void OnState(coop::net::Session& session, const coop::net::LaptopTabPayload& p, uint8_t senderSlot) {
    if (session.role() != coop::net::Role::Client) return;
    ++g_received;
    g_lastReceived = p.index;
    if (senderSlot != 0) {
        Refuse(senderSlot, Cause::BadSlot, "a tab state from a slot other than the host's");
        return;
    }
    // The user the host's claim table names (HolderOf, not this peer's own optimistic entry) is shown nothing: the host
    // never sends it a state, and its own widget is the truth. A client that entered before the host's verdict is not
    // that user yet and shows the tab like a viewer, one index (design T). An unassigned slot (0xFF) names nobody.
    const uint8_t mine = LocalSlot();
    if (mine != 0xFF && DO::HolderOf(kClaim) == mine) {
        Refuse(senderSlot, Cause::NotHolder, "a tab state while this peer holds the PC");
        return;
    }
    void* w = UL::Widget();
    int n = 0;
    if (!w || !UL::TabCount(w, n)) {
        NoWidgetHere("the laptop widget is unresolved here");
        return;
    }
    if (p.index >= n) {
        Refuse(senderSlot, Cause::BadIndex, "a tab past the switcher's last");
        return;
    }
    if (!UL::WriteTab(w, p.index)) NoWidgetHere("the switcher refused the tab");
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    void* w = UL::Widget();
    if (!w) {  // no answer to give the joiner: said, so a joiner left on tab 0 has its cause in the log
        UE_LOGW("laptop_tab: no widget to seed slot %d", peerSlot);
        return;
    }
    int idx = 0;
    if (!UL::ReadTab(w, idx) || idx > 255) return;
    coop::net::LaptopTabPayload out{};
    out.index = static_cast<uint8_t>(idx);
    s->SendReliableToSlot(peerSlot, coop::net::ReliableKind::LaptopTabState, &out, sizeof(out));
    UE_LOGI("laptop_tab: connect state -> slot %d (tab %d)", peerSlot, idx);
}

uint64_t RefusedCount(Cause cause) { return g_refused[static_cast<int>(cause)]; }
uint64_t ReceivedCount() { return g_received; }
int      LastReceivedIndex() { return g_lastReceived; }

void OnDisconnect() {
    g_lastReported = -1;
    g_reportedWidget = nullptr;
    g_wasHolding = false;
    g_noWidgetSaid = false;
    g_owed = Owed{};
    g_refusals.Clear();
}

}  // namespace coop::laptop_tab
