// harness/autotest/autotest_broomstroke.cpp -- see harness/autotest/broomstroke.h for the phases.
//
// Every stroke is the game's own: the drill presses the broom's right mouse button through the
// entry the player's input reaches the hand with, the swing montage plays, and its notify runs the
// stroke -- on a client refused and sent, on the host run, and for a client's the host runs it on
// its mirror. The broom is put in the hand by the game's own pickup and the striker is moved by the
// game's own teleport. What the verdicts turn on is counted in the world, never asked of the lane;
// the rows live in broomstroke_world.h.

#include "harness/autotest/broomstroke.h"

#include "broomstroke_world.h"   // co-located private header (src tree, not include/)

#include "harness/autotest.h"   // IsClientRole
#include "coop/net/session.h"
#include "coop/session/net_pump.h"          // HasAnnouncedWorldReady
#include "harness/session_runtime.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/types.h"

#include <functional>
#include <memory>
#include <vector>

namespace harness::autotest {
namespace {

namespace BW = broom_world;

// How far a chip-pile census reaches around a striker: past the stroke's sphere and past where a
// swept clump rolls.
constexpr float kPileRadiusCm = 700.f;
// The heap: nine chip piles around a pile on the floor, within the stroke's 50 uu sphere of its
// centre, so one stroke sweeps ten -- more clumps than one datagram of the roll stream carries.
constexpr int   kHeapExtra = 9;
constexpr float kHeapRingCm = 30.f;
constexpr float kHeapCensusCm = 250.f;
// A client teleported facing this far off its subject and then turned onto it: its body follows its
// view, while the host's puppet keeps the teleport's facing inside its 60-degree turn-in-place hold,
// so the heading its push takes tells the client's heading from the puppet's.
constexpr float kBodyTurnDeg = 40.f;
// B's pile stands alone. A clump born among others is knocked about by their births: six clumps of a
// pile on A's landed heap launched 22 degrees off the push between them (run 19), where a lone clump
// leaves along it.
constexpr float kSubjectAloneCm = 150.f;
// ... and lies on the floor A's striker stood on, where the drill's teleports have always gone: a pile
// the rule picks elsewhere is a destination no run has checked against the game's backrooms trigger.
constexpr float kSubjectFromStrikerCm = 800.f;
// The dispenser phase counts trash within this of its pile; the push phases census props within
// the next, which is where that trash landed.
constexpr float kTrashRadiusCm = 300.f;
constexpr float kPropRadiusCm = 400.f;
// The rest phases work on the floor phase A's heap lay on, where the host stood for A and the heap
// rolled: two chip piles either side of the heap's seed, this far off it across the roll, far
// enough apart that one stroke's sphere takes one. Every other spot a drill picks is a gamble with
// the world: a pile beside a backrooms trigger sent the host there and ended the session (run 10).
// The client's census reaches further than the host's, so every id the host follows the client
// follows too.
constexpr float kRestSideCm = 60.f;
constexpr float kRestCensusHostCm = 250.f;
constexpr float kRestCensusClientCm = 400.f;
// A knock straight up: the clump rises about 80 cm and lands where it lay.
constexpr float kKnockUpCmS = 400.f;
// A rest phase's clump is the one born of its pile: within this of where the pile stood.
constexpr float kSubjectNearCm = 80.f;

// Where phase A's heap lay and where the host stood to strike it.
struct RestSite {
    bool valid = false;
    ue_wrap::FVector seed{};
    ue_wrap::FVector stand{};
    float yaw = 0.f;
};

// One thing the striker does, at `ms`, while the tracker follows the ids.
struct Beat {
    DWORD ms;
    std::function<void()> act;
};

// Follow the ids near `center` until `endMs`, doing each beat when it falls due: a beat blocks the
// tracker while it runs -- a swing until its notify -- and the tracker reads the world again the
// moment it returns.
void TrackThrough(const std::vector<uint32_t>& ids, const ue_wrap::FVector& center, float radiusCm,
                  const std::vector<Beat>& beats, DWORD endMs, DWORD t0, const char* who, const char* phase) {
    std::vector<BW::TrackState> states;
    for (uint32_t id : ids) states.push_back(BW::TrackState{id, -1, ue_wrap::FVector{}});
    int unnamed = -1;
    size_t next = 0;
    for (;;) {
        const DWORD elapsed = ::GetTickCount() - t0;
        if (elapsed >= endMs) break;
        if (next < beats.size() && elapsed >= beats[next].ms) beats[next++].act();
        BW::TrackStep(states, center, radiusCm, unnamed, who, phase);
        ::Sleep(50);
    }
    BW::TrackFinal(states, who, phase);
}

// The clump among `ids` nearest `at`, if one is within `withinCm` of it.
uint32_t ClumpNear(const std::vector<uint32_t>& ids, const ue_wrap::FVector& at, float withinCm,
                   ue_wrap::FVector& outPos) {
    uint32_t best = 0;
    float bestCm = withinCm;
    for (const BW::TrackState& s : BW::FormsOf(ids)) {
        if (s.form != 2) continue;
        const float d = BW::Dist(s.pos, at);
        if (d <= bestCm) { bestCm = d; best = s.eid; outPos = s.pos; }
    }
    return best;
}

// Wait on this thread until `ms` after `t0`.
void At(DWORD t0, DWORD ms) {
    for (;;) {
        const DWORD elapsed = ::GetTickCount() - t0;
        if (elapsed >= ms) return;
        ::Sleep(ms - elapsed > 100 ? 100 : ms - elapsed);
    }
}

// ---- C: the client empties a dispenser pile with strokes; D and E: the dispensed trash pushed, by
// the host and then by the client ----
void DispenserPhases(bool isClient, const char* who, DWORD t0) {
    auto sb = std::make_shared<BW::Dispenser>();
    At(t0, 54000);
    int trashBefore = 0;
    if (!BW::PickDispenser(sb, who, trashBefore)) return;
    const bool aliveBefore = BW::DispenserAlive(sb);
    if (isClient) {
        BW::AimAt(sb->pos, 0.f, who, "C");
        for (int i = 0; i < 4; ++i) {
            At(t0, 59000 + static_cast<DWORD>(i) * 2000);
            if (!BW::DispenserAlive(sb)) break;
            BW::Swing(who, "C", 1, 1500);
        }
    }
    At(t0, 70000);
    const bool aliveAfter = BW::DispenserAlive(sb);
    auto trashAfter = std::make_shared<int>(0);
    BW::RunGT([sb, trashAfter](std::atomic<int>& d) { *trashAfter = BW::CountTrashNear(sb->pos, kTrashRadiusCm); d.store(1); });
    const int gained = *trashAfter - trashBefore;
    UE_LOGI("broom_drill: VERDICT role=%s phase=C key='%ls' trash %d->%d (gained %d) pileAlive %d->%d -- %s",
            who, sb->key.c_str(), trashBefore, *trashAfter, gained, aliveBefore ? 1 : 0, aliveAfter ? 1 : 0,
            (gained > 0 && aliveBefore && !aliveAfter) ? "PASS: the client's strokes put trash here and took the pile"
            : gained > 0 ? "PARTIAL: trash arrived but the pile still stands"
                         : "FAIL: nothing came out of the pile on this peer");

    struct PushPhase { const char* name; bool hostStrikes; DWORD aimMs; DWORD strokeMs; };
    const PushPhase push[] = {{"D", true, 72000, 76000}, {"E", false, 84000, 88000}};
    for (const PushPhase& ph : push) {
        const bool striking = (ph.hostStrikes != isClient);
        ue_wrap::FVector prop{};
        bool haveProp = false;
        At(t0, ph.aimMs);
        if (striking && (haveProp = BW::PickProp(sb->pos, kPropRadiusCm, who, ph.name, prop)))
            BW::AimAt(prop, 0.f, who, ph.name);
        At(t0, ph.strokeMs - 1000);
        BW::CensusProps(sb->pos, kPropRadiusCm, who, ph.name, "before");
        At(t0, ph.strokeMs);
        if (striking && haveProp) BW::Swing(who, ph.name, 1, 2500);
        At(t0, ph.strokeMs + 6000);
        BW::CensusProps(sb->pos, kPropRadiusCm, who, ph.name, "late");
    }
}

// ---- G: the host strikes two chip piles, and the drill takes each push back and silences each
// clump's re-pile, so the clump drops beside its pile, lies un-piled and its carry closes at rest;
// H: the drill gives the first its hit back and knocks it up, and it lands as a pile nobody
// carries; I: the host pushes the second, still silenced, so its carry opens again and it rolls
// as far as a roll that ends with no re-pile does ----
void RestPhases(bool isClient, const char* who, DWORD t0, const RestSite& site) {
    At(t0, 96000);
    const bool striking = !isClient && site.valid;
    const ue_wrap::FVector g0{site.seed.X, site.seed.Y - kRestSideCm, site.seed.Z};
    const ue_wrap::FVector g1{site.seed.X, site.seed.Y + kRestSideCm, site.seed.Z};
    if (striking) {
        BW::StandAt(site.stand, site.yaw);
        const std::vector<ue_wrap::FVector> piles{g0, g1};
        BW::SpawnPilesAt(piles, who, "G");
        // The adoption scan names them; a pile with no id is neither tracked nor swept under one.
        for (DWORD waited = 0; waited < 8000 && (BW::ChipPilesNear(g0, 10.f) < 1 || BW::ChipPilesNear(g1, 10.f) < 1);
             waited += 250)
            ::Sleep(250);
    }
    At(t0, 106000);
    ue_wrap::FVector center{};
    if (!BW::StrikerBody(!isClient, 0, center)) {
        UE_LOGW("broom_drill: %s INVALID -- the striker's body could not be placed", who);
        return;
    }
    const float radius = isClient ? kRestCensusClientCm : kRestCensusHostCm;
    std::vector<uint32_t> ids;
    BW::CensusPiles(center, radius, who, "G", "before", &ids);

    std::vector<Beat> beatsG, beatsH, beatsI;
    if (striking) {
        beatsG.push_back(Beat{106500, [who, g0] { BW::LookAt(g0, who, "G"); }});
        beatsG.push_back(Beat{108000, [who, &ids] { BW::FreezeNextStroke(ids); BW::Swing(who, "G", 1, 2500); }});
        beatsG.push_back(Beat{110000, [who, g1] { BW::LookAt(g1, who, "G"); }});
        beatsG.push_back(Beat{111500, [who, &ids] { BW::FreezeNextStroke(ids); BW::Swing(who, "G", 1, 2500); }});
        beatsH.push_back(Beat{118000, [who, &ids, g0] {
            ue_wrap::FVector at{};
            const uint32_t eid = ClumpNear(ids, g0, kSubjectNearCm, at);
            UE_LOGI("broom_drill: %s H SUBJECT clump eid=%u", who, eid);
            if (!eid) return;
            BW::Thaw(eid, who);
            BW::Knock(eid, ue_wrap::FVector{0.f, 0.f, kKnockUpCmS}, who, "H");
        }});
        beatsI.push_back(Beat{126000, [who, &ids, g1] {
            ue_wrap::FVector at{};
            const uint32_t eid = ClumpNear(ids, g1, kSubjectNearCm, at);
            UE_LOGI("broom_drill: %s I SUBJECT clump eid=%u", who, eid);
            if (eid) BW::LookAt(at, who, "I");
        }});
        beatsI.push_back(Beat{128000, [who] { BW::Swing(who, "I", 1, 2500); }});
    }
    TrackThrough(ids, center, radius, beatsG, 117000, t0, who, "G");
    TrackThrough(ids, center, radius, beatsH, 125000, t0, who, "H");
    TrackThrough(ids, center, radius, beatsI, 137000, t0, who, "I");
    // The clump I pushed, and any a knock never found, get their hits back all the same.
    if (striking) BW::ThawAll(who);
}

}  // namespace

void RunBroomStrokeProbe() {
    const bool isClient = IsClientRole();
    const char* who = isClient ? "CLIENT" : "HOST";
    // The shared origin is the client's world coming up: the client announces it and the host learns
    // it as that slot going world-ready, a round trip apart.
    UE_LOGI("broom_drill: %s -- waiting for the client's world", who);
    bool ready = false;
    for (int i = 0; i < 1800 && !ready; ++i) {
        const auto& s = harness::session_runtime::Session();
        ready = isClient ? coop::net_pump::HasAnnouncedWorldReady() : s.IsSlotWorldReady(1);
        if (!ready) ::Sleep(100);
    }
    if (!ready) { UE_LOGW("broom_drill: %s never saw the client's world -- aborting", who); return; }
    UE_LOGI("broom_drill: %s ANCHOR tick=%lu -- settling 20s for the pile index and the save binds", who,
            static_cast<unsigned long>(::GetTickCount()));
    ::Sleep(20000);
    const DWORD t0 = ::GetTickCount();
    auto at = [t0](DWORD ms) { At(t0, ms); };
    BW::WatchNotifies(who, /*bodiesRunHere=*/!isClient);
    if (!BW::EquipBroom(who)) UE_LOGW("broom_drill: %s holds no broom -- its striking phases prove nothing", who);

    // ---- M: the button held, the host's and then the client's: the swing montage's cadence ----
    at(3000);
    if (!isClient) BW::Swing(who, "M", 1000, 3000);
    at(8000);
    if (isClient) BW::Swing(who, "M", 1000, 3000);

    // ---- A: the host sweeps a heap of ten chip piles with one stroke ----
    at(13000);
    ue_wrap::FVector seed{};
    RestSite rest;
    const bool haveHeap = !isClient && BW::PickChipPile(who, "A", seed);
    if (haveHeap) {
        BW::SpawnHeap(seed, kHeapExtra, kHeapRingCm, who);
        // The adoption scan names the new piles, and a pile with no id is neither tracked nor swept
        // under one; wait for the heap to be whole.
        for (DWORD waited = 0; waited < 14000 && BW::ChipPilesNear(seed, kHeapRingCm + 10.f) < kHeapExtra + 1;
             waited += 500)
            ::Sleep(500);
        BW::AimAt(seed, 0.f, who, "A");
        rest.seed = seed;
        rest.valid = BW::LocalBody(rest.stand, rest.yaw);
    }
    at(31000);
    // Both peers census around the striker's body, which stands beside the heap, so they follow the
    // same ids.
    ue_wrap::FVector centerA{};
    if (!BW::StrikerBody(!isClient, 0, centerA)) {
        UE_LOGW("broom_drill: %s INVALID -- the striker's body could not be placed", who);
        return;
    }
    std::vector<uint32_t> heapIds;
    BW::CensusPiles(centerA, kHeapCensusCm, who, "A", "before", &heapIds);
    std::vector<Beat> strikeA;
    if (haveHeap) strikeA.push_back(Beat{32000, [who] { BW::Swing(who, "A", 1, 2500); }});
    TrackThrough(heapIds, centerA, kHeapCensusCm, strikeA, 39000, t0, who, "A");
    BW::CensusPiles(centerA, kHeapCensusCm, who, "A", "late", nullptr);

    // ---- B: the client sweeps a lone chip pile, turned onto it from a facing its puppet keeps ----
    at(41000);
    ue_wrap::FVector subject{};
    const bool haveSubject =
        isClient && BW::PickChipPile(who, "B", subject, kSubjectAloneCm, &centerA, kSubjectFromStrikerCm);
    if (haveSubject) BW::AimAt(subject, kBodyTurnDeg, who, "B");
    at(44000);
    ue_wrap::FVector centerB = subject;
    if (!isClient && !BW::StrikerBody(false, 1, centerB)) {
        UE_LOGW("broom_drill: %s INVALID -- the striker's puppet could not be placed", who);
        return;
    }
    std::vector<uint32_t> idsB;
    BW::CensusPiles(centerB, kPileRadiusCm, who, "B", "before", &idsB);
    std::vector<Beat> strikeB;
    if (haveSubject) strikeB.push_back(Beat{45000, [who] { BW::Swing(who, "B", 1, 2500); }});
    TrackThrough(idsB, centerB, kPileRadiusCm, strikeB, 52000, t0, who, "B");
    BW::CensusPiles(centerB, kPileRadiusCm, who, "B", "late", nullptr);

    DispenserPhases(isClient, who, t0);
    RestPhases(isClient, who, t0, rest);
    UE_LOGI("broom_drill: done");
}

DWORD WINAPI BroomStrokeProbeThread(LPVOID) {
    RunBroomStrokeProbe();
    return 0;
}

}  // namespace harness::autotest
