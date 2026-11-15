// coop/world/bed_event_gate.h -- the bed a bedEvent moves is the host's alone; each player's wake teleport is its own.
//
// bedEvent replays on every peer (coop/world/event_fire_policy): each machine spawns its own bedEvent_C, which on that
// machine's wake moves gamemode.bed to its own transform, drops the bed's static and frozen flags and teleports the local
// player beside it. The bed is a shared prop and the prop lanes carry no static move, so every machine moving its own
// copy would leave the copies apart. One watch on bedEvent_C.woken (a Declared row, live once the class loads):
//   - a client: gamemode.bed is cleared for the length of the body and written back after it, so the body's bed moves
//     and prop-property call land on nothing while the teleport of the local player runs; its copy of the bed moves with
//     the host's;
//   - the host: after the body, the bed it moved is handed to the prop drive (prop_drive_host::Coast), which streams the
//     new pose and the end edge's physics flags to every peer's copy.
// Game thread.

#pragma once

namespace coop::net { class Session; }

namespace coop::bed_event_gate {

// Register the watch, once for the process, and keep `session` for the role test. Idempotent. A watch that did not
// register is said in one ERROR line. Any thread.
void Install(coop::net::Session* session);

}  // namespace coop::bed_event_gate
