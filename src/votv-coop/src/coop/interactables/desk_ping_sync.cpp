// coop/interactables/desk_ping_sync.cpp -- see coop/interactables/desk_ping_sync.h.

#include "coop/interactables/desk_ping_sync.h"

#include "coop/interactables/desk_input_sync.h"  // PingActiveSlot, AttributePrimedRun: whose ping runs on the desk
#include "coop/interactables/device_occupancy.h"
#include "coop/interactables/signal_catch_sync.h"
#include "coop/net/intent_bucket.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/desk/console_desk.h"
#include "ue_wrap/desk/coords_panel.h"
#include "ue_wrap/desk/desk_ping.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/profile.h"
#include "ue_wrap/world/game_mode.h"

#include <atomic>
#include <chrono>
#include <cmath>

namespace coop::desk_ping_sync {
namespace {

namespace CD = ue_wrap::console_desk;
namespace CP = ue_wrap::coords_panel;
namespace DP = ue_wrap::desk_ping;
namespace PV = coop::net::desk_ping;
namespace sg = ue_wrap::script_gate;
using coop::net::Role;

std::atomic<coop::net::Session*> g_session{nullptr};
Counts g_counts;

// A client sends one verdict a ping, and a ping runs for seconds; past this a sender's intents are dropped unanswered
// and said at most every kSayEveryMs. An insta-catch is a click in the cheat menu, and has a budget of its own, so
// the clicks spend nothing a ping needs.
constexpr coop::net::IntentBudget kBudget{4.0f, 0.5f};
constexpr coop::net::IntentBudget kInstaBudget{4.0f, 2.0f};
constexpr uint64_t kSayEveryMs = 10000;
coop::net::IntentBucket g_budget[coop::net::kMaxPeers];
coop::net::IntentBucket g_instaBudget[coop::net::kMaxPeers];
uint64_t g_nextSayMs[coop::net::kMaxPeers] = {};
// A refusal is answered every time and said at most every kSayEveryMs a sender, the ones between counted.
uint64_t g_nextRefuseSayMs[coop::net::kMaxPeers] = {};
uint32_t g_refusedQuiet[coop::net::kMaxPeers] = {};
constexpr uint64_t kProgressIntervalMs = 100;
bool g_publishingProgress = false;
uint32_t g_progressSeq = 0;
uint64_t g_nextProgressMs = 0;
int g_mirrorSlot = -1;
uint32_t g_mirrorSeq = 0;
bool g_mirrorActive = false;
DP::Progress g_mirrorProgress{};

uint64_t NowMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

coop::net::Session* SessionAs(Role role) {
    auto* s = g_session.load(std::memory_order_acquire);
    return (s && s->running() && s->connected() && s->role() == role) ? s : nullptr;
}

void Send(coop::net::Session* s, int slot, uint8_t op, uint32_t seq, uint8_t reason = PV::kRefusedLost) {
    coop::net::DeskPingVerdictPayload p{};
    p.op = op;
    p.reason = reason;
    p.seq = seq;
    s->SendReliableToSlot(slot, coop::net::ReliableKind::DeskPingVerdict, &p, static_cast<int>(sizeof(p)));
}

// The watches, by what each serves (Register): the verdict's is the gate itself and the host's roll, the coordinate
// process's the host's priming and the scope of a client's hold.
enum Watch { kWatchVerdict, kWatchProcess, kWatchLogLine, kWatchSound, kWatchInsta, kWatchCount };
bool g_live[kWatchCount] = {};

// ---- the client's verdict, refused and sent ------------------------------------------------------------------------

// The body whose verdict the gate refused, a process_coords or the cheat menu's isntaCatchSignal, holds back the
// failure line and sound that follow in it: the mark is set by the refusal, cleared by either body's next pre, and read
// only while that body runs.
bool  g_verdictRefused = false;
void* g_processFn = nullptr;  // the desk's process_coords, as its own pre names it
void* g_instaFn = nullptr;    // the desk's isntaCatchSignal, as its own pre names it
void* g_refusedFn = nullptr;  // the one of the two the refused verdict ran in
uint32_t g_seq = 0;
bool g_saidUnsent = false;

bool InRefusedBody() { return g_verdictRefused && g_refusedFn && sg::IsBodyActive(g_refusedFn); }

sg::Verdict OnVerdictPre(const sg::Call& c) {
    auto* s = SessionAs(Role::Client);
    if (!s) return sg::Verdict::Run;
    // A client in a session rolls no verdict: the host rolls it from what this one sends.
    const bool insta = g_instaFn && sg::IsBodyActive(g_instaFn);
    const bool written = DP::WriteNoVerdict(c);
    CP::DishAim aim;
    if (!written || !CP::ReadDishAim(aim)) {
        // Unsent, the verdict is nobody's, and the body's own no-signal branch shows the ping failed: its outputs are
        // written false, or left as every earlier verdict of this session wrote them.
        ++g_counts.unsent;
        if (!g_saidUnsent) {
            g_saidUnsent = true;
            UE_LOGE("desk_ping: CLIENT could not %s -- its pings fail here unsent",
                    written ? "read its aim" : "write gatherSignal's outputs");
        }
        return sg::Verdict::Cancel;
    }
    coop::net::DeskPingVerdictPayload p{};
    p.op = insta ? PV::kOpInsta : PV::kOpIntent;
    p.seq = ++g_seq;
    p.viewX = aim.viewX;
    p.viewY = aim.viewY;
    p.aim.c0X = aim.c0X;
    p.aim.c0Y = aim.c0Y;
    p.aim.c1X = aim.c1X;
    p.aim.c1Y = aim.c1Y;
    p.aim.c2X = aim.c2X;
    p.aim.c2Y = aim.c2Y;
    p.aim.selected = aim.selected;
    p.aim.direction = aim.direction;
    s->SendReliableToSlot(0, coop::net::ReliableKind::DeskPingVerdict, &p, static_cast<int>(sizeof(p)));
    g_verdictRefused = true;
    g_refusedFn = insta ? g_instaFn : g_processFn;
    ++g_counts.refused;
    UE_LOGI("desk_ping: CLIENT sent its %s verdict #%u to the host", insta ? "insta-catch's" : "ping's", p.seq);
    return sg::Verdict::Cancel;
}

sg::Verdict OnLogLinePre(const sg::Call&) {
    if (!InRefusedBody()) return sg::Verdict::Run;
    ++g_counts.outputsHeld;
    return sg::Verdict::Cancel;
}

sg::Verdict OnInstaPre(const sg::Call& c) {
    g_instaFn = c.function;
    g_verdictRefused = false;
    return sg::Verdict::Run;
}

// ---- the host's run of a client's verdict ---------------------------------------------------------------------------

// A client's verdict on the host. A ping's is taken by its intent and primed into the machine by the pre of the desk's
// next process_coords, so that very body rolls it; an insta-catch's runs at once, as the host's own isntaCatchSignal.
struct HostRun {
    uint8_t  slot = 0xFF;
    uint32_t seq = 0;
    ue_wrap::CachedObjRef desk;  // the desk the intent's aim was written into, by slot and serial
    bool     queued = false;     // a ping's, taken and not yet primed
    bool     primed = false;     // primed into the process_coords body running now, or the last one
    bool     pending = false;    // running, its verdict not yet rolled
    bool     haveFound = false;
    int32_t  foundBefore = 0;    // the host's own signals_found before the verdict
};
HostRun g_run;

void OnVerdictPost(const sg::Call& c) {
    if (!g_run.pending) return;
    g_run.pending = false;
    auto* s = SessionAs(Role::Host);
    if (!s) return;
    ++g_counts.rolled;
    bool caught = false;
    DP::ReadCaught(c, caught);
    if (caught) {
        ++g_counts.caught;
        // The find is the pinger's: the host's own count is put back, and the pinger's machine adds its own.
        int32_t after = 0;
        if (g_run.haveFound && ue_wrap::profile::ReadSignalsFound(after) && after > g_run.foundBefore)
            ue_wrap::profile::AddSignalsFound(g_run.foundBefore - after);
        Send(s, g_run.slot, PV::kOpFound, g_run.seq);
        CD::CoordSignal sig;
        if (DP::ReadVerdictSignal(c, sig))
            coop::signal_catch_sync::AttributeCatch(sig.x, sig.y, sig.z, sig.frequency, g_run.slot);
        else
            UE_LOGW("desk_ping: HOST could not read the signal slot %u's verdict caught -- it relays as the host's",
                    static_cast<unsigned>(g_run.slot));
    }
    UE_LOGI("desk_ping: HOST rolled slot %u's verdict #%u: %s", static_cast<unsigned>(g_run.slot), g_run.seq,
            caught ? "a catch" : "no catch");
}

void HostProcessPre(const sg::Call& c) {
    auto* s = SessionAs(Role::Host);
    if (!s || c.object != CD::Instance()) return;
    if (g_run.primed) {
        // The primed body has ended. Its verdict rolled in it, unless the machine did not reach it: then the machine
        // is put back idle, since a primed machine left alone would roll the verdict as the host's own ping.
        g_run.primed = false;
        if (g_run.pending) {
            DP::ResetMachine(c.object);
            coop::desk_input_sync::AttributePrimedRun(0xFF);
            g_run.pending = false;
            Send(s, g_run.slot, PV::kOpRefused, g_run.seq);
            UE_LOGW("desk_ping: HOST's machine did not roll slot %u's primed verdict #%u -- put back idle",
                    static_cast<unsigned>(g_run.slot), g_run.seq);
        }
    }
    if (!g_run.queued) return;
    g_run.queued = false;
    // The intent's aim went into the desk it was taken on; a desk born since, as a reloaded level's, never had it.
    bool pinging = false;
    const bool sameDesk = g_run.desk.Is(c.object);
    if (!sameDesk || !DP::ReadPinging(c.object, pinging) || pinging || !DP::PrimeVerdict(c.object)) {
        Send(s, g_run.slot, PV::kOpRefused, g_run.seq, sameDesk && pinging ? PV::kRefusedBusy : PV::kRefusedLost);
        UE_LOGW("desk_ping: HOST could not prime slot %u's verdict #%u (%s)", static_cast<unsigned>(g_run.slot),
                g_run.seq,
                !sameDesk ? "the desk it was taken on is gone"
                : pinging ? "its desk began pinging"
                          : "its machine does not read");
        return;
    }
    g_run.primed = g_run.pending = true;
    coop::desk_input_sync::AttributePrimedRun(g_run.slot);
    ++g_counts.primed;
    UE_LOGI("desk_ping: HOST primed slot %u's verdict #%u into its desk's coordinate process",
            static_cast<unsigned>(g_run.slot), g_run.seq);
}

void OnProcessPost(const sg::Call& c) {
    // Reapply only the renderer-facing values after this peer's idle local machine tick. The run flag stays clear;
    // otherwise every observer would start an independent Blueprint scan.
    if (g_mirrorActive && c.object == CD::Instance()) DP::WriteProgress(c.object, g_mirrorProgress);
}

sg::Verdict OnProcessPre(const sg::Call& c) {
    g_processFn = c.function;
    g_verdictRefused = false;
    HostProcessPre(c);
    return sg::Verdict::Run;
}

sg::Verdict OnPingSoundPre(const sg::Call& c) {
    if (InRefusedBody()) {
        ++g_counts.outputsHeld;
        return sg::Verdict::Cancel;
    }
    // The primed body's stage-change cue: the pinger's machine played its own at its stage change. The verdict's
    // sound follows in the same body and plays.
    const bool primedCue = g_run.primed && g_processFn && sg::IsBodyActive(g_processFn) && DP::IsStageChangeCue(c);
    return primedCue ? sg::Verdict::Cancel : sg::Verdict::Run;
}

CP::DishAim AimOf(const coop::net::DeskPingVerdictPayload& p) {
    CP::DishAim aim;
    aim.viewX = p.viewX;
    aim.viewY = p.viewY;
    aim.c0X = p.aim.c0X;
    aim.c0Y = p.aim.c0Y;
    aim.c1X = p.aim.c1X;
    aim.c1Y = p.aim.c1Y;
    aim.c2X = p.aim.c2X;
    aim.c2Y = p.aim.c2Y;
    aim.selected = p.aim.selected;
    aim.direction = p.aim.direction;
    return aim;
}

// The values go into the host's own coordinate panel and its verdict, as the aim lane's do (console_state_sync).
bool Finite(const CP::DishAim& a) {
    const float vals[] = {a.viewX, a.viewY, a.c0X, a.c0Y, a.c1X, a.c1Y, a.c2X, a.c2Y};
    for (float v : vals)
        if (!std::isfinite(v)) return false;
    return true;
}

void OnIntent(coop::net::Session& s, const coop::net::DeskPingVerdictPayload& p, uint8_t slot) {
    const bool insta = p.op == PV::kOpInsta;
    const uint64_t now = NowMs();
    if (!(insta ? g_instaBudget[slot].Take(kInstaBudget, now) : g_budget[slot].Take(kBudget, now))) {
        ++g_counts.overBudget;
        if (now >= g_nextSayMs[slot]) {
            g_nextSayMs[slot] = now + kSayEveryMs;
            UE_LOGW("desk_ping: HOST drops slot %u's verdicts past its budget (%u so far)", static_cast<unsigned>(slot),
                    g_counts.overBudget);
        }
        return;
    }
    void* desk = CD::EnsureResolved() ? CD::Instance() : nullptr;
    bool pinging = false;
    // A ping's verdict is primed through the coordinate process's watch; every verdict is counted through its own.
    const bool rolls = g_live[kWatchVerdict] && (insta || g_live[kWatchProcess]);
    const bool readable = rolls && desk && DP::ReadPinging(desk, pinging);
    // A ping runs from the desk its sender holds; the cheat menu's insta-catch from anywhere, as its key has no
    // interface gate (mainPlayer.cpp:2457-2490), on the aim of whoever holds the desk.
    const bool holds = coop::device_occupancy::HolderOf(L"desk") == slot;
    const bool claimed = insta || holds;
    const CP::DishAim aim = AimOf(p);
    const bool finite = Finite(aim);
    const bool busy = pinging || g_run.queued || g_run.pending;
    // A ping's verdict comes from a ping its sender runs, as the input lane reports it, on a triangle the atlas pings;
    // an insta-catch's, from a game that lets its players cheat, the host's being the world's (read off the
    // gamemode, never the gate that can quit a game).
    bool cheats = false;
    const bool from = !insta ? coop::desk_input_sync::PingActiveSlot() == slot && CP::AimCanPing(aim)
                             : ue_wrap::game_mode::CheatsAllowed(cheats) && cheats;
    if (!readable || !claimed || !finite || !from || busy) {
        const bool onlyBusy = readable && claimed && finite && from;
        if (onlyBusy) ++g_counts.busy;
        Send(&s, slot, PV::kOpRefused, p.seq, onlyBusy ? PV::kRefusedBusy : PV::kRefusedLost);
        if (now < g_nextRefuseSayMs[slot]) {
            ++g_refusedQuiet[slot];
            return;
        }
        g_nextRefuseSayMs[slot] = now + kSayEveryMs;
        UE_LOGI("desk_ping: HOST refused slot %u's %s verdict #%u (%s; %u more refused unsaid before it)",
                static_cast<unsigned>(slot), insta ? "insta-catch's" : "ping's", p.seq,
                !readable  ? "its desk or its watches are not live"
                : !claimed ? "the sender does not hold the desk"
                : !finite  ? "its view or aim is not finite"
                : !from    ? (insta ? "the host's game does not let its players cheat"
                                    : "the sender runs no ping on a pingable triangle")
                           : "its desk is pinging or holds another verdict",
                g_refusedQuiet[slot]);
        g_refusedQuiet[slot] = 0;
        return;
    }
    if (holds) {
        CP::WriteCursorOnly(aim.viewX, aim.viewY);
        CP::WriteDishCommitted(aim);
    }
    g_run = HostRun{};
    g_run.slot = slot;
    g_run.seq = p.seq;
    g_run.desk.Set(desk);
    g_run.haveFound = ue_wrap::profile::ReadSignalsFound(g_run.foundBefore);
    if (!insta) {
        g_run.queued = true;
        UE_LOGI("desk_ping: HOST took slot %u's verdict #%u for its desk's next coordinate process",
                static_cast<unsigned>(slot), p.seq);
        return;
    }
    // The insta-catch runs now, as the sender's own call ran: its verdict and consequences inside this call.
    g_run.pending = true;
    const bool called = DP::CallInstaCatch(desk);
    if (called) ++g_counts.instas;
    if (g_run.pending) {
        g_run.pending = false;
        Send(&s, slot, PV::kOpRefused, p.seq);
        UE_LOGW("desk_ping: HOST's insta-catch for slot %u's verdict #%u %s", static_cast<unsigned>(slot), p.seq,
                called ? "did not reach its verdict" : "did not dispatch");
    }
}

// ---- the watches ----------------------------------------------------------------------------------------------------

// Each registered once and named by one pointer, since the gate knows a class-scoped watch by the literals it was
// registered with. One the gate refuses, or one that settles dead, is said once with what its loss costs; the others
// stand. Without the verdict's, a client's pings roll on its own desk, unrelayed, and a host refuses verdicts; without
// the coordinate process's, a host refuses pings; without the log line's, the sound's or the insta-catch's, a
// client's refused verdict shows its own failure line or sound beside the host's answer.
UE_FACT_FN(kRowGatherSignal, "desk_ping_sync", L"spaceRenderer_C", L"gatherSignal", Declared);
UE_FACT_FN(kRowProcessCoords, "desk_ping_sync", L"analogDScreenTest_C", L"process_coords", Declared);
UE_FACT_FN(kRowWriteToCoordLog2, "desk_ping_sync", L"analogDScreenTest_C", L"writeToCoordLog_2", Declared);
UE_FACT_FN(kRowPlayPingSound, "desk_ping_sync", L"analogDScreenTest_C", L"playPingSound", Declared);
UE_FACT_FN(kRowIsntaCatchSignal, "desk_ping_sync", L"analogDScreenTest_C", L"isntaCatchSignal", Declared);

struct PingWatch {
    const ue_wrap::facts::Function* row;
    int            tag;
    sg::PreFn      pre;
    sg::PostFn     post;
    const char*    loss;
    bool           registered;
    bool           dead;
};
PingWatch g_watches[kWatchCount] = {
    {&kRowGatherSignal, 0x44504730, &OnVerdictPre, &OnVerdictPost,
     "a client's pings roll on its own desk, and a host refuses clients' verdicts", false, false},  // 'DPG0'
    {&kRowProcessCoords, 0x44504731, &OnProcessPre, &OnProcessPost,
     "a host refuses clients' pings, and a client's shows its own failure beside the host's", false, false},  // 'DPG1'
    {&kRowWriteToCoordLog2, 0x44504732, &OnLogLinePre, nullptr,
     "a client's refused verdict shows its own failure line", false, false},  // 'DPG2'
    {&kRowPlayPingSound, 0x44504733, &OnPingSoundPre, nullptr,
     "a client's refused verdict plays its own failure sound, a host its primed stage cue", false, false},  // 'DPG3'
    {&kRowIsntaCatchSignal, 0x44504734, &OnInstaPre, nullptr,
     "a client's insta-catch crosses as a ping's verdict and is refused", false, false},  // 'DPG4'
};
bool g_settled = false;  // every watch live or dead: the attempts end

void Register() {
    if (g_settled) return;
    for (PingWatch& w : g_watches) {
        if (w.registered || w.dead) continue;
        w.registered = sg::WatchName(*w.row, w.tag, w.pre, w.post);
        if (!w.registered) {
            w.dead = true;
            UE_LOGE("desk_ping: the gate took no watch on %ls::%ls -- %s", w.row->Class(), w.row->Name(), w.loss);
        }
    }
    sg::ResolvePendingNames();
    bool settled = true;
    for (int i = 0; i < kWatchCount; ++i) {
        PingWatch& w = g_watches[i];
        if (w.dead || g_live[i]) continue;
        if (sg::NameWatchLive(*w.row, w.tag)) {
            g_live[i] = true;
        } else if (sg::NameWatchSettled(*w.row, w.tag)) {
            w.dead = true;
            UE_LOGE("desk_ping: the watch on %ls::%ls settled dead -- %s", w.row->Class(), w.row->Name(), w.loss);
        } else {
            settled = false;
        }
    }
    if (!settled) return;
    g_settled = true;
    UE_LOGI("desk_ping: the ping lane's watches settled (verdict %d, coordinate process %d, log %d, sound %d, "
            "insta-catch %d live)", g_live[kWatchVerdict], g_live[kWatchProcess], g_live[kWatchLogLine],
            g_live[kWatchSound], g_live[kWatchInsta]);
}

}  // namespace

void Install(coop::net::Session* session) { g_session.store(session, std::memory_order_release); }

void Tick() {
    Register();
    auto* s = SessionAs(Role::Client);
    void* desk = CD::Instance();
    bool pinging = false;
    const bool reading = s && desk && DP::ReadPinging(desk, pinging);
    const uint64_t now = NowMs();
    if (reading && pinging) {
        if (!g_publishingProgress) {
            g_publishingProgress = true;
            if (++g_progressSeq == 0) ++g_progressSeq;
            g_nextProgressMs = 0;
        }
        if (now >= g_nextProgressMs) {
            DP::Progress state;
            if (DP::ReadProgress(desk, state)) {
                coop::net::DeskPingVerdictPayload p{};
                p.op = PV::kOpProgress;
                p.reason = 1;
                p.seq = g_progressSeq;
                p.progressStage = state.stage;
                p.progressOuter = state.outer;
                p.progressInner = state.inner;
                p.progressPing = state.ping;
                s->SendReliableToSlot(0, coop::net::ReliableKind::DeskPingVerdict, &p,
                                      static_cast<int>(sizeof(p)));
                g_nextProgressMs = now + kProgressIntervalMs;
            }
        }
    } else if (g_publishingProgress) {
        // A reliable terminal edge prevents the last progress bar from remaining frozen on observers.
        if (s) {
            coop::net::DeskPingVerdictPayload p{};
            p.op = PV::kOpProgress;
            p.seq = g_progressSeq;
            s->SendReliableToSlot(0, coop::net::ReliableKind::DeskPingVerdict, &p,
                                  static_cast<int>(sizeof(p)));
        }
        g_publishingProgress = false;
        g_nextProgressMs = 0;
    }
}

void OnMessage(coop::net::Session& session, const coop::net::DeskPingVerdictPayload& p, int senderSlot) {
    if (session.role() == Role::Host) {
        if (senderSlot > 0 && senderSlot < static_cast<int>(coop::net::kMaxPeers) && p.op == PV::kOpProgress) {
            const uint8_t slot = static_cast<uint8_t>(senderSlot);
            const bool active = p.reason == 1;
            const bool ownsRun = coop::device_occupancy::HolderOf(L"desk") == slot &&
                                 coop::desk_input_sync::PingActiveSlot() == slot;
            const bool valid = !active || (ownsRun && p.progressStage >= 0 && p.progressStage <= 3 &&
                std::isfinite(p.progressOuter) && std::isfinite(p.progressInner) && std::isfinite(p.progressPing) &&
                p.progressOuter >= 0.f && p.progressOuter <= 1.f && p.progressInner >= 0.f && p.progressInner <= 1.f &&
                p.progressPing >= 0.f && p.progressPing <= 1.f);
            if (valid && (active ? ownsRun : (g_mirrorSlot == senderSlot && g_mirrorSeq == p.seq))) {
                g_mirrorSlot = active ? senderSlot : -1;
                g_mirrorSeq = p.seq;
                g_mirrorActive = active;
                if (active) g_mirrorProgress = {p.progressStage, p.progressOuter, p.progressInner, p.progressPing};
                else DP::WriteProgress(CD::Instance(), DP::Progress{});
            }
        } else if ((p.op == PV::kOpIntent || p.op == PV::kOpInsta) && senderSlot > 0 &&
                   senderSlot < static_cast<int>(coop::net::kMaxPeers)) {
            OnIntent(session, p, static_cast<uint8_t>(senderSlot));
        }
        return;
    }
    if (p.op == PV::kOpProgress && senderSlot > 0 && senderSlot < static_cast<int>(coop::net::kMaxPeers)) {
        const bool active = p.reason == 1;
        if (active && (p.progressStage < 0 || p.progressStage > 3 || !std::isfinite(p.progressOuter) ||
            !std::isfinite(p.progressInner) || !std::isfinite(p.progressPing))) return;
        if (!active && (g_mirrorSlot != senderSlot || g_mirrorSeq != p.seq)) return;
        if (active && g_mirrorSlot != -1 && g_mirrorSlot != senderSlot) return;
        g_mirrorSlot = active ? senderSlot : -1;
        g_mirrorSeq = p.seq;
        g_mirrorActive = active;
        if (active) g_mirrorProgress = {p.progressStage, p.progressOuter, p.progressInner, p.progressPing};
        else DP::WriteProgress(CD::Instance(), DP::Progress{});
        return;
    }
    if (senderSlot != 0) return;
    if (p.op == PV::kOpRefused) {
        ++g_counts.answered;
        CD::AppendCoordLog(p.reason == PV::kRefusedBusy ? L"<r>Ping failed, the desk was already pinging</>"
                                                        : L"<r>Ping failed, the host could not take the ping</>");
        DP::PlayPingSound(CD::Instance(), DP::kPingFailedSound);
        UE_LOGI("desk_ping: CLIENT's verdict #%u was refused by the host -- shown as a failed ping", p.seq);
    } else if (p.op == PV::kOpFound) {
        ++g_counts.finds;
        ue_wrap::profile::AddSignalsFound(1);
        UE_LOGI("desk_ping: CLIENT's verdict #%u caught a signal -- the find is on this profile", p.seq);
    }
}

void OnPeerLeft(uint8_t slot) {
    if (slot >= coop::net::kMaxPeers) return;
    g_budget[slot].Reset();
    g_instaBudget[slot].Reset();
    g_nextSayMs[slot] = 0;
    g_nextRefuseSayMs[slot] = 0;
    g_refusedQuiet[slot] = 0;
    // A verdict waiting for the desk goes with its pinger; one already primed rolled in its body, and the run that
    // follows is the input lane's to attribute (desk_input_sync::OnPeerLeft).
    if (g_run.queued && g_run.slot == slot) {
        UE_LOGI("desk_ping: HOST dropped slot %u's queued verdict #%u -- the pinger left", static_cast<unsigned>(slot),
                g_run.seq);
        g_run = HostRun{};
    }
    if (g_mirrorSlot == slot) {
        g_mirrorSlot = -1;
        g_mirrorActive = false;
        DP::WriteProgress(CD::Instance(), DP::Progress{});
    }
}

void OnDisconnect() {
    g_verdictRefused = false;
    g_refusedFn = nullptr;
    g_run = HostRun{};
    g_publishingProgress = false;
    g_nextProgressMs = 0;
    g_mirrorSlot = -1;
    g_mirrorSeq = 0;
    g_mirrorActive = false;
    DP::WriteProgress(CD::Instance(), DP::Progress{});
    for (uint8_t slot = 0; slot < coop::net::kMaxPeers; ++slot) {
        g_budget[slot].Reset();
        g_instaBudget[slot].Reset();
        g_nextSayMs[slot] = 0;
        g_nextRefuseSayMs[slot] = 0;
        g_refusedQuiet[slot] = 0;
    }
}

Counts CountsNow() { return g_counts; }

}  // namespace coop::desk_ping_sync
