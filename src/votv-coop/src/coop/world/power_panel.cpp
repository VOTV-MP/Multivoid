// coop/world/power_panel.cpp -- see coop/world/power_panel.h.

#include "coop/world/power_panel.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/element/intent_authority.h"
#include "coop/element/registry.h"
#include "coop/net/intent_bucket.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/player/roster_ledger.h"
#include "coop/session/net_pump.h"  // IsInAnnouncedWorld: a client's own world load runs natively

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/devices/laptop.h"
#include "ue_wrap/devices/portable_pc.h"
#include "ue_wrap/devices/power_control.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <vector>

namespace coop::power_panel {
namespace {

namespace EL = coop::element;
namespace PC = ue_wrap::power_control;
namespace sg = ue_wrap::script_gate;
using coop::net::PowerPanelPayload;

static_assert(coop::net::kMaxPeers <= 4, "PowerPanelPayload::ack holds four slots");
static_assert(coop::net::kMaxPeers <= 8, "the owed canonicals are one bit a slot");

constexpr uint8_t kMaskBits = 0x1F;
constexpr uint8_t kCalcBit = 0x08;       // press_calc: the virus lockout's end switches the servers with it
constexpr uint8_t kFlagPage = 0x01;      // a press: the laptop's breaker page pressed it
constexpr uint8_t kFlagDisabled = 0x01;  // the canonical: the panel is disabled
constexpr uint8_t kNoSlot = 0xFF;
// A lever is pressed within the look-at trace and the breaker page at its terminal: twice the default armLength,
// the door and keypad lanes' reach, which coop/element/intent_authority pads with the target's bounds and the
// puppet's lag.
constexpr float kPressReachUU = 400.0f;
// A slot's presses wait, in arrival order, while the panel has not resolved or their presser has no body here
// yet (a joiner's first pose follows its world-ready by seconds, 3 s measured on the LAN rig). Each is taken as
// refused once it has waited this long, and answered while the host's panel can say its state: unlike a door verb,
// which waits for its body without a bound, a press is a prediction its presser already shows.
constexpr uint64_t kWaitMs = 10000;
constexpr size_t   kMaxQueued = 32;
// A player flips a lever or clicks the page a few times a second at most, and each press runs the panel's whole
// apply; a sender's presses run at this rate, the rest waiting their turn (door_verb_intent's bound).
constexpr coop::net::IntentBudget kPressBudget{4.0f, 4.0f};

std::atomic<coop::net::Session*> g_session{nullptr};

// The press bodies' functions, recorded as each begins, for IsBodyActive; held by slot and serial, since a world
// can load the class anew and put another function at a recorded address.
ue_wrap::CachedObjRef g_leverFn;
ue_wrap::CachedObjRef g_pageFn;
// What the panel's breakers were as each press body began, so the seam can tell what the press flipped.
uint8_t g_leverSnap = 0;
uint8_t g_pageSnap = 0;

// CLIENT: my presses the host has not taken, oldest first; the last canonical, its acknowledgements already
// taken; one that came before the panel.
struct Pending { uint16_t seq; uint8_t bits; };
std::vector<Pending> g_pending;
uint16_t g_seq = 0;
PowerPanelPayload g_canonical{};
bool g_haveCanonical = false;
PowerPanelPayload g_parked{};
bool g_haveParked = false;
uint64_t g_refusedBlackouts = 0, g_refusedLockouts = 0, g_pressesSent = 0, g_canonicalsApplied = 0;

// HOST: the last press seq taken from each slot, what the last broadcast said, the presses that wait, each
// sender's rate, and the joiners owed their canonical.
coop::roster_ledger::PerSlotState<uint16_t> g_ack;
bool    g_ackDirty = false;
uint8_t g_sentMask = 0;
bool    g_sentDisabled = false;
bool    g_haveSent = false;
uint8_t g_leverBits = 0;          // the next canonical's lever clicks
uint8_t g_leverSlot = kNoSlot;    // and whose lever made them
uint8_t g_takingFor = 0;          // the slot whose press the host's lever is running, 0 for its own
struct Waiting { PowerPanelPayload p; uint64_t arrivedMs; };
std::deque<Waiting> g_waiting[coop::net::kMaxPeers];
coop::net::IntentBucket g_rate[coop::net::kMaxPeers];
uint8_t g_owed = 0;
uint64_t g_pressesTaken = 0, g_pressesRefused = 0;

// [dev] grid_drill=red: a client applies the canonical raw, as the old mirror did, with no apply behind it.
bool RedApply() {
    static const bool red = coop::config::ResolveString(::coop::config_registry::rows::grid_drill) == "red";
    return red;
}

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// `a` is later than `b` in a 16-bit sequence that wraps.
bool SeqAfter(uint16_t a, uint16_t b) { return static_cast<int16_t>(static_cast<uint16_t>(a - b)) > 0; }

coop::net::Session* Connected() {
    auto* s = g_session.load(std::memory_order_acquire);
    return (s && s->connected()) ? s : nullptr;
}

uint8_t LocalSlot() { return coop::players::Registry::Get().LocalPeerId(); }

bool TakeToken(uint8_t slot) { return g_rate[slot].Take(kPressBudget, NowMs()); }

// ---- the host ------------------------------------------------------------------------------------------------

void HostBroadcast(coop::net::Session* s, void* panel, int onlySlot = -1) {
    uint8_t mask = 0;
    bool disabled = false;
    if (!PC::ReadPress(panel, mask) || !PC::ReadDisabled(panel, disabled)) return;
    PowerPanelPayload p{};
    p.op = coop::net::kPowerPanelOpCanonical;
    p.bits = static_cast<uint8_t>(mask & kMaskBits);
    p.flags = disabled ? kFlagDisabled : 0;
    p.leverSlot = kNoSlot;
    p.terminal = EL::kInvalidId;
    for (int i = 0; i < coop::net::kMaxPeers; ++i) p.ack[i] = g_ack[i];
    if (onlySlot >= 0) {
        s->SendReliableToSlot(onlySlot, coop::net::ReliableKind::PowerControlState, &p, sizeof(p));
        return;
    }
    p.leverBits = g_leverBits;
    p.leverSlot = g_leverSlot;
    s->SendReliable(coop::net::ReliableKind::PowerControlState, &p, sizeof(p));
    g_sentMask = p.bits;
    g_sentDisabled = disabled;
    g_haveSent = true;
    g_ackDirty = false;
    g_leverBits = 0;
    g_leverSlot = kNoSlot;
}

// Where a press is judged from: its lever's panel, or for a page press the terminal it was worked through, the
// laptop or a portable PC its presser named by element id.
EL::IntentSubject ReachOf(coop::net::Session* s, const PowerPanelPayload& p, uint8_t sender, void* panel) {
    const EL::IntentTarget target = EL::IntentTarget::ForClientIntent(*s, sender, kPressReachUU);
    if (!(p.flags & kFlagPage)) return target.Authorize(panel);
    if (p.terminal == coop::net::kPowerPanelTerminalUnnamed) return EL::IntentSubject{};
    if (p.terminal == EL::kInvalidId) {
        void* laptop = ue_wrap::laptop::Instance();
        return laptop ? target.Authorize(laptop) : EL::IntentSubject{};
    }
    EL::IntentSubject subject = target.Resolve(p.terminal, EL::ElementType::Prop);
    if (subject && !ue_wrap::portable_pc::IsPortablePc(subject.actor)) subject.outcome = EL::IntentOutcome::WrongType;
    return subject;
}

enum class Take : uint8_t { Done, Wait };

Take HostTakePress(coop::net::Session* s, const Waiting& w, uint8_t sender) {
    const PowerPanelPayload& p = w.p;
    const bool waited = NowMs() - w.arrivedMs >= kWaitMs;
    void* panel = PC::EnsureResolved() ? PC::Panel() : nullptr;
    if (!panel && !waited) return Take::Wait;
    if (!SeqAfter(p.seq, g_ack[sender])) {
        // Already taken: its presser still shows it only if no canonical has answered it yet.
        UE_LOGW("power_panel: host dropped slot %u's press seq %u (last taken %u)", sender, p.seq, g_ack[sender]);
        if (panel) HostBroadcast(s, panel, sender);
        return Take::Done;
    }
    const bool page = (p.flags & kFlagPage) != 0;
    const uint8_t bits = static_cast<uint8_t>(p.bits & kMaskBits);
    const EL::IntentSubject reach = panel ? ReachOf(s, p, sender, panel) : EL::IntentSubject{};
    if (reach.outcome == EL::IntentOutcome::NoBody && !waited) return Take::Wait;
    g_ack[sender] = p.seq;
    g_ackDirty = true;
    if (!panel) {
        UE_LOGW("power_panel: host has no panel -- slot %u's press seq %u taken and dropped", sender, p.seq);
        return Take::Done;
    }
    bool waterlogged = false;
    PC::ReadWaterlogged(panel, waterlogged);
    const bool unnamed = page && p.terminal == coop::net::kPowerPanelTerminalUnnamed;
    const char* refusal = !bits                  ? "it flips no breaker"
                          : unnamed              ? "its portable PC has no identity here yet"
                          : !reach               ? EL::OutcomeName(reach.outcome)
                          : (page && waterlogged) ? "the panel is waterlogged"  // the page's own refusal
                                                  : nullptr;
    if (refusal) {
        ++g_pressesRefused;
        UE_LOGW("power_panel: host refused slot %u's %s press 0x%02X (seq %u) -- %s (%.0f uu, allowed %.0f)", sender,
                page ? "page" : "lever", bits, p.seq, refusal, reach.distUU, reach.reachUU);
        HostBroadcast(s, panel, sender);  // its acknowledgement reverts the presser's prediction
        return Take::Done;
    }
    uint8_t cur = 0, next = 0;
    PC::ReadPress(panel, cur);
    if (page) {
        // The page's own flip, whose wait of 1.5 to 25 s the presser already served before its press came.
        PC::WritePress(panel, static_cast<uint8_t>((cur ^ bits) & kMaskBits));
        PC::ButtonsVisibility(panel);  // its seam broadcasts the canonical
    } else {
        // The game's own lever press, by the presser's puppet: its tutorial gate, its flip, its click and its
        // apply, whose seam broadcasts the canonical with this slot's click.
        coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(sender);
        void* body = (rp && rp->valid()) ? rp->GetActor() : nullptr;
        g_takingFor = sender;
        for (int b = 0; b < 5; ++b)
            if (bits & (1u << b)) PC::PressLever(panel, body, b);
        g_takingFor = 0;
    }
    PC::ReadPress(panel, next);
    if (g_ackDirty) HostBroadcast(s, panel);  // the lever's route refused it (the tutorial), or the seam is dead
    if (next == cur) {
        ++g_pressesRefused;
        UE_LOGW("power_panel: host ran slot %u's %s press 0x%02X (seq %u) and its own route flipped nothing (the "
                "tutorial's gate)", sender, page ? "page" : "lever", bits, p.seq);
        return Take::Done;
    }
    ++g_pressesTaken;
    UE_LOGI("power_panel: host took slot %u's %s press 0x%02X (seq %u): 0x%02X -> 0x%02X", sender,
            page ? "page" : "lever", bits, p.seq, cur, next);
    return Take::Done;
}

// Every waiting press of `slot` refused with its newest, whose acknowledgement answers them all: its queue is full.
void HostRefuseQueue(coop::net::Session* s, uint8_t slot, uint16_t newestSeq) {
    const size_t n = g_waiting[slot].size();
    g_waiting[slot].clear();
    if (SeqAfter(newestSeq, g_ack[slot])) g_ack[slot] = newestSeq;
    g_ackDirty = true;
    g_pressesRefused += n + 1;
    UE_LOGE("power_panel: host refused slot %u's %zu waiting presses and seq %u -- its queue is full", slot, n,
            newestSeq);
    if (void* panel = PC::Panel()) HostBroadcast(s, panel, slot);
}

// A press from the wire is taken at once when its slot has none waiting and a token to spend.
void HostOffer(coop::net::Session* s, const PowerPanelPayload& p, uint8_t sender) {
    if (sender == 0 || sender >= coop::net::kMaxPeers) return;
    auto& q = g_waiting[sender];
    const Waiting w{p, NowMs()};
    if (q.empty() && TakeToken(sender)) {
        if (HostTakePress(s, w, sender) == Take::Done) return;
        g_rate[sender].Refund(kPressBudget);  // a wait runs nothing, so it spends no token
    }
    if (q.size() >= kMaxQueued) {
        HostRefuseQueue(s, sender, p.seq);
        return;
    }
    q.push_back(w);
}

void HostDrainWaiting(coop::net::Session* s) {
    for (uint8_t slot = 1; slot < coop::net::kMaxPeers; ++slot) {
        auto& q = g_waiting[slot];
        while (!q.empty() && TakeToken(slot)) {
            if (HostTakePress(s, q.front(), slot) == Take::Wait) {
                g_rate[slot].Refund(kPressBudget);
                break;
            }
            q.pop_front();
        }
    }
}

// The canonical each owed joiner waits for, once the panel resolves.
void HostServeOwed(coop::net::Session* s) {
    if (!g_owed || !PC::EnsureResolved()) return;
    void* panel = PC::Panel();
    if (!panel) return;
    for (int slot = 1; slot < coop::net::kMaxPeers; ++slot)
        if (g_owed & (1u << slot)) HostBroadcast(s, panel, slot);
    g_owed = 0;
}

// ---- a client ------------------------------------------------------------------------------------------------

// The canonical with my untaken presses on top, onto the panel when they differ. A fresh canonical also plays the
// clicks of another player's lever; a re-assertion plays nothing.
void ClientApplyModel(void* panel, const PowerPanelPayload& p, bool fresh) {
    uint8_t model = static_cast<uint8_t>(p.bits & kMaskBits);
    for (const Pending& d : g_pending) model ^= d.bits;
    const bool disabled = (p.flags & kFlagDisabled) != 0;
    uint8_t cur = 0;
    bool curDisabled = false;
    if (!PC::ReadPress(panel, cur) || !PC::ReadDisabled(panel, curDisabled)) return;
    if (cur != model || curDisabled != disabled) {
        PC::WriteDisabled(panel, disabled);
        PC::WritePress(panel, model);
        if (!RedApply()) {
            PC::ButtonsVisibility(panel);
            // The desk virus's lockout (virus_pb) switches the servers off as it starts, and as it ends switches them
            // on with the calc breaker and plays the turn-on cue; the host's 60 s is its only clock, so each half
            // runs here as its edge arrives.
            if (disabled != curDisabled) {
                PC::SetServersActive(panel, !disabled && (model & kCalcBit) != 0);
                if (!disabled) PC::PlayTurnOnCue(panel);
            }
        }
        ++g_canonicalsApplied;
        UE_LOGI("power_panel: canonical 0x%02X%s %s, the panel was 0x%02X%s (%zu press(es) of mine on top)", p.bits,
                disabled ? " disabled" : "", fresh ? "applied" : "re-asserted", cur, curDisabled ? " disabled" : "",
                g_pending.size());
    }
    if (fresh && p.leverBits && p.leverSlot != LocalSlot())
        for (int b = 0; b < 5; ++b)
            if (p.leverBits & (1u << b)) PC::PlayLeverSound(panel, (model >> b) & 1);
}

void ClientTakeCanonical(const PowerPanelPayload& p) {
    const uint8_t me = LocalSlot();
    const uint16_t ack = me < coop::net::kMaxPeers ? p.ack[me] : 0;
    size_t kept = 0;
    for (const Pending& d : g_pending)
        if (SeqAfter(d.seq, ack)) g_pending[kept++] = d;
    g_pending.resize(kept);
    g_canonical = p;
    g_haveCanonical = true;
    void* panel = PC::EnsureResolved() ? PC::Panel() : nullptr;
    if (!panel) {
        g_parked = p;
        g_haveParked = true;
        return;
    }
    g_haveParked = false;
    ClientApplyModel(panel, p, true);
}

// ---- the watches ---------------------------------------------------------------------------------------------

sg::Verdict OnLeverPre(const sg::Call& call) {
    if (!g_leverFn.Is(call.function)) g_leverFn.Set(call.function);
    if (call.object) PC::ReadPress(call.object, g_leverSnap);
    return sg::Verdict::Run;
}

// Every entry of the page's ubergraph, its click and its latent flip after the delay alike, so the snapshot is
// the breakers as the flip's own entry began.
sg::Verdict OnPagePre(const sg::Call& call) {
    if (!g_pageFn.Is(call.function)) g_pageFn.Set(call.function);
    if (void* panel = PC::Panel()) PC::ReadPress(panel, g_pageSnap);
    return sg::Verdict::Run;
}

// The seam: every press and every blackout ends in the panel's buttonsVisibility.
sg::Verdict OnSeamPre(const sg::Call& call) {
    auto* s = Connected();
    if (!s || !call.object) return sg::Verdict::Run;
    void* const leverFn = g_leverFn.Get();
    void* const pageFn = g_pageFn.Get();
    const bool lever = leverFn && sg::IsBodyActive(leverFn);
    const bool page = !lever && pageFn && sg::IsBodyActive(pageFn);
    uint8_t cur = 0;
    if (!PC::ReadPress(call.object, cur)) return sg::Verdict::Run;
    if (s->role() == coop::net::Role::Host) {
        if (lever) {  // a lever press, the host's own or a client's it runs: every other peer plays its click
            g_leverBits = static_cast<uint8_t>((cur ^ g_leverSnap) & kMaskBits);
            g_leverSlot = g_takingFor;
        }
        bool disabled = false;
        PC::ReadDisabled(call.object, disabled);
        if (!g_haveSent || cur != g_sentMask || disabled != g_sentDisabled || g_ackDirty)
            HostBroadcast(s, call.object);
        return sg::Verdict::Run;
    }
    if (!lever && !page) return sg::Verdict::Run;  // a client's panel moves only by its own press or the host
    uint8_t& snap = lever ? g_leverSnap : g_pageSnap;
    const uint8_t delta = static_cast<uint8_t>((cur ^ snap) & kMaskBits);
    snap = cur;
    if (!delta) return sg::Verdict::Run;
    PowerPanelPayload p{};
    p.op = coop::net::kPowerPanelOpPress;
    p.bits = delta;
    p.flags = page ? kFlagPage : 0;
    p.leverSlot = kNoSlot;
    p.terminal = EL::kInvalidId;
    if (page) {
        // The page is worked through the laptop or a portable PC; the host measures the presser against it.
        void* term = ue_wrap::laptop::TerminalInUse();
        if (term && ue_wrap::portable_pc::IsPortablePc(term)) {
            const EL::ElementId eid = EL::Registry::Get().EidForActor(term);
            p.terminal = eid == EL::kInvalidId ? coop::net::kPowerPanelTerminalUnnamed : eid;
        }
    }
    p.seq = ++g_seq;
    g_pending.push_back({p.seq, delta});
    s->SendReliableToSlot(0, coop::net::ReliableKind::PowerControlState, &p, sizeof(p));
    ++g_pressesSent;
    UE_LOGI("power_panel: local %s press flipped 0x%02X (seq %u) -- sent to the host", page ? "page" : "lever",
            delta, p.seq);
    return sg::Verdict::Run;
}

// A client never blacks its panel out nor locks it on its own: the host's blackout and the host's lockout arrive
// as its canonical. A blackout inside a verb of ours (a generator's break the host's rows run here) is let through
// for what else solar() does, and its breakers are put back to the canonical after (ReassertCanonical).
sg::Verdict OnOwnEdgePre(const sg::Call& call) {
    auto* s = Connected();
    if (!s || s->role() != coop::net::Role::Client || call.fromOurCode) return sg::Verdict::Run;
    if (!coop::net_pump::IsInAnnouncedWorld(call.object)) return sg::Verdict::Run;
    const bool lockout = call.tag == 0x50505650 /*'PPVP'*/;
    if (++(lockout ? g_refusedLockouts : g_refusedBlackouts) == 1)
        UE_LOGI("power_panel: this client refuses its own %s -- the host's panel is its author",
                lockout ? "virus_pb lockout" : "solar() blackout");
    return sg::Verdict::Cancel;
}

UE_FACT_FN(kRowButtonsVisibility, "power_panel", L"powerControl_C", L"buttonsVisibility", Declared);
UE_FACT_FN(kRowActionOptionIndex, "power_panel", L"powerControl_C", L"actionOptionIndex", Declared);
UE_FACT_FN(kRowUbergraphBreakerComp, "power_panel", L"ui_breakerComp_C", L"ExecuteUbergraph_ui_breakerComp",
           Declared);
UE_FACT_FN(kRowSolar, "power_panel", L"powerControl_C", L"solar", Declared);
UE_FACT_FN(kRowVirusPb, "power_panel", L"powerControl_C", L"virus_pb", Declared);

struct WatchDef {
    const ue_wrap::facts::Function* row;
    int tag;
    sg::PreFn pre;
};
constexpr WatchDef kWatches[] = {
    { &kRowButtonsVisibility,    0x50504256 /*'PPBV'*/, &OnSeamPre },
    { &kRowActionOptionIndex,    0x5050414F /*'PPAO'*/, &OnLeverPre },
    { &kRowUbergraphBreakerComp, 0x50505047 /*'PPPG'*/, &OnPagePre },
    { &kRowSolar,                0x5050534C /*'PPSL'*/, &OnOwnEdgePre },
    { &kRowVirusPb,              0x50505650 /*'PPVP'*/, &OnOwnEdgePre },
};
constexpr int kWatchCount = static_cast<int>(sizeof(kWatches) / sizeof(kWatches[0]));
bool g_registered = false;
bool g_settled = false;

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_registered) return;
    g_registered = true;
    for (const WatchDef& w : kWatches)
        if (!sg::WatchName(*w.row, w.tag, w.pre, nullptr))
            UE_LOGE("power_panel: the script-body gate refused the watch on %ls::%ls", w.row->Class(),
                    w.row->Name());
}

