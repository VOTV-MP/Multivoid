// coop/dev/hostile_drill_cord_host.cpp -- the cord family's judge: see coop/dev/hostile_drill_cord.h. The host spawns
// the corded prop the arm needs before the client's puppet, says it is ready in the lobby chat once the prop is
// announced and its key resolves, and then judges by accounting: every message is exactly one claim outcome or one
// dispatcher drop, so the counter deltas since the baseline sum to the messages sent and each matches the arm's table.
// An arm whose claims are answered also waits for the client's count of the answers it saw.

#include "hostile_drill_cord_detail.h"  // co-located private header (src tree, not include/)

#include "hostile_drill_act_detail.h"  // SpawnClassOnFloor (co-located private header of the act family)

#include "coop/dev/drill_chat.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"
#include "coop/player/roster_ledger.h"
#include "coop/props/prop_key_index.h"
#include "coop/props/prop_snapshot.h"
#include "coop/save/join_window_baseline.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace coop::dev::hostile_drill::cord::detail {
namespace {

namespace DC = coop::dev::drill_chat;
namespace R  = ue_wrap::reflection;
namespace CS = coop::cord_sync;

enum class HStep { Begin, WaitReady, Judge, Done };
HStep                 g_step = HStep::Begin;
CS::DrillView         g_base{};
Clock::time_point     g_armAt{}, g_judgeAt{};
bool                  g_spawned = false, g_clientReady = false, g_saw = false;
std::string           g_sawRest;
const char*           g_waiting = "slot 1's world-ready";
uint16_t              g_playerNo = 0;    // slot 1's occupant when the host said ready
ue_wrap::CachedObjRef g_maker;

void Finish(const Spec& sp, const char* what, const std::string& why) {
    Verdict(sp.arm, what, why);
    g_step = HStep::Done;
}

// ---- the host's half before the ready line ----

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

// The maker, near the client's puppet (inside the lane's reach) or 2000 uu out (outside it). True once it stands; an
// arm that cannot place it ends INCONCLUSIVE.
bool SpawnMaker(const Spec& sp) {
    namespace A = coop::dev::hostile_drill::act::detail;
    void* const cls = R::FindClass(kMakerCls);
    if (!cls) {
        Finish(sp, "INCONCLUSIVE", "the coffee maker class does not resolve");
        return false;
    }
    void* spawned = nullptr;
    const bool farOut = sp.fixture == Fixture::Far;
    if (const char* why = A::SpawnClassOnFloor(coop::puppet_drive::Puppet(kSlot), cls, farOut ? kFarCm : kNearCm, farOut, 0,
                                               nullptr, &spawned)) {
        Finish(sp, "INCONCLUSIVE", std::string("the maker: ") + why);
        return false;
    }
    g_maker.Set(spawned);
    return true;
}

// The maker holds a key the key index resolves to it: the lane's lookup of the claim's cord will find it.
bool MakerResolves(const Spec& sp) {
    void* const maker = g_maker.Get();
    if (!maker) {
        Finish(sp, "INCONCLUSIVE", "the spawned maker was gone before it was announced");
        return false;
    }
    const std::wstring key = ue_wrap::prop::GetKeyString(maker);
    if (key.empty() || key == L"None") { g_waiting = "the maker's key"; return false; }
    void* found = nullptr;
    if (coop::prop_key_index::Resolve(key, &found) != coop::prop_key_index::Read::Hit || found != maker) {
        g_waiting = "the key index to hold the maker";
        return false;
    }
    return true;
}

void WaitReady(coop::net::Session* s, const Spec& sp) {
    if (Clock::now() - g_armAt > kFailBound) {
        Finish(sp, "INCONCLUSIVE", std::string("the precondition never held: waiting for ") + g_waiting);
        return;
    }
    // The client's line, latched on sight from the arm's first tick: a live chat row retires after seconds.
    if (!g_clientReady && DC::PollDue()) g_clientReady = DC::ChatHasLive(ClientReadyLine(sp.arm));
    if (!PuppetReady(s)) return;
    if (sp.fixture != Fixture::None) {
        if (!g_spawned) {
            // The client records the coffee makers it already holds, then says so: a maker born earlier would sit
            // in that set.
            if (!g_clientReady) { g_waiting = "the client's ready line"; return; }
            if (!SpawnMaker(sp)) return;
            g_spawned = true;
        }
        if (!MakerResolves(sp)) return;
    } else if (!g_clientReady) {
        g_waiting = "the client's ready line";
        return;
    }
    // The client kills its player once it has found the maker; ready is said inside that death.
    if (sp.mode == Mode::Dead && coop::players::Registry::Get().LifeOf(kSlot) != coop::players::PuppetLife::Dead) {
        g_waiting = "slot 1's puppet to read dead";
        return;
    }
    g_base = CS::ReadForDrill();
    g_playerNo = coop::roster_ledger::Get(kSlot).playerNo;
    DC::Say(ReadyLine(sp.arm));
    g_judgeAt = Clock::now();
    g_step = HStep::Judge;
    UE_LOGI("[HOSTILE-DRILL] %s armed on slot %u (%s)", sp.name, static_cast<unsigned>(kSlot),
            sp.fixture == Fixture::None ? "no maker needed" : "the maker stands and its key resolves");
}

// ---- the judge ----

// The counters' movement since the baseline, never below 0.
Tally Diff(const CS::DrillView& now) {
    const Tally a = TallyOf(now), b = TallyOf(g_base);
    Tally d{};
    for (size_t i = 0; i < kFields; ++i) d[i] = a[i] >= b[i] ? a[i] - b[i] : 0;
    return d;
}

// Every message ends as a claim's outcome or a drop: the claims taken plus the dispatcher's drops.
uint64_t Outcomes(const Tally& d) {
    return static_cast<uint64_t>(d[static_cast<size_t>(Field::Claims)]) + d[static_cast<size_t>(Field::DropSize)] +
           d[static_cast<size_t>(Field::DropKey)] + d[static_cast<size_t>(Field::DropOp)] +
           d[static_cast<size_t>(Field::DropField)];
}

std::string Summary(const Tally& d) {
    std::string out;
    for (size_t i = 0; i < kFields; ++i)
        if (d[i]) out += (out.empty() ? "" : ", ") + std::string(FieldName(static_cast<Field>(i))) + "=" +
                         std::to_string(d[i]);
    return out.empty() ? "nothing counted" : out;
}

// What does not match the arm's table, or empty.
std::string Mismatch(const Spec& sp, const Tally& d) {
    const Tally want = Expected(sp);
    std::string bad;
    for (size_t i = 0; i < kFields; ++i)
        if (d[i] != want[i])
            bad += std::string(" ") + FieldName(static_cast<Field>(i)) + " " + std::to_string(d[i]) + " (wanted " +
                   std::to_string(want[i]) + ");";
    return bad;
}

// The client's count of the answers it saw, from its chat line; false until the line is live.
bool ClientSaw(const Spec& sp, uint32_t& n) {
    if (!g_saw && DC::PollDue()) g_saw = DC::LiveRestOf(SawPrefix(sp.arm), &g_sawRest);
    if (!g_saw) return false;
    n = static_cast<uint32_t>(std::strtoul(g_sawRest.c_str(), nullptr, 10));
    return true;
}

void Judge(coop::net::Session* s, const Spec& sp) {
    const Tally d = Diff(CS::ReadForDrill());
    if (!s->HasPeerConn(kSlot)) return Finish(sp, "INCONCLUSIVE", "the judged slot left before the verdict");
    if (coop::roster_ledger::Get(kSlot).playerNo != g_playerNo)
        return Finish(sp, "INCONCLUSIVE", "slot-replaced (the judged slot's occupant changed)");
    const uint64_t outcomes = Outcomes(d);
    if (outcomes > sp.sent)
        return Finish(sp, "FAIL", "more outcomes than messages (" + std::to_string(sp.sent) + " sent): " + Summary(d));
    if (outcomes < sp.sent) {
        if (Clock::now() - g_judgeAt <= kFailBound) return;
        return Finish(sp, "FAIL", "timeout, " + std::to_string(outcomes) + " of " + std::to_string(sp.sent) +
                                      " accounted: " + Summary(d));
    }
    const std::string bad = Mismatch(sp, d);
    if (!bad.empty()) return Finish(sp, "FAIL", "the counters differ from the arm's table:" + bad + " got " + Summary(d));
    const uint32_t answers = d[static_cast<size_t>(Field::Answers)];
    if (answers == 0) {
        Finish(sp, "PASS", "all " + std::to_string(sp.sent) + " messages accounted, as the arm's table: " + Summary(d));
        return;
    }
    // The host's answers are counted; the client's count of the ones it received closes the loop.
    uint32_t saw = 0;
    if (!ClientSaw(sp, saw)) {
        if (Clock::now() - g_judgeAt > kFailBound)
            Finish(sp, "FAIL", "the host answered " + std::to_string(answers) + " but the client never said what it saw");
        return;
    }
    if (saw != answers)
        return Finish(sp, "FAIL", "the host answered " + std::to_string(answers) + " and the client saw " +
                                      std::to_string(saw));
    Finish(sp, "PASS", "all " + std::to_string(sp.sent) + " messages accounted, as the arm's table, and the client saw " +
                           std::to_string(saw) + " answers: " + Summary(d));
}

}  // namespace

void HostTick(coop::net::Session* s, const Spec& sp) {
    if (g_step == HStep::Done) return;
    if (g_step == HStep::Begin) {
        g_armAt = Clock::now();
        g_step = HStep::WaitReady;
    }
    if (g_step == HStep::WaitReady) WaitReady(s, sp);
    else if (g_step == HStep::Judge) Judge(s, sp);
}

void HostReset() {
    g_step = HStep::Begin;
    g_base = CS::DrillView{};
    g_armAt = g_judgeAt = {};
    g_spawned = g_clientReady = g_saw = false;
    g_sawRest.clear();
    g_waiting = "slot 1's world-ready";
    g_playerNo = 0;
    g_maker.Reset();
}

}  // namespace coop::dev::hostile_drill::cord::detail
