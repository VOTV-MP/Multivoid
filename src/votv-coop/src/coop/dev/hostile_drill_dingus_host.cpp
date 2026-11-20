// coop/dev/hostile_drill_dingus_host.cpp -- the dingus_report arm's judge: see coop/dev/hostile_drill_dingus.h. The host spawns a
// plain prop and a dingus out of the client's reach, says the two keys the client sends in the lobby chat once each prop is
// announced and its key resolves, and judges by accounting at the client's done line: the report messages precede it on the
// ordered lane, so every one has ended in a drop or an outcome by then. The judge's budget refills with time, so reports
// judged farther apart than kSpanMs make the table's counts an INCONCLUSIVE, never a FAIL.

#include "hostile_drill_dingus_detail.h"  // co-located private header (src tree, not include/)

#include "hostile_drill_act_detail.h"  // SpawnOnFloor, SpawnClassOnFloor (co-located private header of the act family)

#include "coop/dev/drill_chat.h"
#include "coop/net/session.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"
#include "coop/player/roster_ledger.h"
#include "coop/props/dingus_sync.h"
#include "coop/props/prop_key_index.h"
#include "coop/props/prop_snapshot.h"
#include "coop/save/join_window_baseline.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>

namespace coop::dev::hostile_drill::dingus::detail {
namespace {

namespace DC = coop::dev::drill_chat;
namespace DS = coop::dingus_sync::dev;
namespace A  = coop::dev::hostile_drill::act::detail;

constexpr const wchar_t* kDingusCls = L"prop_dingus_C";
constexpr const wchar_t* kDingusPath = L"/Game/objects/prop_dingus.prop_dingus_C";

enum class HStep { Begin, Spawn, WaitKeys, Judge, Done };
HStep                 g_step = HStep::Begin;
Tally                 g_base{};
Clock::time_point     g_armAt{}, g_judgeAt{};
bool                  g_spawned = false, g_clientDone = false;
const char*           g_waiting = "slot 1's world-ready";
uint16_t              g_playerNo = 0;    // slot 1's occupant when the host said the keys
ue_wrap::CachedObjRef g_plain, g_far;

// The judge's budget (4 at once, 0.5 a second) stays inside the arm's table while the reports are judged this close together.
constexpr uint64_t kSpanMs = 1500;

void Finish(const Spec& sp, const char* what, const std::string& why) {
    Verdict(sp.arm, what, why);
    g_step = HStep::Done;
}

Tally Now() { return TallyOf(DS::Judge(), DS::FoundKeyCount()); }

// Slot 1 is world-ready with its bracket closed and a posed puppet: the host can place the fixtures round it.
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

// The plain prop (a crowbar, a keyed prop_C descendant) and a dingus out of the judge's reach of the puppet. True once they
// stand; an arm that cannot place one ends INCONCLUSIVE.
bool SpawnFixtures(const Spec& sp) {
    coop::RemotePlayer& rp = coop::puppet_drive::Puppet(kSlot);
    void* plain = nullptr;
    void* farDingus = nullptr;
    void* cls = ue_wrap::object_index::ClassByName(kDingusCls);
    if (!cls) cls = ue_wrap::asset_load::LoadObjectByPath(kDingusPath);
    if (!cls) {
        Finish(sp, "INCONCLUSIVE", "the dingus class does not resolve");
        return false;
    }
    const char* why = A::SpawnOnFloor(rp, A::kCrowbarPath, false, 0, nullptr, &plain);
    if (!why) why = A::SpawnClassOnFloor(rp, cls, kFarCm, true, 0, nullptr, &farDingus);
    if (why) {
        Finish(sp, "INCONCLUSIVE", std::string("a fixture: ") + why);
        return false;
    }
    g_plain.Set(plain);
    g_far.Set(farDingus);
    return true;
}

// The prop holds a key the key index resolves to it: the judge's lookup of a report's key will find it.
bool Resolves(const Spec& sp, ue_wrap::CachedObjRef& ref, std::wstring& key) {
    void* const prop = ref.Get();
    if (!prop) {
        Finish(sp, "INCONCLUSIVE", "a spawned fixture was gone before it was announced");
        return false;
    }
    key = ue_wrap::prop::GetKeyString(prop);
    if (key.empty() || key == L"None") { g_waiting = "a fixture's key"; return false; }
    void* found = nullptr;
    if (coop::prop_key_index::Resolve(key, &found) != coop::prop_key_index::Read::Hit || found != prop) {
        g_waiting = "the key index to hold the fixtures";
        return false;
    }
    return true;
}

std::string Narrow(const std::wstring& ascii) {
    std::string out;
    for (const wchar_t ch : ascii) out.push_back(static_cast<char>(ch));
    return out;
}

void Begin(coop::net::Session* s, const Spec& sp) {
    if (Clock::now() - g_armAt > kBound) {
        Finish(sp, "INCONCLUSIVE", std::string("the precondition never held: waiting for ") + g_waiting);
        return;
    }
    if (!PuppetReady(s)) return;
    if (!g_spawned) {
        if (!SpawnFixtures(sp)) return;
        g_spawned = true;
    }
    g_step = HStep::WaitKeys;
}

void WaitKeys(const Spec& sp) {
    if (Clock::now() - g_armAt > kBound) {
        Finish(sp, "INCONCLUSIVE", std::string("the precondition never held: waiting for ") + g_waiting);
        return;
    }
    std::wstring plainKey, farKey;
    if (!Resolves(sp, g_plain, plainKey) || !Resolves(sp, g_far, farKey)) return;
    g_base = Now();
    DS::ResetJudgeSpan();
    g_playerNo = coop::roster_ledger::Get(kSlot).playerNo;
    DC::Say(KeyLine(sp.arm, "plain") + Narrow(plainKey));
    DC::Say(KeyLine(sp.arm, "far") + Narrow(farKey));
    g_judgeAt = Clock::now();
    g_step = HStep::Judge;
    UE_LOGI("[HOSTILE-DRILL] %s armed on slot %u: the plain prop '%ls' and the far dingus '%ls' stand and their keys resolve",
            sp.name, static_cast<unsigned>(kSlot), plainKey.c_str(), farKey.c_str());
}

// ---- the judge ----

// The counters' movement since the baseline, in either direction: a fall of the found set would be a change too.
Tally Diff(const Tally& now) {
    Tally d{};
    for (size_t i = 0; i < kFields; ++i) d[i] = now[i] >= g_base[i] ? now[i] - g_base[i] : g_base[i] - now[i];
    return d;
}

// Every report ends as a dispatcher drop, a refusal or an acceptance.
uint64_t Outcomes(const Tally& d) {
    uint64_t n = 0;
    for (size_t i = 0; i < static_cast<size_t>(Field::FoundKeys); ++i) n += d[i];
    return n;
}

std::string Summary(const Tally& d) {
    std::string out;
    for (size_t i = 0; i < kFields; ++i)
        if (d[i]) out += (out.empty() ? "" : ", ") + std::string(FieldName(static_cast<Field>(i))) + "=" + std::to_string(d[i]);
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
    // The client's line, latched on sight: a live chat row retires after seconds. Polled a few times a second.
    if (!g_clientDone && DC::PollDue()) g_clientDone = DC::ChatHasLive(DoneLine(sp.arm));
    if (!g_clientDone) {
        if (Clock::now() - g_judgeAt > kBound) Finish(sp, "INCONCLUSIVE", "the client never said it had sent the reports");
        return;
    }
    const Tally d = Diff(Now());
    if (!s->HasPeerConn(kSlot)) return Finish(sp, "INCONCLUSIVE", "the judged slot left before the verdict");
    if (coop::roster_ledger::Get(kSlot).playerNo != g_playerNo)
        return Finish(sp, "INCONCLUSIVE", "slot-replaced (the judged slot's occupant changed)");
    if (d[static_cast<size_t>(Field::FoundKeys)] != 0)
        return Finish(sp, "FAIL", "a key was added to the found set: " + Summary(d));
    const uint64_t outcomes = Outcomes(d);
    if (outcomes != sp.sent)
        return Finish(sp, "FAIL", "the counters sum to " + std::to_string(outcomes) + " of " + std::to_string(sp.sent) +
                                      " messages sent: " + Summary(d));
    const DS::JudgeView view = DS::Judge();
    const uint64_t span = view.lastMs - view.firstMs;
    if (span > kSpanMs) {
        char secs[24];
        std::snprintf(secs, sizeof(secs), "%.1f", static_cast<double>(span) / 1000.0);
        return Finish(sp, "INCONCLUSIVE", "the " + std::to_string(sp.sent) + " reports were judged over " + secs +
                                              " s, past the budget's refill: " + Summary(d));
    }
    const std::string bad = Mismatch(sp, d);
    if (!bad.empty()) return Finish(sp, "FAIL", "the counters differ from the arm's table:" + bad + " got " + Summary(d));
    Finish(sp, "PASS", "all " + std::to_string(sp.sent) + " messages accounted, as the arm's table: " + Summary(d));
}

}  // namespace

void HostTick(coop::net::Session* s, const Spec& sp) {
    if (g_step == HStep::Done) return;
    if (g_step == HStep::Begin) {
        g_armAt = Clock::now();
        g_step = HStep::Spawn;
    }
    if (g_step == HStep::Spawn) Begin(s, sp);
    else if (g_step == HStep::WaitKeys) WaitKeys(sp);
    else if (g_step == HStep::Judge) Judge(s, sp);
}

void HostReset() {
    g_step = HStep::Begin;
    g_base = Tally{};
    g_armAt = g_judgeAt = {};
    g_spawned = g_clientDone = false;
    g_waiting = "slot 1's world-ready";
    g_playerNo = 0;
    g_plain.Reset();
    g_far.Reset();
}

}  // namespace coop::dev::hostile_drill::dingus::detail