void Tick() {
    auto* s = Connected();
    if (s && s->role() == coop::net::Role::Host) {
        HostDrainWaiting(s);
        HostServeOwed(s);
    } else if (s) {
        // A client's panel resolves before its first canonical, so a press made before one is caught too.
        if (PC::EnsureResolved() && g_haveParked && PC::Panel()) ClientTakeCanonical(g_parked);
    }
    if (g_settled || !g_registered) return;
    sg::ResolvePendingNames();
    int live = 0, settled = 0;
    for (const WatchDef& w : kWatches) {
        if (sg::NameWatchLive(*w.row, w.tag)) ++live;
        if (sg::NameWatchSettled(*w.row, w.tag)) ++settled;
    }
    if (live == kWatchCount) {
        g_settled = true;
        UE_LOGI("power_panel: the panel's gates are live (buttonsVisibility, the lever, the breaker page, solar, "
                "the virus lockout)");
    } else if (settled == kWatchCount) {
        g_settled = true;
        UE_LOGE("power_panel: %d of %d panel gates are dead -- presses on those surfaces do not reach the host",
                kWatchCount - live, kWatchCount);
    }
}

void OnReliable(const coop::net::PowerPanelPayload& payload, uint8_t senderSlot) {
    auto* s = Connected();
    if (!s) return;
    if (s->role() == coop::net::Role::Host) {
        if (payload.op == coop::net::kPowerPanelOpPress) HostOffer(s, payload, senderSlot);
        return;
    }
    if (payload.op == coop::net::kPowerPanelOpCanonical && senderSlot == 0) ClientTakeCanonical(payload);
}

