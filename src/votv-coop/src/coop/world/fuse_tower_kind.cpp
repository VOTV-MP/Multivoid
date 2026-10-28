// coop/world/fuse_tower_kind.cpp -- see coop/world/fuse_tower_kind.h.
//
// The radio tower's facts (bp_cpp radiotower_C, line numbers are its pseudo-C++): `scramble` (:1496) blows the first
// good fuse and broadcasts radioTowerBroken(tower, true), or else breaks the puzzle and broadcasts (tower, false);
// the reset lever's timeline event re-rolls through it (:2339); the confirm lever's forward end judges the puzzle
// through tryToFix (:2165); completeMinigame lights every cell and judges (:2185); actionOptionIndex and
// playerUsedOn are ignored while `animation` (:510, :655).

#include "coop/world/fuse_tower_kind.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/devices/coord_tower.h"
#include "ue_wrap/devices/radio_tower.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/game_rules.h"  // the difficulty an insert reads

#include <algorithm>
#include <cstdint>

namespace coop::fuse_tower_kind {
namespace {

namespace CT = ue_wrap::coord_tower;
namespace RT = ue_wrap::radio_tower;
using coop::net::FuseTowerRow;
using Edges = coop::fuse_tower_rows::Edges;

static_assert(CT::kMaxFuses <= coop::net::kFuseTowerMaxFuses && RT::kMaxFuses <= coop::net::kFuseTowerMaxFuses,
              "a row holds eight fuses");
static_assert(CT::kMaxLights <= coop::net::kFuseTowerMaxLights && RT::kMaxCells <= coop::net::kFuseTowerMaxLights,
              "a row holds the lights of the larger puzzle");
static_assert(coop::net::kFuseTowerMaxLights < 32, "a row's lights are one 32-bit mask");

// The four bodies each kind's gate watches. The coordinate dish's roll is one function, and its lever's judgement
// the timeline's end; the radio tower's roll is `scramble`, which its reset lever, its decay timer and a blast all
// reach, and its judgement the same timeline end.
UE_FACT_FN(kRowCoordRoll, "fuse_tower_kind", L"coordRadarDish_C", L"Scramble Radar Dish", Declared);
UE_FACT_FN(kRowCoordJudge, "fuse_tower_kind", L"coordRadarDish_C", L"leverTL__FinishedFunc", Declared);
UE_FACT_FN(kRowCoordUse, "fuse_tower_kind", L"coordRadarDish_C", L"actionOptionIndex", Declared);
UE_FACT_FN(kRowCoordInsert, "fuse_tower_kind", L"coordRadarDish_C", L"playerUsedOn", Declared);
UE_FACT_FN(kRowRadioRoll, "fuse_tower_kind", L"radiotower_C", L"scramble", Declared);
UE_FACT_FN(kRowRadioJudge, "fuse_tower_kind", L"radiotower_C", L"TL_lever__FinishedFunc", Declared);
UE_FACT_FN(kRowRadioUse, "fuse_tower_kind", L"radiotower_C", L"actionOptionIndex", Declared);
UE_FACT_FN(kRowRadioInsert, "fuse_tower_kind", L"radiotower_C", L"playerUsedOn", Declared);
// The radio tower generates its puzzle and its fuses at construction and at BeginPlay (radiotower.cpp:1061,
// :488-493); a client follows each with the host's kept row.
UE_FACT_FN(kRowRadioGenPuzzle, "fuse_tower_kind", L"radiotower_C", L"genPuzzle", Declared);
UE_FACT_FN(kRowRadioGenFuses, "fuse_tower_kind", L"radiotower_C", L"genFuses", Declared);

const WatchRows kWatches[kKinds] = {
    { &kRowCoordRoll, &kRowCoordJudge, &kRowCoordUse, &kRowCoordInsert, nullptr, nullptr },
    { &kRowRadioRoll, &kRowRadioJudge, &kRowRadioUse, &kRowRadioInsert, &kRowRadioGenPuzzle, &kRowRadioGenFuses },
};

bool LightBit(uint32_t lights, int i) { return ((lights >> i) & 1u) != 0; }

// ---- the coordinate towers -----------------------------------------------------------------------------------

CT::LookAt CoordPart(uint8_t op, int32_t index) {
    switch (op) {
    case coop::net::kFuseTowerOpButton:  return {CT::Part::Button, index};
    case coop::net::kFuseTowerOpLever:   return {CT::Part::Lever, -1};
    case coop::net::kFuseTowerOpRetract: return {CT::Part::Retract, -1};
    case coop::net::kFuseTowerOpPull:
    case coop::net::kFuseTowerOpInsert:  return {CT::Part::Fuse, index};
    default:                              return {CT::Part::None, -1};
    }
}

bool CoordReadRow(void* tower, FuseTowerRow& w) {
    CT::State st;
    if (!CT::Read(tower, st)) return false;
    w = FuseTowerRow{};
    w.id = st.id;
    w.kind = coop::net::kFuseTowerKindCoordinate;
    w.flags = static_cast<uint8_t>((st.isBroken ? coop::net::kFuseTowerBroken : 0) |
                                   (st.opened ? coop::net::kFuseTowerOpened : 0) |
                                   (st.isAnim ? coop::net::kFuseTowerAnim : 0) |
                                   (st.leverMoving ? coop::net::kFuseTowerLeverMoving : 0) |
                                   (st.leverUp ? coop::net::kFuseTowerLeverUp : 0));
    w.fuseCount = st.fuseCount;
    for (int i = 0; i < st.fuseCount; ++i) w.fuses[i] = st.fuses[i];
    w.lightCount = st.lightCount;
    for (int i = 0; i < st.lightCount; ++i)
        if (st.lights[i]) w.lights |= 1u << i;
    return true;
}

void CoordApply(void* tower, const FuseTowerRow& c, const ApplyCtx& x) {
    CT::State l;
    if (!CT::Read(tower, l)) return;
    Edges& edges = *x.edges;
    const bool live = x.live;
    const bool broken = (c.flags & coop::net::kFuseTowerBroken) != 0;
    bool lights[CT::kMaxLights] = {};
    bool lightsDiffer = false;
    if (c.lightCount == l.lightCount) {
        for (int i = 0; i < c.lightCount; ++i) {
            lights[i] = LightBit(c.lights, i);
            lightsDiffer = lightsDiffer || lights[i] != l.lights[i];
        }
    } else if (!*x.sizeWarned) {
        // Every peer plays under the host's rules (ue_wrap/world/game_rules.h), which size the puzzle.
        *x.sizeWarned = true;
        UE_LOGW("fuse_tower: tower %d's puzzle has %u lights here and %u on the host -- its lights are not applied",
                c.id, l.lightCount, c.lightCount);
    }
    if (lightsDiffer) CT::WriteLights(tower, lights, c.lightCount);
    if (l.isBroken && !broken) {
        if (live) {
            CT::SolvePuzzle(tower);  // the repair: every light lit, painted, its success sound, isBroken off, painted
        } else {
            // The tower's first row is state, not an act: solvePuzzle's body without its sound (@2098..@2105); the
            // lights are the row's, written above.
            CT::WriteBroken(tower, false);
            CT::UpdPuzzle(tower);
            CT::UpdBroken(tower);
        }
        ++edges.repairs;
        UE_LOGI("fuse_tower: tower %d repaired as the host's", c.id);
    } else if (!l.isBroken && broken) {
        CT::WriteBroken(tower, true);
        CT::UpdPuzzle(tower);
        CT::UpdBroken(tower);
        ++edges.breaks;
        UE_LOGI("fuse_tower: tower %d broke as the host's", c.id);
    } else if (lightsDiffer) {
        CT::UpdPuzzle(tower);
        if (live) CT::Play(tower, CT::Sound::Click);  // a press, the host's route's own click
        ++edges.presses;
    }
    bool fusesDiffer = false, pulled = false, inserted = false;
    if (c.fuseCount == l.fuseCount) {
        for (int i = 0; i < c.fuseCount; ++i) {
            if (c.fuses[i] == l.fuses[i]) continue;
            pulled = pulled || (l.fuses[i] == 2 && c.fuses[i] == 0);
            inserted = inserted || (l.fuses[i] == 0 && c.fuses[i] == 1);
            CT::WriteFuse(tower, i, c.fuses[i]);
            fusesDiffer = true;
        }
    }
    if (fusesDiffer) {
        CT::UpdFuses(tower);
        if (live && pulled) CT::Play(tower, CT::Sound::FusePulled);
        if (live && inserted) CT::Play(tower, CT::Sound::FuseInserted);
        edges.pulls += pulled ? 1 : 0;
        edges.inserts += inserted ? 1 : 0;
    }
    const bool up = (c.flags & coop::net::kFuseTowerLeverUp) != 0;
    if (up != l.leverUp) {
        // A lever that goes down on a tower broken before and after is the judgement's fail branch: its other
        // caller, a scramble, breaks a whole tower (Scramble Radar Dish's end, and loadData, before any join).
        if (!up && l.isBroken && broken) {
            if (live) CT::Play(tower, CT::Sound::Fail);
            ++edges.fails;
        }
        CT::MoveLever(tower, up);
        ++edges.levers;
    }
    // The lever's end, which judges the puzzle, is refused here, and it is what clears the flag: the host's.
    CT::WriteLeverMoving(tower, (c.flags & coop::net::kFuseTowerLeverMoving) != 0);
}

Look CoordReadUse(void* tower, uint8_t& op, int32_t& index) {
    CT::LookAt at;
    if (!CT::ReadLookAt(tower, at)) return Look::None;
    switch (at.part) {
    case CT::Part::Button:
        op = coop::net::kFuseTowerOpButton;
        index = at.index;
        return Look::Send;
    case CT::Part::Lever:
        op = coop::net::kFuseTowerOpLever;
        index = -1;
        return Look::Send;
    case CT::Part::Retract:
        op = coop::net::kFuseTowerOpRetract;
        index = -1;
        return Look::Send;
    case CT::Part::Fuse: {
        CT::State st;
        if (!CT::Read(tower, st) || at.index < 0 || at.index >= st.fuseCount || st.fuses[at.index] != 2)
            return Look::None;
        op = coop::net::kFuseTowerOpPull;
        index = at.index;
        return Look::Pull;
    }
    default:
        return Look::None;
    }
}

// ---- the radio tower -----------------------------------------------------------------------------------------

RT::LookAt RadioPart(uint8_t op, int32_t index) {
    switch (op) {
    case coop::net::kFuseTowerOpButton: return {RT::Part::Button, index};
    case coop::net::kFuseTowerOpLever:  return {RT::Part::Confirm, -1};
    case coop::net::kFuseTowerOpReset:  return {RT::Part::Reset, -1};
    case coop::net::kFuseTowerOpPull:
    case coop::net::kFuseTowerOpInsert: return {RT::Part::Fuse, index};
    default:                             return {RT::Part::None, -1};  // the radio tower has no panel
    }
}

bool RadioReadRow(void* tower, FuseTowerRow& w) {
    RT::State st;
    if (!RT::Read(tower, st)) return false;
    w = FuseTowerRow{};
    w.id = 0;
    w.kind = coop::net::kFuseTowerKindRadio;
    // The confirm lever's bits are its timeline's: running, and running forward. The reset lever plays one way. The
    // tower's `animation` is its own flag: the confirm lever's forward end clears it before the judgement, so it is
    // false while a failed lever runs back.
    w.flags = static_cast<uint8_t>((st.isBroken ? coop::net::kFuseTowerBroken : 0) |
                                   (st.leverPlaying ? coop::net::kFuseTowerLeverMoving : 0) |
                                   (st.leverPlaying && st.leverForward ? coop::net::kFuseTowerLeverUp : 0) |
                                   (st.resetPlaying ? coop::net::kFuseTowerResetMoving : 0) |
                                   (st.animation ? coop::net::kFuseTowerAnimating : 0));
    w.fuseCount = st.fuseCount;
    for (int i = 0; i < st.fuseCount; ++i) w.fuses[i] = st.fuses[i];
    w.lightCount = st.cellCount;
    for (int i = 0; i < st.cellCount; ++i)
        if (st.cells[i]) w.lights |= 1u << i;
    return true;
}

// The tower's own graph is not run for the host's act: its writes, painters, sounds and lever timelines are made
// here, and the alert its scramble broadcasts is raised on the console by hand. The scramble itself, the lever's
// judgement and the reset event's roll are refused at the gate, so the host's rows are the only author.
void RadioApply(void* tower, const FuseTowerRow& c, const ApplyCtx& x) {
    RT::State l;
    if (!RT::Read(tower, l)) return;
    Edges& edges = *x.edges;
    const bool live = x.live;
    const bool broken = (c.flags & coop::net::kFuseTowerBroken) != 0;
    bool cells[RT::kMaxCells] = {};
    bool cellsDiffer = false;
    if (c.lightCount == l.cellCount) {
        for (int i = 0; i < c.lightCount; ++i) {
            cells[i] = LightBit(c.lights, i);
            cellsDiffer = cellsDiffer || cells[i] != l.cells[i];
        }
    } else if (!*x.sizeWarned) {
        *x.sizeWarned = true;
        UE_LOGW("fuse_tower: the radio tower's puzzle has %u cells here and %u on the host -- its cells are not "
                "applied", l.cellCount, c.lightCount);
    }
    if (cellsDiffer) RT::WriteCells(tower, cells, c.lightCount);
    if (l.isBroken && !broken) {
        if (live) {
            RT::CompleteMinigame(tower);  // every cell lit, painted, tryToFix: isBroken off, its success sound
        } else {
            RT::SetBroken(tower, false, false);  // the tower's first row is state, not an act: no sound
            RT::UpdPuzzle(tower);
        }
        ++edges.repairs;
        UE_LOGI("fuse_tower: the radio tower repaired as the host's");
    } else if (!l.isBroken && broken) {
        RT::SetBroken(tower, true, false);
        RT::UpdPuzzle(tower);
        if (live) RT::Alert(tower, false);  // "is down!", what scramble broadcasts on a break
        ++edges.breaks;
        UE_LOGI("fuse_tower: the radio tower broke as the host's");
    } else if (cellsDiffer) {
        RT::UpdPuzzle(tower);
        if (live) RT::Play(tower, RT::Sound::Click);
        ++edges.presses;
    }
    bool fusesDiffer = false, pulled = false, inserted = false, blown = false;
    if (c.fuseCount == l.fuseCount) {
        for (int i = 0; i < c.fuseCount; ++i) {
            if (c.fuses[i] == l.fuses[i]) continue;
            pulled = pulled || (l.fuses[i] == 2 && c.fuses[i] == 0);
            inserted = inserted || (l.fuses[i] == 0 && c.fuses[i] == 1);
            blown = blown || (l.fuses[i] == 1 && c.fuses[i] == 2);
            RT::WriteFuse(tower, i, c.fuses[i]);
            fusesDiffer = true;
        }
    }
    if (fusesDiffer) {
        RT::UpdFuses(tower);
        if (live && pulled) RT::Play(tower, RT::Sound::FusePulled);
        if (live && inserted) RT::Play(tower, RT::Sound::FuseInserted);
        if (live && blown) RT::Alert(tower, true);  // "has been damaged!", what scramble broadcasts on a fuse blow
        edges.pulls += pulled ? 1 : 0;
        edges.inserts += inserted ? 1 : 0;
    }
    // Each lever is started once, when the host's row first has it running (or running the other way); its
    // timeline then ends by itself, and its end event, which judges or clears `animation`, is refused here.
    const bool leverMoving = (c.flags & coop::net::kFuseTowerLeverMoving) != 0;
    const bool leverUp = (c.flags & coop::net::kFuseTowerLeverUp) != 0;
    const bool resetMoving = (c.flags & coop::net::kFuseTowerResetMoving) != 0;
    const bool wasMoving = (x.prevFlags & coop::net::kFuseTowerLeverMoving) != 0;
    const bool wasUp = (x.prevFlags & coop::net::kFuseTowerLeverUp) != 0;
    const bool resetWas = (x.prevFlags & coop::net::kFuseTowerResetMoving) != 0;
    if (leverMoving && (!wasMoving || wasUp != leverUp)) {
        if (leverUp) {
            if (live) RT::Play(tower, RT::Sound::Lever);
        } else {
            // The lever heading back is the judgement's fail branch (tryToFix's resetLever).
            if (live) RT::Play(tower, RT::Sound::Fail);
            ++edges.fails;
        }
        if (live) RT::PlayLever(tower, RT::Lever::Confirm, leverUp);
        ++edges.levers;
    }
    // The reset lever's timeline event plays the scramble and click sounds, which a tower's first row must not
    // make: a lever already running when the tower first reads is left at rest, and `animation` below clears with
    // the host's next row.
    if (resetMoving && !resetWas && live) {
        RT::Play(tower, RT::Sound::Lever);
        RT::PlayLever(tower, RT::Lever::Reset, true);
        ++edges.levers;
    }
    RT::WriteAnimation(tower, (c.flags & coop::net::kFuseTowerAnimating) != 0);
}

Look RadioReadUse(void* tower, uint8_t& op, int32_t& index) {
    RT::LookAt at;
    RT::State st;
    if (!RT::ReadLookAt(tower, at) || !RT::Read(tower, st) || st.animation) return Look::None;
    switch (at.part) {
    case RT::Part::Button:
    case RT::Part::Confirm:
    case RT::Part::Reset:
        // A whole tower answers a press with its own hint and does nothing else: the player's own game runs it.
        if (!st.isBroken) return Look::None;
        op = at.part == RT::Part::Button ? coop::net::kFuseTowerOpButton
           : at.part == RT::Part::Confirm ? coop::net::kFuseTowerOpLever : coop::net::kFuseTowerOpReset;
        index = at.part == RT::Part::Button ? at.index : -1;
        return Look::Send;
    case RT::Part::Fuse:
        if (at.index < 0 || at.index >= st.fuseCount || st.fuses[at.index] != 2) return Look::None;
        op = coop::net::kFuseTowerOpPull;
        index = at.index;
        return Look::Pull;
    default:
        return Look::None;
    }
}

}  // namespace

bool Resolved(Kind kind) { return kind == Kind::Radio ? RT::Instance() != nullptr : CT::EnsureResolved(); }

int32_t ReadAll(Tower* out, int32_t cap) {
    int32_t n = 0;
    if (cap > 0)
        if (void* radio = RT::Instance()) out[n++] = {Kind::Radio, 0, radio};
    void* towers[coop::net::kFuseTowers];
    const int32_t room = std::min<int32_t>(cap - n, coop::net::kFuseTowers);
    const int32_t c = room > 0 ? CT::ReadAll(towers, room) : 0;
    if (c < 0) return n > 0 ? n : -1;
    for (int32_t i = 0; i < c; ++i) out[n++] = {Kind::Coordinate, CT::IdOf(towers[i]), towers[i]};
    return n;
}

void* Find(Kind kind, int32_t id) {
    if (kind == Kind::Radio) return id == 0 ? RT::Instance() : nullptr;
    void* towers[coop::net::kFuseTowers];
    const int32_t n = CT::ReadAll(towers, coop::net::kFuseTowers);
    for (int32_t i = 0; i < n; ++i)
        if (CT::IdOf(towers[i]) == id) return towers[i];
    return nullptr;
}

bool Identify(void* obj, Kind& kind, int32_t& id) {
    if (const int32_t i = CT::IdOf(obj); i >= 0) {
        kind = Kind::Coordinate;
        id = i;
        return true;
    }
    if (!RT::IsTower(obj)) return false;
    kind = Kind::Radio;
    id = 0;
    return true;
}

bool ReadView(Kind kind, void* tower, View& out) {
    out = View{};
    if (kind == Kind::Radio) {
        RT::State st;
        if (!RT::Read(tower, st)) return false;
        out.broken = st.isBroken;
        out.fuseCount = st.fuseCount;
        for (int i = 0; i < st.fuseCount; ++i) out.fuses[i] = st.fuses[i];
        out.lightCount = st.cellCount;
        return true;
    }
    CT::State st;
    if (!CT::Read(tower, st)) return false;
    out.broken = st.isBroken;
    out.fuseCount = st.fuseCount;
    for (int i = 0; i < st.fuseCount; ++i) out.fuses[i] = st.fuses[i];
    out.lightCount = st.lightCount;
    return true;
}

bool ReadRow(const Tower& tower, FuseTowerRow& row) {
    return tower.kind == Kind::Radio ? RadioReadRow(tower.obj, row) : CoordReadRow(tower.obj, row);
}

void ApplyRow(Kind kind, void* tower, const FuseTowerRow& row, const ApplyCtx& ctx) {
    if (kind == Kind::Radio) RadioApply(tower, row, ctx);
    else CoordApply(tower, row, ctx);
}

bool ConvergePanel(Kind kind, void* tower, void* player, const FuseTowerRow& row, bool& opens, bool& ran) {
    if (kind != Kind::Coordinate) return false;
    CT::State l;
    if (!CT::Read(tower, l) || l.isAnim) return false;
    const bool open = (row.flags & coop::net::kFuseTowerOpened) != 0;
    const bool target = (row.flags & coop::net::kFuseTowerAnim) ? !open : open;
    if (l.opened == target) return false;
    opens = target;
    ran = CT::Use(tower, player, CT::LookAt{CT::Part::Retract, -1});
    return true;
}

bool PartLocation(Kind kind, void* tower, uint8_t op, int32_t index, ue_wrap::FVector& out) {
    if (kind == Kind::Radio) return RT::PartLocation(tower, RadioPart(op, index), out);
    return CT::PartLocation(tower, CoordPart(op, index), out);
}

bool Use(Kind kind, void* tower, void* player, uint8_t op, int32_t index) {
    if (kind == Kind::Radio) return RT::Use(tower, player, RadioPart(op, index));
    return CT::Use(tower, player, CoordPart(op, index));
}

Look ReadUse(Kind kind, void* tower, uint8_t& op, int32_t& index) {
    return kind == Kind::Radio ? RadioReadUse(tower, op, index) : CoordReadUse(tower, op, index);
}

bool ReadFuseLook(Kind kind, void* tower, int32_t& slot) {
    return kind == Kind::Radio ? RT::ReadFuseLook(tower, slot) : CT::ReadFuseLook(tower, slot);
}

const char* CanUse(Kind kind, void* tower, uint8_t op) {
    if (kind == Kind::Radio) {
        // radiotower actionOptionIndex :510, :4027-:4259: ignored while a lever runs; a whole tower answers a press
        // with a hint and does nothing.
        RT::State st;
        if (!RT::Read(tower, st)) return "the tower does not read";
        if (st.animation) return "its lever is moving";
        if (!st.isBroken) return "the tower is not broken";
        return nullptr;
    }
    // coordRadarDish actionOptionIndex @2885..@3958: nothing while the panel moves; the retract is then free; the
    // rest needs the panel open, the lever at rest and the tower broken.
    CT::State st;
    if (!CT::Read(tower, st)) return "the tower does not read";
    if (st.isAnim) return "its panel is moving";
    if (op == coop::net::kFuseTowerOpRetract) return nullptr;
    if (!st.opened) return "its panel is closed";
    if (st.leverMoving) return "its lever is moving";
    if (!st.isBroken) return "the tower is not broken";
    return nullptr;
}

const char* CanPull(Kind kind, void* tower, const View& view, int32_t slot) {
    if (kind == Kind::Radio) {
        // The pull's own gates (radiotower actionOptionIndex :510, :524-540), on the host's copy: no lever runs.
        RT::State st;
        if (!RT::Read(tower, st)) return "the tower does not read";
        if (st.animation) return "its lever is moving";
    } else {
        // The pull's own gates (coordRadarDish actionOptionIndex @3586-@3782), on the host's copy.
        CT::State st;
        if (!CT::Read(tower, st)) return "the tower does not read";
        if (!st.opened) return "its panel is closed";
        if (st.isAnim) return "its panel is moving";
        if (st.leverMoving) return "its lever is moving";
    }
    if (slot < 0 || slot >= view.fuseCount || view.fuses[slot] != 2) return "that fuse is not blown on the host";
    return nullptr;
}

const char* CanInsert(Kind kind, void* tower, const View& view, int32_t slot) {
    // The radio tower's insert is ignored while a lever runs (radiotower playerUsedOn :655); the coordinate tower's
    // has no such test (coordRadarDish :616-665), only the held class and the slot.
    if (kind == Kind::Radio) {
        RT::State st;
        if (!RT::Read(tower, st)) return "the tower does not read";
        if (st.animation) return "its lever is moving";
    }
    if (slot < 0 || slot >= view.fuseCount || view.fuses[slot] != 0) return "that fuse slot is not empty on the host";
    return nullptr;
}

void TakePull(Kind kind, void* tower, int32_t slot) {
    // The pull's writes on the host's copy; its fuse is already in its puller's hand.
    if (kind == Kind::Radio) {
        RT::WriteFuse(tower, slot, 0);
        RT::UpdFuses(tower);
        RT::Play(tower, RT::Sound::FusePulled);
        return;
    }
    CT::WriteFuse(tower, slot, 0);
    CT::UpdFuses(tower);
    CT::Play(tower, CT::Sound::FusePulled);
}

// The insert's own branch on the difficulty (coordRadarDish playerUsedOn @4823, radiotower playerUsedOn :696):
// difficulty 0 and 1 keep the fuse and solve, anything above spends it.
bool EasyInsert() {
    const int difficulty = ue_wrap::game_rules::ReadDifficulty();
    return difficulty >= 0 && difficulty <= 1;
}

void TakeInsert(Kind kind, void* tower, int32_t slot) {
    // The insert's writes on the host's copy; its fuse is already spent (playerUsedOn @5129..@5317, :681-701).
    if (kind == Kind::Radio) {
        RT::WriteFuse(tower, slot, 1);
        RT::Play(tower, RT::Sound::FuseInserted);
        RT::UpdFuses(tower);
        return;
    }
    CT::WriteFuse(tower, slot, 1);
    CT::Play(tower, CT::Sound::FuseInserted);
    CT::UpdFuses(tower);
}

void TakeEasyInsert(Kind kind, void* tower, int32_t slot) {
    if (kind == Kind::Radio) {
        // radiotower playerUsedOn :681-696: the slot good, its sound, updFuses, completeMinigame; the fuse stays.
        RT::WriteFuse(tower, slot, 1);
        RT::Play(tower, RT::Sound::FuseInserted);
        RT::UpdFuses(tower);
        RT::CompleteMinigame(tower);
        return;
    }
    CT::SolvePuzzle(tower);  // coordRadarDish playerUsedOn @5354: solvePuzzle and nothing else
}

bool IsFuse(void* actor) { return CT::IsFuse(actor); }
void* SpawnFuse(const ue_wrap::FVector& at) { return CT::SpawnFuse(at); }
bool IsPulledFuse(void* actor) { return CT::IsPulledFuse(actor); }

const WatchRows& WatchesOf(Kind kind) { return kWatches[static_cast<int>(kind)]; }

}  // namespace coop::fuse_tower_kind
