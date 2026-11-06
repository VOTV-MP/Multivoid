// coop/interactables/desk_verb_intent.cpp -- see coop/interactables/desk_verb_intent.h.

#include "coop/interactables/desk_verb_intent.h"

#include "coop/element/element.h"
#include "coop/element/intent_authority.h"
#include "coop/element/registry.h"
#include "coop/requester_feedback/requester_feedback.h"
#include "coop/interactables/signal_wire.h"
#include "coop/net/intent_bucket.h"
#include "coop/net/intent_queue.h"
#include "coop/net/net_clock.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/player/roster_ledger.h"  // PerSlotState
#include "coop/props/prop_sound.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/desk/comp_pane.h"
#include "ue_wrap/desk/console_desk.h"
#include "ue_wrap/desk/desk_press.h"
#include "ue_wrap/desk/drive_chain.h"
#include "ue_wrap/desk/saved_signals.h"
#include "ue_wrap/desk/signal_dynamic.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/profile.h"

#include <atomic>
#include <string>
#include <string_view>

namespace coop::desk_verb_intent {
namespace {

namespace EL = coop::element;
namespace sg = ue_wrap::script_gate;
namespace DP = ue_wrap::desk_press;
namespace DC = ue_wrap::drive_chain;
namespace SD = ue_wrap::signal_dynamic;
namespace DV = coop::net::desk_verb;
using coop::net::DeskVerbPayload;

constexpr int kTagPress = 0x44565001;  // 'DVP' 1
UE_FACT_FN(kRowActionOptionIndex, "desk_verb_intent", L"analogDScreenTest_C", L"actionOptionIndex", Declared);

// The seven buttons by the desk's component variables (analogDScreenTest.cpp :2860, :3048, :2606, :2691, :3128,
// :3173, :3203).
constexpr const wchar_t* kButtonMember[DV::kButtons] = {
    L"button_downl_saveSig1", L"button_downl_delSig", L"button_play_left", L"button_play_saveSig",
    L"button_comp_upload",    L"button_comp_start",   L"button_comp_stop"};
const char* const kButtonName[DV::kButtons] = {"save", "delete", "deck drive", "send", "upload", "refiner start",
                                               "refiner stop"};

// The console lanes' reach, an arm's length, measured to the desk's colliding bounds.
constexpr float kDeskReachUU = 400.0f;

// A press is a person's click; the bucket bounds a stalled client's backlog, not a player. A press past a full
// queue is dropped unanswered and said at most this often, so a flooding client costs the host no log or reply per
// message (the Kerfus lane's shape, kerfus_intent.cpp).
constexpr coop::net::IntentBudget kBudget{6.0f, 3.0f};  // presses at once, and per second after
constexpr size_t   kMaxPending = 8;
constexpr uint64_t kSayEveryMs = 10000;

std::atomic<coop::net::Session*> g_session{nullptr};
bool     g_watched = false;
bool     g_saidLive = false;
bool     g_watchRefused = false;  // refused or settled dead: said once, and the attempts end
bool     g_saidOffline = false;
uint32_t g_pressSeq = 0;  // CLIENT: this machine's press count

// One sender: its queued presses with their bucket, and the latches that clear with its occupant.
struct Slot {
    coop::net::IntentQueue<DeskVerbPayload> queue;
    uint64_t nextSayMs = 0;      // the full queue's drop, said once in kSayEveryMs
    bool     waitSaid  = false;  // a wait for the body, said once a streak
};
coop::roster_ledger::PerSlotState<Slot> g_slots;

unsigned long long g_sent = 0, g_ran = 0, g_refused = 0, g_dropped = 0, g_heard = 0;
unsigned long long g_statsSent = 0, g_statsMade = 0, g_statsLost = 0;  // op 4, the decode's profile stat

// ---- what a button acts on ----------------------------------------------------------------------------------

// As one machine's desk shows it: DeskVerbPayload's op-0 fields.
struct Seen {
    uint32_t driveEid = 0;
    uint64_t driveRow = 0, selectedRow = 0, compRow = 0;
    float    signal[4] = {};
    bool     signalArmed = false;
};

// A row's cross-peer identity, 0 for a row with no data (size 0, the empty drive's and refiner's).
uint64_t HashOf(const SD::Row& r) {
    return r.size > 0 ? coop::signal_wire::ContentHash(coop::signal_wire::Serialize(r)) : 0;
}

// The decode a start or stop acts on: the refiner's row less what a completion rewrites, its level, id and isCopy
// (analogDScreenTest.cpp :6232-6240), so a press that crossed a level-up still names the same decode.
uint64_t DecodeOf(SD::Row r) {
    r.level = 0;
    r.id.clear();
    r.isCopy = false;
    return HashOf(r);
}

uint64_t RowAt(int32_t index) {
    SD::Row r;
    if (index < 0 || index >= ue_wrap::saved_signals::Count() || !ue_wrap::saved_signals::ReadRow(index, r)) return 0;
    return HashOf(r);
}

int32_t FindRow(uint64_t hash) {
    if (!hash) return -1;
    const int32_t n = ue_wrap::saved_signals::Count();
    for (int32_t i = 0; i < n; ++i)
        if (RowAt(i) == hash) return i;
    return -1;
}

// The slot's drive, and its row when the button reads the drive's data.
void DriveIn(int role, bool withRow, Seen& out) {
    void* slot = DC::SlotActor(role);
    void* drive = slot ? DC::SlotDrive(slot) : nullptr;
    if (!drive) return;
    out.driveEid = static_cast<uint32_t>(EL::Registry::Get().EidForActor(drive));
    SD::Row r;
    if (withRow && DC::ReadDriveRow(drive, r)) out.driveRow = HashOf(r);
}

Seen Look(void* desk, uint8_t button) {
    Seen s;
    if (button == DV::kSave || button == DV::kDelete) {
        ue_wrap::console_desk::CoordSignal sig;
        if (ue_wrap::console_desk::ReadCoordSignal(sig)) {
            s.signal[0] = sig.x;
            s.signal[1] = sig.y;
            s.signal[2] = sig.z;
            s.signal[3] = sig.frequency;
            s.signalArmed = !sig.objectName.empty() && sig.objectName != L"None";
        }
    } else if (button == DV::kDeckDrive || button == DV::kDeckSend) {
        if (button == DV::kDeckDrive) DriveIn(DC::kRoleDeskPlay, true, s);
        s.selectedRow = RowAt(DP::SelectedRow(desk));
    } else if (button == DV::kUpload || button == DV::kCompStart || button == DV::kCompStop) {
        // The start asks only that a drive sits in the slot (analogDScreenTest.cpp :3180); the upload moves its data.
        if (button != DV::kCompStop) DriveIn(DC::kRoleDeskComp, button == DV::kUpload, s);
        SD::Row r;
        void* base = ue_wrap::comp_pane::CompDataPtr();
        if (base && SD::ReadStruct(base, r)) s.compRow = button == DV::kUpload ? HashOf(r) : DecodeOf(r);
    }
    return s;
}

// Where what the presser saw and what this desk shows first differ, for what the button acts on; null when
// they agree. The caught signal compares exactly: every non-catcher copy is the catcher's bytes.
const char* Differs(const DeskVerbPayload& p, const Seen& here) {
    switch (p.button) {
    case DV::kSave:
    case DV::kDelete:
        if ((p.signalArmed != 0) != here.signalArmed) return "the caught signal";
        for (int i = 0; i < 4; ++i)
            if (here.signalArmed && p.signal[i] != here.signal[i]) return "the caught signal";
        return nullptr;
    case DV::kDeckDrive:
        if (p.driveEid != here.driveEid) return "the deck's drive";
        return p.driveRow != here.driveRow ? "the deck drive's data" : nullptr;
    case DV::kUpload:
        if (p.driveEid != here.driveEid) return "the refiner's drive";
        if (p.driveRow != here.driveRow) return "the refiner drive's data";
        return p.compRow != here.compRow ? "the refiner's data" : nullptr;
    case DV::kCompStart:
        if (p.driveEid != here.driveEid) return "the refiner's drive";
        return p.compRow != here.compRow ? "the refiner's decode" : nullptr;
    case DV::kCompStop:
        return p.compRow != here.compRow ? "the refiner's decode" : nullptr;
    default:
        return nullptr;
    }
}

// Send reads the deck's selected row; the drive button does when an empty drive sits in the slot (export).
bool ReadsSelectedRow(const DeskVerbPayload& p) {
    return p.button == DV::kDeckSend || (p.button == DV::kDeckDrive && p.driveEid != 0 && p.driveRow == 0);
}

// ---- the client's gate --------------------------------------------------------------------------------------

coop::net::Session* ClientSession() {
    auto* s = g_session.load(std::memory_order_acquire);
    return (s && s->running() && s->role() == coop::net::Role::Client) ? s : nullptr;
}

int ButtonOf(void* desk, void* component) {
    if (!component) return -1;
    for (int b = 0; b < DV::kButtons; ++b)
        if (DP::Member(desk, kButtonMember[b]) == component) return b;
    return -1;
}

sg::Verdict OnPressPre(const sg::Call& c) {
    auto* s = ClientSession();
    if (!s || !c.object) return sg::Verdict::Run;  // the host's presses, its replays and single player run as ever
    const int button = ButtonOf(c.object, DP::Pressed(c.function, c.locals));
    if (button < 0) return sg::Verdict::Run;  // every other button runs where it is pressed (P1)
    const Seen seen = Look(c.object, static_cast<uint8_t>(button));
    DeskVerbPayload p{};
    p.op = DV::kOpPress;
    p.button = static_cast<uint8_t>(button);
    p.seq = ++g_pressSeq;
    p.driveEid = seen.driveEid;
    p.driveRow = seen.driveRow;
    p.selectedRow = seen.selectedRow;
    p.compRow = seen.compRow;
    for (int i = 0; i < 4; ++i) p.signal[i] = seen.signal[i];
    p.signalArmed = seen.signalArmed ? 1 : 0;
    if (!s->connected()) {
        if (!g_saidOffline) {
            g_saidOffline = true;
            UE_LOGW("desk_verb: CLIENT refused a %s press while not connected -- nothing to ask", kButtonName[button]);
        }
        return sg::Verdict::Cancel;
    }
    s->SendReliable(coop::net::ReliableKind::DeskVerb, &p, static_cast<int>(sizeof(p)));
    ++g_sent;
    UE_LOGI("desk_verb: CLIENT refused its %s press #%u and asked the host (drive eid=%u row=%016llx selected=%016llx "
            "comp=%016llx signal=%u)", kButtonName[button], p.seq, p.driveEid,
            static_cast<unsigned long long>(p.driveRow), static_cast<unsigned long long>(p.selectedRow),
            static_cast<unsigned long long>(p.compRow), static_cast<unsigned>(p.signalArmed));
    return sg::Verdict::Cancel;
}

void OnVerdict(const DeskVerbPayload& p) {
    ++g_heard;
    const char* name = p.button < DV::kButtons ? kButtonName[p.button] : "?";
    if (p.verdict == DV::kRan) {
        UE_LOGI("desk_verb: CLIENT press #%u (%s) ran on the host", p.seq, name);
        return;
    }
    UE_LOGI("desk_verb: CLIENT press #%u (%s) refused by the host (verdict %u)", p.seq, name,
            static_cast<unsigned>(p.verdict));
    coop::prop_sound::PlayDenyClick(coop::players::Registry::Get().Local());
}

// A stat's name into the payload, ASCII; false for a name the wire cannot carry.
bool PutStatName(DeskVerbPayload& p, std::wstring_view text) {
    if (text.empty() || text.size() > DV::kTextCap) return false;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] < 0x20 || text[i] > 0x7E) return false;
        p.text[i] = static_cast<char>(text[i]);
    }
    p.textLen = static_cast<uint8_t>(text.size());
    return true;
}

