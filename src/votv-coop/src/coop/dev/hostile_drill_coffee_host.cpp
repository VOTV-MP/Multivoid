// coop/dev/hostile_drill_coffee_host.cpp -- the coffee family's judge: see coop/dev/hostile_drill_coffee.h. The host
// spawns the prop the arm needs before the client's puppet, says it is ready in the lobby chat once the prop is
// announced and its key resolves, and then judges by accounting: every message is exactly one outcome or one
// dispatcher drop, so the counter deltas since the baseline sum to the messages sent and each matches the arm's table.

#include "hostile_drill_coffee_detail.h"  // co-located private header (src tree, not include/)

#include "hostile_drill_act_detail.h"  // SpawnOnFloor (co-located private header of the act family)

#include "coop/dev/drill_chat.h"
#include "coop/interactables/coffee_intent.h"
#include "coop/interactables/coffee_state.h"
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
#include "ue_wrap/devices/coffee.h"

#include <chrono>
#include <cstdint>
#include <string>

namespace coop::dev::hostile_drill::coffee::detail {
namespace {

namespace DC = coop::dev::drill_chat;
namespace R  = ue_wrap::reflection;
namespace CF = ue_wrap::coffee;
namespace A  = coop::dev::hostile_drill::act::detail;

enum class HStep { Begin, WaitReady, Judge, Done };
HStep                 g_step = HStep::Begin;
Tally                 g_base{};
Clock::time_point     g_armAt{}, g_judgeAt{};
bool                  g_spawned = false, g_clientReady = false;
const char*           g_waiting = "slot 1's world-ready";
uint16_t              g_playerNo = 0;    // slot 1's occupant when the host said ready
ue_wrap::CachedObjRef g_prop;
CF::MakerFlags        g_flagsAtReady;    // the host's maker as it stood when the host said ready

void Finish(const Spec& sp, const char* what, const std::string& why) {
    Verdict(sp.arm, what, why);
    g_step = HStep::Done;
}

Tally Now() { return TallyOf(coop::coffee_intent::ReadForDrill(), coop::coffee_state::ReadForDrill()); }

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

// The arm's prop, near the client's puppet (inside the lane's reach) or 2000 uu out (outside it). True once it
// stands; an arm that cannot place it ends INCONCLUSIVE.
bool SpawnProp(const Spec& sp) {
    coop::RemotePlayer& rp = coop::puppet_drive::Puppet(kSlot);
    void* spawned = nullptr;
    const char* why = nullptr;
    if (sp.fixture == Fixture::NearCrowbar) {
        why = A::SpawnOnFloor(rp, A::kCrowbarPath, false, 0, nullptr, &spawned);
    } else {
        void* const cls = R::FindClass(kMakerCls);
        if (!cls) {
            Finish(sp, "INCONCLUSIVE", "the coffee maker class does not resolve");
            return false;
        }
        const bool farOut = sp.fixture == Fixture::FarMaker;
        why = A::SpawnClassOnFloor(rp, cls, farOut ? kFarCm : kNearCm, farOut, 0, nullptr, &spawned);
    }
    if (why) {
        Finish(sp, "INCONCLUSIVE", std::string("the prop: ") + why);
        return false;
    }
    g_prop.Set(spawned);
    return true;
}

// The prop holds a key the key index resolves to it: the lane's lookup of the press's key will find it.
bool PropResolves(const Spec& sp) {
    void* const prop = g_prop.Get();
    if (!prop) {
        Finish(sp, "INCONCLUSIVE", "the spawned prop was gone before it was announced");
        return false;
    }
    const std::wstring key = ue_wrap::prop::GetKeyString(prop);
    if (key.empty() || key == L"None") { g_waiting = "the prop's key"; return false; }
    void* found = nullptr;
    if (coop::prop_key_index::Resolve(key, &found) != coop::prop_key_index::Read::Hit || found != prop) {
        g_waiting = "the key index to hold the prop";
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
            // The client records the props of the arm's class it already holds, then says so: one born earlier
            // would sit in that set.
            if (!g_clientReady) { g_waiting = "the client's ready line"; return; }
            if (!SpawnProp(sp)) return;
            g_spawned = true;
        }
        if (!PropResolves(sp)) return;
    } else if (!g_clientReady) {
        g_waiting = "the client's ready line";
        return;
    }
    // The client kills its player once it has found the prop; ready is said inside that death.
    if (sp.mode == Mode::Dead && coop::players::Registry::Get().LifeOf(kSlot) != coop::players::PuppetLife::Dead) {
        g_waiting = "slot 1's puppet to read dead";
        return;
    }
    if (sp.arm == Arm::ForgedRecord && !CF::ReadMakerFlags(g_prop.Get(), g_flagsAtReady)) {
        Finish(sp, "INCONCLUSIVE", "the host's maker flags did not read");
        return;
    }
    g_base = Now();
    g_playerNo = coop::roster_ledger::Get(kSlot).playerNo;
    DC::Say(ReadyLine(sp.arm));
    g_judgeAt = Clock::now();
    g_step = HStep::Judge;
    UE_LOGI("[HOSTILE-DRILL] %s armed on slot %u (%s)", sp.name, static_cast<unsigned>(kSlot),
            sp.fixture == Fixture::None ? "no prop needed" : "the prop stands and its key resolves");
}

// ---- the judge ----

// The counters' movement since the baseline, never below 0.
Tally Diff(const Tally& now) {
    Tally d{};
    for (size_t i = 0; i < kFields; ++i) d[i] = now[i] >= g_base[i] ? now[i] - g_base[i] : 0;
    return d;
}

// Every message ends as a run, a refusal, a refused record or a dispatcher drop: the claims' endings plus the drops.
uint64_t Outcomes(const Tally& d) {
    uint64_t n = 0;
    for (const Field f : {Field::Ran, Field::NoDevice, Field::WrongClass, Field::OutOfReach, Field::NotRun,
                          Field::QueueFull, Field::SenderDead, Field::DropSize, Field::DropKey,
                          Field::RecordsRefused})
        n += d[static_cast<size_t>(f)];
    return n;
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

void Judge(coop::net::Session* s, const Spec& sp) {
    const Tally d = Diff(Now());
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
    if (sp.arm == Arm::ForgedRecord) {
        // The refusal is the gate's; the proof it held is the host's maker, as it stood when the host said ready.
        CF::MakerFlags now;
        if (!CF::ReadMakerFlags(g_prop.Get(), now))
            return Finish(sp, "INCONCLUSIVE", "the host's maker flags did not read at the verdict");
        if (!(now == g_flagsAtReady))
            return Finish(sp, "FAIL", "the record was refused but the host's maker changed (hasCup=" +
                                          std::to_string(now.hasCup) + ")");
    }
    Finish(sp, "PASS", "all " + std::to_string(sp.sent) + " messages accounted, as the arm's table: " + Summary(d));
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
    g_base = Tally{};
    g_armAt = g_judgeAt = {};
    g_spawned = g_clientReady = false;
    g_waiting = "slot 1's world-ready";
    g_playerNo = 0;
    g_prop.Reset();
    g_flagsAtReady = CF::MakerFlags{};
}

}  // namespace coop::dev::hostile_drill::coffee::detail
