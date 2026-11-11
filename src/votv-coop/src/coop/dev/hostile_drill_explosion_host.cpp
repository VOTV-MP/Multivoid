// coop/dev/hostile_drill_explosion_host.cpp -- the explosion family's judge: see coop/dev/hostile_drill_explosion.h.
// The host prepares its half (the guard, or the story arm's writes), says it is ready in the lobby chat once it holds a
// posed puppet and the hand state the arm needs for slot 1, and then judges by accounting: every message is exactly one
// accept or one refusal, so the counter deltas since the baseline sum to the messages sent and each reason matches
// the arm's table.

#include "hostile_drill_explosion_detail.h"  // co-located private header (src tree, not include/)

#include "explosion_drill_source.h"  // Guard, Unguard, ReadBool, WriteBool, LegOf (co-located private header)

#include "coop/net/net_clock.h"
#include "coop/net/session.h"
#include "coop/player/hand_history.h"
#include "coop/player/hand_item.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"
#include "coop/player/roster_ledger.h"
#include "coop/props/prop_snapshot.h"
#include "coop/save/join_window_baseline.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/world/game_mode.h"
#include "ue_wrap/world/world_singleton.h"

#include <chrono>
#include <cstdint>
#include <string>

namespace coop::dev::hostile_drill::explosion::detail {
namespace {

namespace XD = coop::dev::explosion_drill::detail;
namespace ER = coop::explosion_request;
namespace R  = ue_wrap::reflection;
namespace GM = ue_wrap::game_mode;
using Refusal = ER::Refusal;
constexpr size_t kReasons = static_cast<size_t>(Refusal::Count);
// The table's cap of judged shots: a shot past it evicts the oldest.
constexpr uint32_t kShotsMax = 64;
// A hand that "held nothing this session" is read over this window, past the longest any judge asks (a grenade's 5 s).
constexpr uint64_t kNoneHeldWindowMs = 10000;

enum class HStep { Begin, WaitReady, Judge, Done };
HStep             g_step = HStep::Begin;
ER::Counters      g_base{};
Clock::time_point g_armAt{}, g_judgeAt{};
bool              g_prepared = false;
const char*       g_waiting = "the slot's world-ready";
uint16_t          g_playerNo = 0;    // slot 1's occupant when the host said ready
// The rejoin arm's phase: life 1's shot to take, the wait for the returning occupant, and life 2's request to count.
enum class Rejoin { One, Between, Two };
Rejoin            g_rejoin = Rejoin::One;
// The lane's counters are cleared with the host's last client leaving (explosion_request::OnDisconnect), so life 1's
// accepted shot is gone from them when life 2 is judged: the baseline is retaken then, and life 2's table is its own
// one request (refused as an unknown shot), which is what proves the first life's shot id did not outlive it.
bool              g_rebased = false;

// ---- the story arm's writes: the host's mode and the gamemode's flags the guns' hold gate reads ----
struct Story {
    bool written = false;
    int  mode = -1, christmas = -1, halloween = -1, unlock = -1;
};
Story g_story;

bool ReadFlag(void* gm, const wchar_t* name, int& out) {
    bool b = false;
    if (!XD::ReadBool(gm, name, b)) return false;
    out = b ? 1 : 0;
    return true;
}

// Writes Story (0) and the three flags false, keeping what they held; false while the gamemode does not resolve.
bool WriteStory() {
    void* gi = ue_wrap::world_singleton::GameInstance();
    void* gm = ue_wrap::world_singleton::Gamemode();
    Story s;
    if (!gi || !gm || !R::IsLive(gm)) return false;
    s.mode = GM::ReadFrom(gi);
    if (s.mode < 0 || !ReadFlag(gm, L"isChristmas", s.christmas) || !ReadFlag(gm, L"isHalloween", s.halloween) ||
        !ReadFlag(gm, L"unlockFungunsInStory", s.unlock))
        return false;
    GM::WriteTo(gi, GM::kStory);
    XD::WriteBool(gm, L"isChristmas", false);
    XD::WriteBool(gm, L"isHalloween", false);
    XD::WriteBool(gm, L"unlockFungunsInStory", false);
    s.written = true;
    g_story = s;
    return true;
}

void RestoreStory() {
    if (!g_story.written) return;
    void* gi = ue_wrap::world_singleton::GameInstance();
    void* gm = ue_wrap::world_singleton::Gamemode();
    if (gi) GM::WriteTo(gi, g_story.mode);
    if (gm && R::IsLive(gm)) {
        XD::WriteBool(gm, L"isChristmas", g_story.christmas != 0);
        XD::WriteBool(gm, L"isHalloween", g_story.halloween != 0);
        XD::WriteBool(gm, L"unlockFungunsInStory", g_story.unlock != 0);
    }
    UE_LOGI("[HOSTILE-DRILL] explosion_story put back the host's mode and gamemode flags");
    g_story = Story{};
}

void Finish(const Spec& sp, const char* what, const std::string& why) {
    Verdict(sp.arm, what, why);
    XD::Unguard();
    RestoreStory();
    g_step = HStep::Done;
}

// ---- the host's half before the ready line ----

// The guard (or the story writes) the arm needs; false while it is still not in, or once the arm ended.
bool Prepare(const Spec& sp) {
    if (g_prepared) return true;
    XD::g_src.host = true;   // this process's role, for the guard's own log lines
    if (sp.mode == Mode::Story) {
        if (!WriteStory()) { g_waiting = "the gamemode to resolve"; return false; }
        if (GM::FungunHoldAllowed()) {
            Finish(sp, "INCONCLUSIVE", "the host still allows the guns after its mode and flags were written");
            return false;
        }
    } else if (XD::Guard(!sp.hostPlayer)) {
        g_waiting = "the guard (unlockFungunsInStory) to resolve";
        return false;
    }
    g_prepared = true;
    return true;
}

// The state of slot 1's hand the arm needs: the item named held, or an empty hand that held no launcher or explosive
// in the window the judges read.
bool HandReady(const Spec& sp) {
    if (sp.hand != Hand::Empty) return coop::hand_item::HeldClassIs(kSlot, HandClass(sp.hand));
    const uint64_t now = coop::net::NowMs();
    return !coop::hand_item::HoldsItem(kSlot) &&
           !coop::hand_history::HeldWithin(kSlot, HandClass(Hand::Rpg), kNoneHeldWindowMs, now) &&
           !coop::hand_history::HeldWithin(kSlot, HandClass(Hand::Sticky), kNoneHeldWindowMs, now) &&
           !coop::hand_history::HeldWithin(kSlot, HandClass(Hand::Grenade), kNoneHeldWindowMs, now);
}

// Slot 1 is world-ready with its bracket closed and a posed puppet: the host can place the sender.
bool PuppetReady(coop::net::Session* s) {
    if (!s->IsSlotWorldReady(kSlot)) { g_waiting = "slot 1 to be world-ready"; return false; }
    if (!coop::prop_snapshot::IsBracketClosed(kSlot) || coop::join_window_baseline::IsLateWindowOpen(kSlot)) {
        g_waiting = "slot 1's join window to close";
        return false;
    }
    coop::RemotePlayer& rp = coop::puppet_drive::Puppet(kSlot);
    if (!rp.valid() || !rp.HasPose()) { g_waiting = "a posed puppet for slot 1"; return false; }
    return true;
}

void WaitReady(coop::net::Session* s, const Spec& sp) {
    if (Clock::now() - g_armAt > kFailBound) {
        Finish(sp, "INCONCLUSIVE", std::string("the precondition never held: waiting for ") + g_waiting);
        return;
    }
    if (!PuppetReady(s) || !Prepare(sp)) return;
    if (!HandReady(sp)) { g_waiting = "slot 1's hand state"; return; }
    g_playerNo = coop::roster_ledger::Get(kSlot).playerNo;
    Say(ReadyLine(sp.arm, 1));
    g_judgeAt = Clock::now();
    g_step = HStep::Judge;
    UE_LOGI("[HOSTILE-DRILL] %s armed on slot %u (a posed puppet, the hand state in)", sp.name,
            static_cast<unsigned>(kSlot));
}

// ---- the judge ----

struct Delta {
    uint64_t shots = 0, req[3] = {}, refused[kReasons] = {}, evicted = 0, sum = 0;
};

Delta Diff(const ER::Counters& now) {
    Delta d;
    d.shots = now.shotsAccepted - g_base.shotsAccepted;
    d.evicted = now.evicted - g_base.evicted;
    d.sum = d.shots;
    for (int k = 0; k < 3; ++k) {
        d.req[k] = now.requestsAccepted[k] - g_base.requestsAccepted[k];
        d.sum += d.req[k];
    }
    for (size_t r = 0; r < kReasons; ++r) {
        d.refused[r] = now.refused[r] - g_base.refused[r];
        d.sum += d.refused[r];
    }
    return d;
}

std::string Summary(const Delta& d) {
    std::string out = "shots " + std::to_string(d.shots) + ", requests " + std::to_string(d.req[0]) + "/" +
                      std::to_string(d.req[1]) + "/" + std::to_string(d.req[2]) + ", evicted " +
                      std::to_string(d.evicted) + ", refused";
    bool any = false;
    for (size_t r = 0; r < kReasons; ++r) {
        if (!d.refused[r]) continue;
        out += std::string(" ") + ER::RefusalName(static_cast<Refusal>(r)) + "=" + std::to_string(d.refused[r]);
        any = true;
    }
    return any ? out : out + " none";
}

// What does not match the arm's table, or empty. The flood's table is its own shape: accepted and queue_full add to
// the messages sent, queue_full is at least 1, and the shots past the table's cap evicted the oldest.
std::string Mismatch(const Spec& sp, const Delta& d) {
    const size_t want = static_cast<size_t>(sp.refusal);
    const bool flood = sp.mode == Mode::Flood;
    std::string bad;
    for (size_t r = 0; r < kReasons; ++r) {
        const uint64_t w = r == want && !flood ? sp.refused : 0;
        if (flood && r == want) {
            if (d.refused[r] < 1) bad += " queue_full is 0 (the cap never refused);";
        } else if (d.refused[r] != w) {
            bad += std::string(" ") + ER::RefusalName(static_cast<Refusal>(r)) + " " + std::to_string(d.refused[r]) +
                   " (wanted " + std::to_string(w) + ");";
        }
    }
    for (int k = 0; k < 3; ++k)
        if (d.req[k] != sp.requests[k]) bad += " requestsAccepted[" + std::to_string(k) + "] " + std::to_string(d.req[k]) + " (wanted " + std::to_string(sp.requests[k]) + ");";
    if (flood) {
        const uint64_t wantEvicted = d.shots > kShotsMax ? d.shots - kShotsMax : 0;
        if (d.evicted != wantEvicted) bad += " evicted " + std::to_string(d.evicted) + " (wanted " + std::to_string(wantEvicted) + ");";
    } else {
        if (d.shots != sp.shots) bad += " shots " + std::to_string(d.shots) + " (wanted " + std::to_string(sp.shots) + ");";
        if (d.evicted != sp.evicted) bad += " evicted " + std::to_string(d.evicted) + " (wanted " + std::to_string(sp.evicted) + ");";
    }
    return bad;
}

// The rejoin arm's host, in three phases: life 1's shot is accepted and said in chat; slot 1 takes a new occupant with
// a posed puppet and the host says it is ready for life 2; then the second life's request is counted with the first
// life's shot, which the occupant's change must have cleared. True while the arm is still between those.
bool RejoinPhases(coop::net::Session* s, const Spec& sp, const Delta& d) {
    if (g_rejoin == Rejoin::One) {
        if (d.sum == 0) {
            if (Clock::now() - g_judgeAt > kFailBound) Finish(sp, "FAIL", "the first life's shot never reached the host");
            return true;
        }
        if (d.sum != 1 || d.shots != 1) {
            Finish(sp, "FAIL", "the first life's shot was not accepted once: " + Summary(d));
            return true;
        }
        Say(TakenLine(sp.arm) + " slot=" + std::to_string(kSlot));
        g_rejoin = Rejoin::Between;
        g_judgeAt = Clock::now();
        UE_LOGI("[HOSTILE-DRILL] %s life-1 accepted its shot on slot %u", sp.name, static_cast<unsigned>(kSlot));
        return true;
    }
    if (g_rejoin == Rejoin::Between) {
        const uint16_t now = coop::roster_ledger::Get(kSlot).playerNo;
        if (now == 0 || now == g_playerNo) {
            if (Clock::now() - g_judgeAt > kFailBound) Finish(sp, "INCONCLUSIVE", "slot 1 never took a new occupant");
            return true;
        }
        if (!PuppetReady(s)) {
            if (Clock::now() - g_judgeAt > kFailBound)
                Finish(sp, "INCONCLUSIVE", std::string("the returning client: waiting for ") + g_waiting);
            return true;
        }
        Say(ReadyLine(sp.arm, 2));
        g_rejoin = Rejoin::Two;
        g_base = ER::GetCounters();
        g_rebased = true;
        g_judgeAt = Clock::now();
        UE_LOGI("[HOSTILE-DRILL] %s armed on the returning client in slot %u", sp.name, static_cast<unsigned>(kSlot));
        return true;
    }
    return false;
}

void Judge(coop::net::Session* s, const Spec& spec) {
    const Delta d = Diff(ER::GetCounters());
    const bool rejoin = spec.mode == Mode::Rejoin;
    if (rejoin && RejoinPhases(s, spec, d)) return;
    Spec sp = spec;
    if (rejoin && g_rebased) { sp.sent = 1; sp.shots = 0; }   // life 2 alone (see g_rebased)
    // The per-slot state resets when the slot's occupant changes, so a replaced occupant is not judged on it (the
    // rejoin arm waits for exactly that change).
    if (!rejoin && sp.mode != Mode::NotReady) {
        if (!s->HasPeerConn(kSlot)) return Finish(sp, "INCONCLUSIVE", "the judged slot left before the verdict");
        if (coop::roster_ledger::Get(kSlot).playerNo != g_playerNo)
            return Finish(sp, "INCONCLUSIVE", "slot-replaced (the judged slot's occupant changed)");
    }
    if (d.sum > sp.sent) return Finish(sp, "FAIL", "more outcomes than messages (" + std::to_string(sp.sent) + " sent): " + Summary(d));
    if (d.sum < sp.sent) {
        if (Clock::now() - g_judgeAt <= kFailBound) return;
        if (sp.mode == Mode::NotReady) return Finish(sp, "FAIL", "the shot sent before the announce was never refused: " + Summary(d));
        return Finish(sp, "FAIL", "timeout, " + std::to_string(d.sum) + " of " + std::to_string(sp.sent) + " accounted: " + Summary(d));
    }
    const std::string bad = Mismatch(sp, d);
    if (bad.empty()) Finish(sp, "PASS", "all " + std::to_string(sp.sent) + " messages accounted, as the arm's table: " + Summary(d));
    else Finish(sp, "FAIL", "the deltas differ from the arm's table:" + bad + " got " + Summary(d));
}

}  // namespace

void HostTick(coop::net::Session* s, const Spec& sp) {
    if (g_step == HStep::Done) return;
    if (g_step == HStep::Begin) {
        // The baseline is the session's start: every counter reads from here, the not_ready arm's included, which has
        // no ready line and no slot yet.
        g_base = ER::GetCounters();
        g_armAt = Clock::now();
        if (sp.mode == Mode::NotReady) {
            g_judgeAt = g_armAt;
            g_step = HStep::Judge;
            UE_LOGI("[HOSTILE-DRILL] %s armed at the session's start (the client's announce is held)", sp.name);
        } else {
            g_step = HStep::WaitReady;
        }
    }
    if (g_step == HStep::WaitReady) WaitReady(s, sp);
    else if (g_step == HStep::Judge) Judge(s, sp);
}

void HostReset() {
    XD::Unguard();
    RestoreStory();
    // The rejoin arm spans the client's death: its first life's shot was accepted and the counters, the baseline and
    // the first occupant's number are what the second life is judged against. The host's last client leaving is
    // exactly the disconnect that runs this reset, so a reset that took them would put the arm back at life 1 and
    // the returning client would send life 1's shot again (measured: never a verdict). They stand until the second
    // life is judged.
    if (g_rejoin != Rejoin::One) return;
    g_step = HStep::Begin;
    g_base = ER::Counters{};
    g_armAt = g_judgeAt = {};
    g_prepared = false;
    g_waiting = "the slot's world-ready";
    g_playerNo = 0;
    g_rejoin = Rejoin::One;
    g_rebased = false;
}

}  // namespace coop::dev::hostile_drill::explosion::detail
