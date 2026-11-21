// coop/dev/hostile_drill_kdress_host.cpp -- the kdress_ask arm's judge: see coop/dev/hostile_drill_kdress.h. The host spawns
// the two Omegas and the glasses round the client's puppet, says their eids in the lobby chat once each has an element id and
// is at rest, and judges each phase by accounting at the client's line that ends it: the asks and the commit precede it on the
// ordered lane, so each has ended in an answer by then. Between the phases it gives the near Omega the outfit the take-offs
// need. The pat judge's budget refills with time, so pats judged farther apart than kSpanMs make the counts an INCONCLUSIVE,
// never a FAIL.

#include "hostile_drill_kdress_detail.h"  // co-located private header (src tree, not include/)

#include "kdress_drill_detail.h"  // the Omega drill's reads and spawns (co-located private header of the kdress drill)

#include "coop/cheats/item_catalog.h"
#include "coop/cheats/item_spawn.h"
#include "coop/creatures/kerfur_dress_sync.h"
#include "coop/dev/drill_chat.h"
#include "coop/dev/drill_ground.h"
#include "coop/net/session.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"
#include "coop/player/roster_ledger.h"
#include "coop/props/prop_snapshot.h"
#include "coop/save/join_window_baseline.h"

#include "ue_wrap/actors/kerfur_dress.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace coop::dev::hostile_drill::kdress::detail {
namespace {

namespace DC = coop::dev::drill_chat;
namespace DG = coop::dev::drill_ground;
namespace IS = coop::cheats::item_spawn;
namespace KS = coop::kerfur_dress_sync;
namespace KD = ue_wrap::kerfur_dress;
namespace K  = coop::dev::kdress_drill::detail;

enum class HStep { Begin, Spawn, WaitFixtures, WaitHeld, Phase, Done };
HStep                 g_step = HStep::Begin;
Tally                 g_base{};
Clock::time_point     g_armAt{};
bool                  g_spawned = false;
const char*           g_waiting = "slot 1's world-ready";
uint16_t              g_playerNo = 0;    // slot 1's occupant when the host said the eids
int                   g_phase = 0;       // the phase the host waits the client's line for
int                   g_fix = 0;         // the fixture being waited for: 0 near Omega, 1 far Omega, 2 glasses
ue_wrap::CachedObjRef g_near, g_far, g_item;
std::vector<void*>    g_itemBefore;
int                   g_eidNear = 0, g_eidFar = 0, g_eidItem = 0, g_quiet = 0;

// The pat judge's budget (4 at once, 0.5 a second) stays inside the table while the pats are judged this close together.
constexpr uint64_t kSpanMs = 1500;

void Finish(const char* what, const std::string& why) {
    Verdict(what, why);
    g_step = HStep::Done;
}

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

bool Bounded() {
    if (Clock::now() - g_armAt <= kBound) return false;
    Finish("INCONCLUSIVE", std::string("the precondition never held: waiting for ") + g_waiting);
    return true;
}

// The two Omegas and the glasses, round the client's puppet. False and ended when one cannot be placed.
bool SpawnFixtures() {
    std::wstring canonical;
    if (coop::cheats::item_catalog::Resolve(K::Narrow(kGlasses), canonical) == coop::cheats::item_catalog::Lookup::Unfilled) {
        g_waiting = "the game's prop list";
        return false;
    }
    const std::wstring cls = K::ItemClass(kGlasses);
    if (cls.empty()) {
        Finish("INCONCLUSIVE", "the glasses name no class the game spawns");
        return false;
    }
    coop::RemotePlayer& rp = coop::puppet_drive::Puppet(kSlot);
    g_near.Set(K::SpawnOmega(rp.actor(), kNearCm, 0.f));
    g_far.Set(K::SpawnOmega(rp.actor(), kFarCm, 0.f));
    if (!g_near.Get() || !g_far.Get()) {
        Finish("INCONCLUSIVE", "the world refused an Omega's spawn");
        return false;
    }
    g_itemBefore = K::LiveOf(cls.c_str());
    int made = 0;
    IS::Spawn(K::Narrow(kGlasses), 1, kSlot, false, &made);
    return true;
}

// Each fixture, in turn, has an element id and is at rest (the Omega's rest read follows one actor at a time).
bool FixturesStand() {
    if (g_fix == 0) {
        g_eidNear = K::OmegaEid(g_near.Get());
        if (!g_eidNear || !K::AtRest(g_near.Get())) { g_waiting = "the near Omega's id and rest"; return false; }
        g_fix = 1;
    }
    if (g_fix == 1) {
        g_eidFar = K::OmegaEid(g_far.Get());
        if (!g_eidFar || !K::AtRest(g_far.Get())) { g_waiting = "the far Omega's id and rest"; return false; }
        g_fix = 2;
    }
    if (!g_item.Get()) {
        void* item = nullptr;
        for (void* a : K::LiveOf(K::ItemClass(kGlasses).c_str()))
            if (std::find(g_itemBefore.begin(), g_itemBefore.end(), a) == g_itemBefore.end()) item = a;
        g_item.Set(item);
        g_quiet = 0;
    }
    void* const item = g_item.Get();
    g_eidItem = item ? K::PropEid(item) : 0;
    g_quiet = g_eidItem && DG::Quiet(item) ? g_quiet + 1 : 0;
    if (g_quiet < K::kQuietTicks) { g_waiting = "the glasses' id and rest"; return false; }
    return true;
}

// ---- the judge ----

Tally Diff(const Tally& now) {
    Tally d{};
    for (size_t i = 0; i < kFields; ++i) d[i] = now[i] >= g_base[i] ? now[i] - g_base[i] : g_base[i] - now[i];
    return d;
}

std::string Summary(const Tally& d) {
    std::string out;
    for (size_t i = 0; i < kFields; ++i)
        if (d[i]) out += (out.empty() ? "" : ", ") + std::string(FieldName(static_cast<Field>(i))) + "=" + std::to_string(d[i]);
    return out.empty() ? "nothing counted" : out;
}

std::string Mismatch(const Phase& ph, const Tally& d) {
    const Tally want = Expected(ph);
    std::string bad;
    for (size_t i = 0; i < kFields; ++i)
        if (d[i] != want[i])
            bad += std::string(" ") + FieldName(static_cast<Field>(i)) + " " + std::to_string(d[i]) + " (wanted " +
                   std::to_string(want[i]) + ");";
    return bad;
}

// The client's line that ends the phase: judge the counters, then what the near Omega wears.
void JudgePhase(coop::net::Session* s) {
    const Phase& ph = PhaseAt(g_phase);
    const std::string done = Line((std::string("p") + std::to_string(g_phase + 1) + "-done").c_str());
    if (!DC::SeenHas(done)) {
        if (Bounded()) return;
        g_waiting = "the client's phase line";
        return;
    }
    if (!s->HasPeerConn(kSlot)) return Finish("INCONCLUSIVE", "the judged slot left before the verdict");
    if (coop::roster_ledger::Get(kSlot).playerNo != g_playerNo)
        return Finish("INCONCLUSIVE", "slot-replaced (the judged slot's occupant changed)");
    const Tally d = Diff(Now());
    if (g_phase == 2) {
        const KS::dev::PatView v = KS::dev::Pats();
        if (v.lastMs - v.firstMs > kSpanMs) {
            char secs[24];
            std::snprintf(secs, sizeof(secs), "%.1f", static_cast<double>(v.lastMs - v.firstMs) / 1000.0);
            return Finish("INCONCLUSIVE", std::string("the pats were judged over ") + secs +
                                              " s, past the budget's refill: " + Summary(d));
        }
    }
    const std::string bad = Mismatch(ph, d);
    if (!bad.empty())
        return Finish("FAIL", std::string("phase ") + ph.name + ": the counters differ from the table:" + bad + " got " + Summary(d));
    void* const n = K::OmegaOf(g_eidNear);
    if (!n) return Finish("INCONCLUSIVE", "the near Omega is gone");
    const std::string wears = K::DripText(n);
    if (g_phase == 0 && wears != K::Narrow(kGlasses))
        return Finish("FAIL", "phase asks: the near Omega wears [" + wears + "], the glasses alone expected");
    if (g_phase == 1 && wears != K::Narrow(kMaid))
        return Finish("FAIL", "phase takeoffs: the near Omega wears [" + wears + "], the maid alone expected");
    UE_LOGI("[HOSTILE-DRILL] kdress_ask phase %s matches the table: %s; the near Omega wears [%s]", ph.name,
            Summary(d).c_str(), wears.c_str());
    if (g_phase == 0) {
        // The take-offs need rows to take off: the host dresses the near Omega with the glasses, the bucket and the maid.
        if (!KD::ApplyDrip(n, {kGlasses, kBucket, kMaid}) || K::DripText(n).find(K::Narrow(kMaid)) == std::string::npos)
            return Finish("INCONCLUSIVE", "the host could not dress the near Omega for the take-offs");
    }
    if (++g_phase == kPhases) {
        Finish("PASS", "the forged asks and pats were answered and counted as the table, one reason each, and nothing a client "
                       "forged was written");
        return;
    }
    g_base = Now();
    if (g_phase == 2) KS::dev::ResetPatSpan();
    DC::Say(Line((std::string("go") + std::to_string(g_phase + 1)).c_str()));
}

}  // namespace

