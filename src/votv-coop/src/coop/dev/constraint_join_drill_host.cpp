// coop/dev/constraint_join_drill_host.cpp -- the constraint-join drill's host half: the nailed set, its carry (c1, c3) or its
// watch (c2), and the verdicts that read the host's own drive table. See coop/dev/constraint_join_drill.h.

#include "constraint_join_drill_detail.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/drill_chat.h"
#include "coop/dev/drill_ground.h"
#include "coop/dev/nail_act.h"
#include "coop/net/net_clock.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"
#include "coop/props/constraint_ties.h"
#include "coop/props/prop_drive_host.h"
#include "coop/props/prop_element_tracker.h"
#include "coop/props/prop_snapshot.h"
#include "coop/save/join_window_baseline.h"

#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"

#include <algorithm>
#include <cstdio>

namespace coop::dev::constraint_join_drill::detail::host {
namespace {

namespace DC = coop::dev::drill_chat;
namespace DG = coop::dev::drill_ground;
namespace DR = coop::director;
namespace NA = coop::dev::nail_act;
namespace PD = coop::prop_drive_host;
namespace E  = ue_wrap::engine;
namespace R  = ue_wrap::reflection;
using ue_wrap::FVector;

constexpr const char* kLog = "[CONSTRAINT-JOIN-DRILL] [H]";

char Letter(size_t i) { return static_cast<char>('A' + i); }

// The nail act's voice for this drill: plain log lines under the drill's tag, no labels; a step that cannot run ends INVALID.
class DrillVoice final : public NA::Voice {
public:
    void Label(void*, const char*) override {}
    void Info(const char* line) override { UE_LOGI("%s %s", kLog, line); }
    void Warn(const char* line) override { UE_LOGW("%s %s", kLog, line); }
    void Event(const char* line) override { UE_LOGI("%s %s", kLog, line); }
    void Unmeasured(const char* act, const char* line) override { Invalid('H', "%s: %s", act, line); }
    std::string TransformOf(void* actor) override {
        FVector at{};
        char b[96] = "unread";
        if (E::TryGetActorLocation(actor, at))
            std::snprintf(b, sizeof(b), "(%.1f,%.1f,%.1f)", static_cast<double>(at.X), static_cast<double>(at.Y),
                          static_cast<double>(at.Z));
        return b;
    }
    std::string LabelOf(void*) override { return "box"; }
    std::string NailState(void*) override { return "nailed"; }
};

enum class Step : uint8_t { WaitPeer, Bind, Nail, NailWait, Drain, Park, Announce, Run, Settle, WaitLeave, WaitJoiner, Done };

DrillVoice g_voice;
Step       g_step = Step::WaitPeer;
uint64_t   g_stepMs = 0;
int        g_slot = 0;
size_t     g_n = 0;
void*      g_boxCls = nullptr;
void*      g_nailCls = nullptr;
FVector    g_spot{};
ue_wrap::CachedObjRef g_box[kMaxBoxes];
uint32_t   g_eid[kMaxBoxes] = {};
uint8_t    g_gen0[kMaxBoxes] = {};        // each box's drive generation when the set was built and every drive closed
DG::Settle g_rest[kMaxBoxes];
size_t     g_nailed = 0;
int        g_driveFree = 0;
bool       g_parkStarted = false;
std::shared_ptr<DR::BackgroundWalk> g_park;
CarryAct   g_carry;
bool       g_carryDone = false;
SetInfo    g_set;

// What the host reads of the carry, tick by tick (Observe).
bool       g_taken = false, g_letGo = false, g_leaderOpened = false, g_judged = false, g_capHit = false;
bool       g_prevOpen[kMaxBoxes] = {};
uint8_t    g_heldGen[kMaxBoxes] = {};      // each box's generation at the last tick the leader was still in a hand
uint64_t   g_endMs[kMaxBoxes] = {};        // when each box's row last closed
uint64_t   g_phaseMs = 0;

void To(Step s) {
    g_step = s;
    g_stepMs = coop::net::NowMs();
}
bool Expired() { return coop::net::NowMs() - g_stepMs > kBoundMs; }
bool PhaseExpired() { return coop::net::NowMs() - g_phaseMs > kBoundMs; }

void InvalidWaiting(const char* what) { Invalid('H', "%s never came", what); }

bool SlotReady(coop::net::Session& s, int slot) {
    return s.IsSlotWorldReady(slot) && coop::prop_snapshot::IsBracketClosed(slot) &&
           !coop::join_window_baseline::IsLateWindowOpen(slot);
}

int FirstReadySlot(coop::net::Session& s) {
    for (int i = 1; i < coop::net::kMaxPeers; ++i)
        if (SlotReady(s, i)) return i;
    return 0;
}

// Every box is alive and its element id is read. False says INVALID: a box that dies ends the drill.
bool Refresh() {
    for (size_t i = 0; i < g_n; ++i) {
        void* b = g_box[i].Get();
        if (!b) {
            Invalid('H', "box %c died", Letter(i));
            return false;
        }
        const coop::element::ElementId e = coop::prop_element_tracker::GetPropElementIdForActor(b);
        g_eid[i] = e == coop::element::kInvalidId ? 0u : static_cast<uint32_t>(e);
    }
    return true;
}

bool AllBound() {
    for (size_t i = 0; i < g_n; ++i)
        if (g_eid[i] == 0u) return false;
    return true;
}

bool AllClosed() {
    for (size_t i = 0; i < g_n; ++i)
        if (PD::ContextOf(g_box[i].Get(), g_eid[i]).open) return false;
    return true;
}

// Every box has stood still for `ticks` ticks (each box's own count advances every call).
bool AllStable(int ticks) {
    bool all = true;
    for (size_t i = 0; i < g_n; ++i) {
        FVector at{};
        void* b = g_box[i].Get();
        const bool stable = E::TryGetActorLocation(b, at) && DG::Stable(g_rest[i], at, DG::Quiet(b), ticks);
        all = all && stable;
    }
    return all;
}

void RestartRest() {
    for (size_t i = 0; i < g_n; ++i) DG::Restart(g_rest[i]);
}

float NearestBoxCm(const FVector& from) {
    float best = 1e9f;
    for (size_t i = 0; i < g_n; ++i) {
        FVector at{};
        if (E::TryGetActorLocation(g_box[i].Get(), at)) best = (std::min)(best, DG::Dist(from, at));
    }
    return best;
}

// A tie between box `k` and box `k+1`, read from the index the join walks.
bool TiedPair(size_t k) {
    void* tied[32];
    const size_t m = coop::constraint_ties::TiesOf(g_box[k].Get(), tied, 32);
    for (size_t j = 0; j < m; ++j)
        if (tied[j] == g_box[k + 1].Get()) return true;
    return false;
}

void StepWaitPeer(coop::net::Session& s) {
    const int slot = FirstReadySlot(s);
    if (!slot) { Wait('H', "a client slot that is world-ready with its join window closed"); return; }
    void* puppet = coop::puppet_drive::Puppet(slot).GetActor();
    void* host = coop::players::Registry::Get().Local();
    FVector at{};
    if (!puppet || !R::IsLive(puppet) || !E::TryGetActorLocation(puppet, at) || !host || !R::IsLive(host)) {
        Wait('H', "the client's puppet and this host's pawn to read");
        return;
    }
    g_slot = slot;
    g_n = BoxCount(ArmOf());
    if (ArmOf() == Arm::C3 && coop::config::ResolveInt(::coop::config_registry::rows::constraint_join_drill_cap) <= 0) {
        Invalid('H', "c3 needs constraint_join_drill_cap above 0: with the join's own cap no six-box chain reaches it");
        return;
    }
    const float half = static_cast<float>(g_n - 1) * kStepCm * 0.5f + kFootCm;
    if (!DG::FindSpot(host, at, half, kBoxClass, kCrowdCm, "CONSTRAINT-JOIN-DRILL", g_spot)) {
        Invalid('H', "no level open ground with a clear footprint and no other box near");
        return;
    }
    g_boxCls = ue_wrap::asset_load::LoadObjectByPath(kBoxPath);
    g_nailCls = ue_wrap::asset_load::LoadObjectByPath(kNailPath);
    if (!g_boxCls || !g_nailCls) { Invalid('H', "a class did not load (box=%d nail=%d)", g_boxCls ? 1 : 0, g_nailCls ? 1 : 0); return; }
    const float ax = g_spot.X - static_cast<float>(g_n - 1) * kStepCm * 0.5f;
    for (size_t i = 0; i < g_n; ++i) {
        void* b = E::SpawnActor(g_boxCls, FVector{ax + static_cast<float>(i) * kStepCm, g_spot.Y, g_spot.Z + kLiftCm});
        if (!b) { Invalid('H', "box %c did not spawn", Letter(i)); return; }
        g_box[i].Set(b);
    }
    FVector origin{}, ext{};
    if (!E::GetActorBounds(g_box[0].Get(), true, origin, ext) || 2.f * ext.X + 10.f > kStepCm) {
        Invalid('H', "a box's width (half extent %.1f cm) does not fit the %.0f cm step", static_cast<double>(ext.X),
                static_cast<double>(kStepCm));
        return;
    }
    UE_LOGI("%s spawned %zu boxes %.0f cm apart over the floor (%.0f, %.0f, %.0f), %.0f cm from slot %d's puppet", kLog, g_n,
            static_cast<double>(kStepCm), static_cast<double>(g_spot.X), static_cast<double>(g_spot.Y),
            static_cast<double>(g_spot.Z), static_cast<double>(DG::Dist(g_spot, at)), slot);
    RestartRest();
    To(Step::Bind);
}

void StepBind() {
    if (!Refresh()) return;
    const bool stable = AllStable(kQuietTicks);
    if (!(AllBound() && stable && AllClosed())) {
        Wait('H', "every box's element id, rest and the end of its spawn fall's drive");
        if (Expired()) InvalidWaiting("every box's element id and rest");
        return;
    }
    g_nailed = 0;
    To(Step::Nail);
}

void StepNail() {
    char label[8];
    std::snprintf(label, sizeof(label), "J%zu", g_nailed);
    void* nail = nullptr;
    if (!NA::NailThePair(g_voice, "cj", label, g_nailCls, g_box[g_nailed].Get(), g_box[g_nailed + 1].Get(), nail)) return;
    RestartRest();
    To(Step::NailWait);
}

void StepNailWait() {
    if (!Refresh()) return;
    const bool stable = AllStable(kQuietTicks);
    if (!(stable && AllClosed())) {
        Wait('H', "the set to rest after the nail");
        if (Expired()) InvalidWaiting("the set's rest after the nail");
        return;
    }
    RestartRest();
    g_driveFree = 0;
    To(++g_nailed < g_n - 1 ? Step::Nail : Step::Drain);
}

// The nails' own settling is over: no drive open on any box for kDriveFreeTicks in a row, all still. The tie index must hold
// each nail's tie (a precondition: the join walks it), and each box's drive generation is taken as the drill's baseline.
void StepDrain() {
    if (!Refresh()) return;
    const bool stable = AllStable(kDriveFreeTicks);
    g_driveFree = AllClosed() ? g_driveFree + 1 : 0;
    if (!(stable && g_driveFree >= kDriveFreeTicks)) {
        Wait('H', "no drive open on the nailed set");
        if (Expired()) InvalidWaiting("the end of the nailed set's drives");
        return;
    }
    for (size_t k = 0; k + 1 < g_n; ++k) {
        if (!TiedPair(k)) {
            Invalid('H', "the tie index holds no tie between box %c and box %c after nailNail (the nail did not take, or its "
                    "SetConstrainedComponents seam did not fire)", Letter(k), Letter(k + 1));
            return;
        }
    }
    for (size_t i = 0; i < g_n; ++i) g_gen0[i] = PD::ContextOf(g_box[i].Get(), g_eid[i]).gen;
    UE_LOGI("%s the nailed set stands (%zu boxes, %zu ties indexed), every drive closed; the baseline generations are taken", kLog,
            g_n, coop::constraint_ties::Count());
    To(ArmOf() == Arm::C2 ? Step::Park : Step::Announce);
}

// c2: the host's body must be more than kAwayCm from the set, or a body's push of its own would give B a row. It walks to
// open ground far from the set unless it already stands that far.
void StepPark() {
    void* host = coop::players::Registry::Get().Local();
    FVector me{};
    if (!host || !E::TryGetActorLocation(host, me)) { Wait('H', "the host's pawn"); return; }
    if (!g_parkStarted) {
        g_parkStarted = true;
        if (NearestBoxCm(me) > kAwayCm) { To(Step::Announce); return; }
        FVector park{};
        if (!DG::FindSpot(host, g_spot, 50.f, kBoxClass, kCrowdCm, "CONSTRAINT-JOIN-DRILL", park)) {
            Invalid('H', "no open ground far from the set for the host to wait on");
            return;
        }
        g_park = DR::StartBackgroundWalk(park, 150.f, kWalkDeadlineS);
        UE_LOGI("%s the host walks off to (%.0f, %.0f, %.0f); it stands %.0f cm from the set", kLog, static_cast<double>(park.X),
                static_cast<double>(park.Y), static_cast<double>(park.Z), static_cast<double>(NearestBoxCm(me)));
        g_stepMs = coop::net::NowMs();
        return;
    }
    const int st = g_park ? g_park->state.load() : 2;
    if (st == 0) {
        Wait('H', "the host's walk away from the set");
        if (Expired()) InvalidWaiting("the host's walk away from the set");
        return;
    }
    g_park.reset();
    if (st != 1) { Invalid('H', "the host's walk away from the set did not arrive"); return; }
    To(Step::Announce);
}

void SayFinals() {
    for (size_t i = 0; i < g_n; ++i) {
        FVector at{};
        E::TryGetActorLocation(g_box[i].Get(), at);
        DC::Say(FinalLine(i, g_eid[i], at));
    }
}

void StepAnnounce() {
    g_set = SetInfo{};
    g_set.n = g_n;
    for (size_t i = 0; i < g_n; ++i) g_set.eid[i] = g_eid[i];
    E::TryGetActorLocation(g_box[0].Get(), g_set.a);
    DC::Say(ReadyLine(g_set));
    for (size_t i = 0; i < g_n; ++i) g_prevOpen[i] = PD::ContextOf(g_box[i].Get(), g_eid[i]).open;
    if (ArmOf() != Arm::C2) g_carry.Begin(g_box[0].Get(), FVector{-1.f, 0.f, 0.f}, 'H');
    else g_carryDone = true;
    g_phaseMs = coop::net::NowMs();
    UE_LOGI("%s READY: the set is told to the client; %s carries box A", kLog, ArmOf() == Arm::C2 ? "the client" : "the host");
    To(Step::Run);
}

// A member that has had no row since `since` is a FAIL under `leg`, naming it.
bool Judge(const char* leg, const char* since, const uint8_t* gens) {
    for (size_t i = 1; i < g_n; ++i) {
        if (gens[i] != g_gen0[i]) continue;
        Fail('H', leg, "box %c (element %u) had no drive row since %s: its drive generation stayed %u", Letter(i), g_eid[i], since,
             static_cast<unsigned>(gens[i]));
        return false;
    }
    return true;
}

// The host's reads of the drive table, every tick from the ready line: the leader's hand (the take, the let-go), each box's
// row closing (an End), and the join cap.
void Observe() {
    const uint64_t now = coop::net::NowMs();
    void* a = g_box[0].Get();
    bool open[kMaxBoxes];
    uint8_t gen[kMaxBoxes];
    for (size_t i = 0; i < g_n; ++i) {
        const PD::DriveContext c = PD::ContextOf(g_box[i].Get(), g_eid[i]);
        open[i] = c.open;
        gen[i] = c.gen;
        if (g_prevOpen[i] && !c.open && g_taken) {
            g_endMs[i] = now;
            UE_LOGI("%s box %c's row closed (an End), generation %u", kLog, Letter(i), static_cast<unsigned>(c.gen));
        }
    }
    const bool held = PD::InAHand(a);
    if (!g_taken && held) {
        g_taken = true;
        g_phaseMs = now;
        if (ArmOf() == Arm::C2) {
            void* host = coop::players::Registry::Get().Local();
            FVector me{};
            if (!host || !E::TryGetActorLocation(host, me)) { Invalid('H', "the host's pawn could not be read at the client's take"); return; }
            const float away = NearestBoxCm(me);
            if (away <= kAwayCm) {
                Invalid('H', "the host's body stood %.0f cm from the set at the client's take, within %.0f cm", static_cast<double>(away),
                        static_cast<double>(kAwayCm));
                return;
            }
        }
        UE_LOGI("%s box A is taken (a hand holds it)", kLog);
    }
    if (g_taken && !g_judged) {
        if (held) std::copy(gen, gen + g_n, g_heldGen);
        // The cap was reached when a member stands with no row while no join slot is free (c3's precondition).
        for (size_t i = 1; i < g_n && !g_capHit; ++i) {
            void* b = g_box[i].Get();
            if (!PD::IsDriven(b) && !PD::InAHand(b) && !PD::JoinSlotFree()) {
                g_capHit = true;
                UE_LOGI("%s the join cap is reached: box %c waits with no row and no join slot is free", kLog, Letter(i));
            }
        }
    }
    if (g_taken && !held && !g_letGo) {
        g_letGo = true;
        g_phaseMs = now;
        UE_LOGI("%s box A is let go (the hand released it)", kLog);
        // The leader's let-go row (and the join its own Owe owes) must not count: the check reads the last tick of the hold.
        if (ArmOf() == Arm::C2 && !Judge("let-go", "the client's take", g_heldGen)) return;
    }
    if (g_letGo && open[0]) g_leaderOpened = true;
    if (g_letGo && !g_judged && g_leaderOpened && g_prevOpen[0] && !open[0]) {
        g_judged = true;
        UE_LOGI("%s the leader's row closed (its End): the members are judged", kLog);
        if (ArmOf() == Arm::C3 && !g_capHit) {
            Invalid('H', "the join cap was never reached: no member waited without a row while no join slot was free");
            return;
        }
        Judge("leader-end", "the carry began", gen);
    }
    for (size_t i = 0; i < g_n; ++i) g_prevOpen[i] = open[i];
}

void StepRun() {
    if (!Refresh()) return;
    Observe();
    if (Over()) return;
    if (!g_carryDone && g_carry.Tick() == CarryAct::Status::Done) g_carryDone = true;
    if (Over()) return;
    if (g_judged) {
        RestartRest();
        To(Step::Settle);
        return;
    }
    const char* what = g_letGo ? "the leader's End" : g_taken ? "the leader's let-go" : "the leader's take";
    Wait('H', "%s", what);
    if (PhaseExpired()) InvalidWaiting(what);
}

void StepSettle() {
    if (!Refresh()) return;
    const bool stable = AllStable(kQuietTicks);
    if (!(stable && AllClosed())) {
        Wait('H', "every box's row to close and the set to rest");
        if (Expired()) InvalidWaiting("the end of every box's row");
        return;
    }
    uint64_t lo = ~0ull, hi = 0;
    for (size_t i = 0; i < g_n; ++i) {
        if (g_endMs[i] == 0) continue;
        lo = (std::min)(lo, g_endMs[i]);
        hi = (std::max)(hi, g_endMs[i]);
    }
    if (hi >= lo)
        UE_LOGI("%s the boxes' last Ends span %llu ms, against the bound of %llu ms (two rest probes): %s", kLog,
                static_cast<unsigned long long>(hi - lo), static_cast<unsigned long long>(kSpreadBoundMs),
                hi - lo <= kSpreadBoundMs ? "within it" : "beyond it");
    SayFinals();
    UE_LOGI("%s DONE: every member had a row; the final places are told to the client", kLog);
    To(ArmOf() == Arm::C2 ? Step::WaitLeave : Step::Done);
}

void StepWaitLeave(coop::net::Session& s) {
    if (SlotReady(s, g_slot)) {
        Wait('H', "the client to leave for its rejoin");
        if (Expired()) InvalidWaiting("the client's leaving for its rejoin");
        return;
    }
    UE_LOGI("%s the client has left; the host waits for its second life", kLog);
    To(Step::WaitJoiner);
}

void StepWaitJoiner(coop::net::Session& s) {
    if (!FirstReadySlot(s)) {
        Wait('H', "the joiner's world to come up");
        if (Expired()) InvalidWaiting("the joiner's world to come up");
        return;
    }
    DC::Say(ReadyLine(g_set));
    SayFinals();
    UE_LOGI("%s the joiner's world is up: the set and its final places are said again", kLog);
    To(Step::Done);
}

}  // namespace

void Tick(coop::net::Session& s) {
    switch (g_step) {
    case Step::WaitPeer:   StepWaitPeer(s); return;
    case Step::Bind:       StepBind(); return;
    case Step::Nail:       StepNail(); return;
    case Step::NailWait:   StepNailWait(); return;
    case Step::Drain:      StepDrain(); return;
    case Step::Park:       StepPark(); return;
    case Step::Announce:   StepAnnounce(); return;
    case Step::Run:        StepRun(); return;
    case Step::Settle:     StepSettle(); return;
    case Step::WaitLeave:  StepWaitLeave(s); return;
    case Step::WaitJoiner: StepWaitJoiner(s); return;
    case Step::Done:       return;
    }
}

bool KeepsAcrossDisconnect() { return ArmOf() == Arm::C2 && (g_step == Step::WaitLeave || g_step == Step::WaitJoiner); }

void Reset() {
    g_step = Step::WaitPeer;
    g_stepMs = 0;
    g_slot = 0;
    g_n = 0;
    g_boxCls = g_nailCls = nullptr;
    g_spot = {};
    for (size_t i = 0; i < kMaxBoxes; ++i) {
        g_box[i].Reset();
        g_eid[i] = 0;
        g_gen0[i] = g_heldGen[i] = 0;
        g_prevOpen[i] = false;
        g_endMs[i] = 0;
        DG::Restart(g_rest[i]);
    }
    g_nailed = 0;
    g_driveFree = 0;
    g_parkStarted = false;
    if (g_park) DR::CancelBackgroundWalk(*g_park);
    g_park.reset();
    g_carry.Reset();
    g_carryDone = false;
    g_set = SetInfo{};
    g_taken = g_letGo = g_leaderOpened = g_judged = g_capHit = false;
    g_phaseMs = 0;
}

}  // namespace coop::dev::constraint_join_drill::detail::host