void QueueConnectBroadcastForSlot(int slot) {
    auto* s = Connected();
    if (!s || s->role() != coop::net::Role::Host || slot <= 0 || slot >= coop::net::kMaxPeers) return;
    g_owed |= static_cast<uint8_t>(1u << slot);
    HostServeOwed(s);
    if (g_owed & (1u << slot))
        UE_LOGI("power_panel: slot %d's canonical waits for the host's panel to resolve", slot);
}

void OnPeerLeft(uint8_t slot) {
    if (slot == 0 || slot >= coop::net::kMaxPeers) return;
    g_waiting[slot].clear();
    g_rate[slot].Reset();
    g_owed = static_cast<uint8_t>(g_owed & ~(1u << slot));
}

void ReassertCanonical() {
    auto* s = Connected();
    if (!s || s->role() != coop::net::Role::Client || !g_haveCanonical || RedApply()) return;
    if (void* panel = PC::Panel()) ClientApplyModel(panel, g_canonical, false);
}

size_t PendingPresses() { return g_pending.size(); }
uint64_t ClientPressesSent() { return g_pressesSent; }
uint64_t HostPressesTaken() { return g_pressesTaken; }

bool LastCanonical(uint8_t& bits, bool& disabled) {
    if (!g_haveCanonical) return false;
    bits = static_cast<uint8_t>(g_canonical.bits & kMaskBits);
    disabled = (g_canonical.flags & kFlagDisabled) != 0;
    return true;
}