void HostTick(coop::net::Session* s) {
    if (g_step == HStep::Done) return;
    if (g_step == HStep::Begin) {
        g_armAt = Clock::now();
        g_step = HStep::Spawn;
    }
    if (g_step == HStep::Spawn) {
        if (Bounded()) return;
        if (!PuppetReady(s)) return;
        if (!g_spawned) {
            if (!SpawnFixtures()) return;
            g_spawned = true;
        }
        g_step = HStep::WaitFixtures;
    }
    if (g_step == HStep::WaitFixtures) {
        if (Bounded()) return;
        if (!FixturesStand()) return;
        g_playerNo = coop::roster_ledger::Get(kSlot).playerNo;
        g_base = Now();
        DC::SeenHas(Line("held"));   // starts the chat history
        DC::Say(Line(("targets " + std::to_string(g_eidNear) + " " + std::to_string(g_eidFar) + " " +
                      std::to_string(g_eidItem)).c_str()));
        UE_LOGI("[HOSTILE-DRILL] kdress_ask armed on slot %u: the near Omega eid=%d, the far Omega eid=%d, the glasses eid=%d",
                static_cast<unsigned>(kSlot), g_eidNear, g_eidFar, g_eidItem);
        g_step = HStep::WaitHeld;
    }
    if (g_step == HStep::WaitHeld) {
        if (!DC::SeenHas(Line("held"))) {
            Bounded();
            return;
        }
        // The client holds the glasses and the lane has announced them: the baselines are taken, the first phase goes.
        g_base = Now();
        g_phase = 0;
        DC::Say(Line("go1"));
        g_step = HStep::Phase;
        return;
    }
    if (g_step == HStep::Phase) JudgePhase(s);
}

void HostReset() {
    g_step = HStep::Begin;
    g_base = Tally{};
    g_armAt = {};
    g_spawned = false;
    g_waiting = "slot 1's world-ready";
    g_playerNo = 0;
    g_phase = g_fix = g_quiet = 0;
    g_near.Reset();
    g_far.Reset();
    g_item.Reset();
    g_itemBefore.clear();
    g_eidNear = g_eidFar = g_eidItem = 0;
}

}  // namespace coop::dev::hostile_drill::kdress::detail
