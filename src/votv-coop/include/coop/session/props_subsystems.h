// coop/session/props_subsystems.h -- the props lanes' slice of the sync-module fan-out.
//
// The clean-up calls of every lane whose header sits under coop/props/: a leaver's snapshot drain,
// half assemblies and escrowed records, and each lane's session-end drop. subsystems.cpp calls the
// two entries from the one place its own list reached the first of them, so the props calls keep
// their order among themselves. The calls whose order against the rest of the fan-out is written
// down (the trash mirrors before the generic mirror drain, the script gate's release before the
// lanes below it, the probes' verdicts before the prop seams reset, the park and the rider hold
// last) and the calls that hand a value back stay in subsystems.cpp.

#pragma once

namespace coop::subsystems::props {

// A peer left: what each props lane held for its slot is dropped. Game thread only.
void OnPeerLeft(int slot);

// The session's end: each props lane's state, tallies and latches are dropped, in the fan-out's
// order. Game thread only.
void OnDisconnect();

}  // namespace coop::subsystems::props