// CLIENT: the host's op 4, `level` added to this profile's stat the text names, as the decode would have.
void OnStat(const DeskVerbPayload& p) {
    const size_t n = p.textLen < DV::kTextCap ? p.textLen : DV::kTextCap;
    const std::wstring text(p.text, p.text + n);
    if (!ue_wrap::profile::AddStat(text, p.level)) {
        ++g_statsLost;
        UE_LOGW("desk_verb: the host's %ls %+d for press #%u did not apply here", text.c_str(), p.level, p.seq);
        return;
    }
    ++g_statsMade;
    UE_LOGI("desk_verb: CLIENT added %+d to this profile's %ls (press #%u)", p.level, text.c_str(), p.seq);
}

// ---- the host -----------------------------------------------------------------------------------------------

void Answer(coop::net::Session& s, uint8_t slot, const DeskVerbPayload& press, uint8_t verdict) {
    DeskVerbPayload a{};
    a.op = DV::kOpVerdict;
    a.button = press.button;
    a.verdict = verdict;
    a.seq = press.seq;
    s.SendReliableToSlot(slot, coop::net::ReliableKind::DeskVerb, &a, static_cast<int>(sizeof(a)));
}

void Refuse(coop::net::Session& s, uint8_t slot, const DeskVerbPayload& p, uint8_t verdict, const char* why) {
    ++g_refused;
    UE_LOGI("desk_verb: HOST refused slot %u's %s press #%u -- %s", static_cast<unsigned>(slot),
            kButtonName[p.button], p.seq, why);
    Answer(s, slot, p, verdict);
}