void OnDisconnect() {
    if (g_pressesSent || g_canonicalsApplied || g_refusedBlackouts || g_refusedLockouts || g_pressesTaken ||
        g_pressesRefused)
        UE_LOGI("power_panel: session end -- presses sent %llu, canonicals applied %llu, own blackouts refused %llu, "
                "own lockouts refused %llu; as host: presses taken %llu, refused %llu",
                static_cast<unsigned long long>(g_pressesSent), static_cast<unsigned long long>(g_canonicalsApplied),
                static_cast<unsigned long long>(g_refusedBlackouts),
                static_cast<unsigned long long>(g_refusedLockouts), static_cast<unsigned long long>(g_pressesTaken),
                static_cast<unsigned long long>(g_pressesRefused));
    g_pending.clear();
    g_seq = 0;
    g_haveCanonical = false;
    g_haveParked = false;
    g_haveSent = false;
    g_ackDirty = false;
    g_leverBits = 0;
    g_leverSlot = kNoSlot;
    g_takingFor = 0;
    g_leverFn.Reset();
    g_pageFn.Reset();
    for (int slot = 0; slot < coop::net::kMaxPeers; ++slot) {
        g_waiting[slot].clear();
        g_rate[slot].Reset();
    }
    g_owed = 0;
    g_pressesSent = g_canonicalsApplied = g_refusedBlackouts = g_refusedLockouts = 0;
    g_pressesTaken = g_pressesRefused = 0;
}

}  // namespace coop::power_panel
