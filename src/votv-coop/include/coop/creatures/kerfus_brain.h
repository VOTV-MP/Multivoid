// coop/creatures/kerfus_brain.h -- the plain kerfur (the Kerfus, p_kerfus_C) thinks on the host alone.
//
// The Kerfus is a prop with a brain: its tick drains or charges its energy and drives its wheel
// toward the next point of an invisible nav pawn, two timers re-path it and unstick it, and its
// server job walks to a broken server box and fixes it. Every peer loaded the same Kerfus from the
// save, so every peer ran that brain on its own copy: each followed its own player, drained its own
// energy and fixed servers only for itself. MTA runs a ped's AI on one syncer and shows the rest the
// syncer's state (reference/mtasa-blue/Server/mods/deathmatch/logic/CPedSync.cpp:106-139); here the
// host is the one syncer. On a client this lane refuses the brain's own bodies at the script-body
// gate, on p_kerfus_C and every colour variant, which inherits them or calls them as its parent's:
// the tick, the two timer events, the movement and server-job functions, the haunting, the laptop's
// jump and the cord events that set `charging`. The Kerfus's pose comes from the host's drive stream,
// its state from kerfus_state, and a client's own presses become kerfus_intent's requests. A last
// guard refuses a server box's fix() when a Kerfus is the caller, on a client, for a body that began
// before its watch went live. Engine access through ue_wrap; game thread.

#pragma once

namespace coop::net { class Session; }

namespace coop::kerfus_brain {

// Register the gate watches (retried until the gate takes them) and cache the session. Idempotent.
// Game thread.
void Install(coop::net::Session* session);

// Drive the pending name resolution and say once when every watch is live. Game thread.
void Tick();

// Session end: the per-session "said" latches reset and the refusal counts are logged.
void OnDisconnect();

// How many brain bodies this client has refused this session (the kerfus drill's proof). Game thread.
unsigned long long RefusedCount();

}  // namespace coop::kerfus_brain
