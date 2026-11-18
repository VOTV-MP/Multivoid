// harness/autotest/cover_measure.h -- TEST-ONLY: the cover drill's measure arm (cover_drill=measure). Both peers log what
// the game announces about the signal desk's plates and a station's door, so the next increment's events are read off a
// run rather than assumed: every updPanels call with its caller and the plate arrays around it, the desk's overlaps with
// the plate prop's hold and lid state, the door timeline's end and openDoor, the unbolt's sound timer, and every
// station's place in the game mode's list. Lines tagged `[COVER-MEASURE]`. Observe only: no watch cancels a call.
#pragma once

namespace harness::autotest::cover_measure {

// Register the watches and wait, on the drill's own thread, until their names have resolved. Once per process.
void Start();

// One line per station: its index, its place, and the count the list reads -1 for. After the world is announced.
void Census();

}  // namespace harness::autotest::cover_measure