// One press, run or refused and then consumed, or left at the head of its queue while the host has no body for
// its presser yet or requester_feedback's seams are settling: a window, not a verdict.
bool Execute(coop::net::Session& s, const DeskVerbPayload& p, uint8_t slot) {
    void* desk = ue_wrap::console_desk::Instance();
    void* button = desk ? DP::Member(desk, kButtonMember[p.button]) : nullptr;
    if (!button || requester_feedback::Refused()) {
        Refuse(s, slot, p, DV::kUnavailable,
               button ? "requester_feedback's seams are refused" : "no desk or no button");
        return true;
    }
    if (!requester_feedback::Ready()) return false;
    const EL::IntentSubject subj = EL::IntentTarget::ForClientIntent(s, slot, kDeskReachUU).Authorize(desk);
    if (subj.outcome == EL::IntentOutcome::NoBody) {
        if (!g_slots[slot].waitSaid) {
            g_slots[slot].waitSaid = true;
            UE_LOGI("desk_verb: slot %u's desk presses wait: the host has no body for it yet",
                    static_cast<unsigned>(slot));
        }
        return false;
    }
    if (!subj) {
        Refuse(s, slot, p, DV::kFar, EL::OutcomeName(subj.outcome));
        return true;
    }
    if (const char* what = Differs(p, Look(desk, p.button))) {
        Refuse(s, slot, p, DV::kMissed, what);
        return true;
    }
    int32_t row = -1;
    if (ReadsSelectedRow(p)) {
        row = FindRow(p.selectedRow);
        if (row < 0) {
            Refuse(s, slot, p, DV::kMissed, "the selected row");
            return true;
        }
    }
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(slot);
    void* puppet = rp ? rp->GetActor() : nullptr;  // Authorize found it
    // The deck points at the presser's row, found by its hash, and stays there: the selection is the deck's one
    // index, carried between peers by desk_input_sync, and after a press it is the presser's, as in single player.
    if (row >= 0) DP::SelectRow(desk, row);
    int dropped = 0;
    bool pressed = false;
    {
        requester_feedback::Scope feedback(slot, p.seq);
        pressed = DP::PressForAnother(desk, puppet, button, &dropped);
    }
    if (!pressed) {
        Refuse(s, slot, p, DV::kUnavailable, "the press did not dispatch");
        return true;
    }
    ++g_ran;
    Answer(s, slot, p, DV::kRan);
    UE_LOGI("desk_verb: HOST ran slot %u's %s press #%u (row %d)%s", static_cast<unsigned>(slot),
            kButtonName[p.button], p.seq, row, dropped ? " -- it bound the wheel on this machine's player, dropped" : "");
    return true;
}

