// coop/dev/tower_drill_detail.h -- what the tower drill's files share: the arm the config row names, the clock, a
// distance, the host's last row of a tower, and the arm families' entry points (the coordinate arms in
// tower_drill_coord.cpp, the radio arms in tower_drill_radio.cpp). Impl-private: nothing outside coop/dev includes
// it. Game thread.

#pragma once

#include "coop/net/protocol.h"

#include "ue_wrap/core/types.h"

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::dev::tower_drill::detail {

// Every value of the config row `tower_drill`: the coordinate towers' run and join and their controls (red,
// joinred: the client's gates open and no rows applied), then the radio tower's: the walk that measures the reach
// (radioreach), the repair (radio) and its control (radiored), the reset lever (radioreset), and the repair of a
// tower the host broke before the client joined (radiojoin) with its control (radiojoinred), the reset lever's control
// (radioresetred), and a fuse pulled from the host's tower and a good one inserted (radiofuse) with its control (radiofusered).
enum class Arm : uint8_t {
    Off, Run, Red, Join, JoinRed, RadioReach, Radio, RadioRed, RadioReset, RadioJoin, RadioJoinRed, RadioResetRed,
    RadioFuse, RadioFuseRed
};

// The arm the row names, read once.
Arm ArmOf();

inline bool IsRadio(Arm a) { return a >= Arm::RadioReach; }
// The coordinate arms' two shapes, each with its control.
inline bool IsRun(Arm a) { return a == Arm::Run || a == Arm::Red; }
inline bool IsJoin(Arm a) { return a == Arm::Join || a == Arm::JoinRed; }
// The radio arms whose host breaks the tower before any client stands in its world.
inline bool IsRadioJoin(Arm a) { return a == Arm::RadioJoin || a == Arm::RadioJoinRed; }
// The reset lever's arm and the fuse arm, each with its control.
inline bool IsRadioReset(Arm a) { return a == Arm::RadioReset || a == Arm::RadioResetRed; }
inline bool IsRadioFuse(Arm a) { return a == Arm::RadioFuse || a == Arm::RadioFuseRed; }

uint64_t NowMs();
float Dist(const ue_wrap::FVector& a, const ue_wrap::FVector& b);

inline bool RowBroken(const coop::net::FuseTowerRow& w) { return (w.flags & coop::net::kFuseTowerBroken) != 0; }

// The host's last row of the tower (kind, id), as this client holds it (false before any rows came).
bool FindRow(uint8_t kind, int32_t id, coop::net::FuseTowerRow& out);

// The coordinate arms (tower_drill_coord.cpp): the host's break and its own player's repair, the client's reads.
void CoordHostTick(coop::net::Session& s);
void CoordClientTick();
void CoordReset();

// The radio arms (tower_drill_radio.cpp, and tower_drill_radio_arms.cpp for the reset lever's; what the two share is
// tower_drill_radio_steps.h): the host's break on its own roll, the client's walk, climb, presses and levers.
void RadioHostTick(coop::net::Session& s);
void RadioClientTick();
void RadioReset();

}  // namespace coop::dev::tower_drill::detail