void Register() {
    if (!g_watched && !g_watchRefused) {
        g_watched = sg::WatchName(kRowActionOptionIndex, kTagPress, &OnPressPre, nullptr);
        if (!g_watched) {
            g_watchRefused = true;
            UE_LOGE("desk_verb: the gate took no watch on the desk's actionOptionIndex -- a client's desk press runs on "
                    "its own machine");
        }
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    Register();
}

void SendStat(uint8_t slot, uint32_t seq, const wchar_t* stat, int32_t delta) {
    DeskVerbPayload p{};
    p.op = DV::kOpStat;
    p.level = delta;
    p.seq = seq;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!stat || !PutStatName(p, stat) || !s || !s->connected() ||
        !s->SendReliableToSlot(slot, coop::net::ReliableKind::DeskVerb, &p, static_cast<int>(sizeof(p)))) {
        ++g_statsLost;
        return;
    }
    ++g_statsSent;
    UE_LOGI("desk_verb: HOST sent slot %u its %ls %+d (#%u)", static_cast<unsigned>(slot), stat, delta, seq);
}

void Tick(coop::net::Session& session) {
    Register();
    if (!g_saidLive && g_watched && !g_watchRefused) {
        sg::ResolvePendingNames();
        if (!sg::NameWatchLive(kRowActionOptionIndex, kTagPress)) {
            if (sg::NameWatchSettled(kRowActionOptionIndex, kTagPress)) {
                g_watchRefused = true;
                UE_LOGE("desk_verb: the watch on the desk's actionOptionIndex settled dead -- a client's desk press runs "
                        "on its own machine");
            }
        } else {
            g_saidLive = true;
            UE_LOGI("desk_verb: the desk's press is watched (save, delete, deck drive, send, upload, refiner start "
                    "and stop)");
        }
    }
    if (!session.running() || session.role() != coop::net::Role::Host) return;
    for (uint8_t slot = 1; slot < coop::net::kMaxPeers; ++slot) {
        Slot& sl = g_slots[slot];
        sl.queue.Serve(kBudget, coop::net::NowMs, [&](const DeskVerbPayload& p) {
            if (!Execute(session, p, slot)) return coop::net::Served::Wait;  // a wait runs nothing: no token spent
            sl.waitSaid = false;  // a consumed press ends the wait's streak
            return coop::net::Served::Done;
        });
    }
}

void OnMessage(coop::net::Session& session, const DeskVerbPayload& p, int senderSlot) {
    if (session.role() == coop::net::Role::Host) {
        if (p.op != DV::kOpPress || senderSlot < 1 || senderSlot >= coop::net::kMaxPeers || p.button >= DV::kButtons) {
            ++g_dropped;  // no client of this build sends one: counted, said at the session's end
            return;
        }
        const uint8_t slot = static_cast<uint8_t>(senderSlot);
        Slot& sl = g_slots[slot];
        DeskVerbPayload copy = p;
        if (!sl.queue.Push(std::move(copy), kMaxPending)) {
            ++g_dropped;
            const uint64_t now = coop::net::NowMs();
            if (now >= sl.nextSayMs) {
                sl.nextSayMs = now + kSayEveryMs;
                UE_LOGW("desk_verb: slot %u's queue is full (%zu) -- its presses are dropped until it drains",
                        static_cast<unsigned>(slot), sl.queue.size());
            }
        }
        return;
    }
    // A client hears DeskVerb from the host alone: the kind is never relayed.
    if (p.op == DV::kOpVerdict) OnVerdict(p);
    else if (p.op == DV::kOpStat) OnStat(p);
}

void OnDisconnect() {
    if (g_sent || g_ran || g_refused || g_dropped || g_heard)
        UE_LOGI("desk_verb: session end -- sent=%llu ran=%llu refused=%llu dropped=%llu verdicts heard=%llu", g_sent,
                g_ran, g_refused, g_dropped, g_heard);
    if (g_statsSent || g_statsMade || g_statsLost)
        UE_LOGI("desk_verb: session end -- stats sent=%llu made=%llu lost=%llu", g_statsSent, g_statsMade, g_statsLost);
    g_sent = g_ran = g_refused = g_dropped = g_heard = 0;
    g_statsSent = g_statsMade = g_statsLost = 0;
    g_saidOffline = false;
}

}  // namespace coop::desk_verb_intent
