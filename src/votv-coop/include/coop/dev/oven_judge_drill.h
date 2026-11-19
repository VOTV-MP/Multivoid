// coop/dev/oven_judge_drill.h -- [dev] the host's judge of a client's oven steps and repair
// (coop/interactables/oven_repair_judge), and the client's walk to the oven's switch box.
//
// Arms of `oven_progress_drill`, run on the host and on the clients of one session; client 1 acts, the host reads:
//   repair    -- client 1 is walked to the box by the director, writes the screws and the lid on its repair screen
//                (RaiseProgress, not the screen's click handlers), then calls the oven's fix() directly. The host's judge
//                grants it and every peer reads the oven fixed; the client's own fix() never runs.
//   again     -- as repair, then the client asks the repair a second time: the host answers Already, grants once.
//   reach     -- client 1 stays far from the box and sends a raw screw bit; the host's reach counter moves and its record
//                never holds the bit; a sentinel bit the host then raises must reach client 2 (`--clients 2`) alone.
//   order     -- client 1, at the box, sends a raw lid bit with no screw in: the order counter moves, the record lacks it.
//   fixearly  -- client 1, at the box, sends a raw repair with no lid in the record: the order counter moves, no repair.
//   burst     -- client 1, far from the box, sends 12 raw screw messages at once: the host's queue holds eight, so the
//                bucket counter moves. Not covered by any arm: the host's own open screen quit() and the no-pose wait.

#pragma once

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::dev::oven_judge_drill {

// Every verdict is one line, `[OVEN-DRILL]` with the peer's tag and `PASS`, `FAIL` or `INVALID`, decided at the event
// that settles it: a counter moving on the arm's own reason, the refused bit reaching the host's record (FAIL), the
// client's own fix() running (FAIL). A FAIL is told to the other peer in the lobby chat so neither idles to its bound; a
// bound only says INVALID. Red arms: `oven_judge_off` on the host (reach, order and fixearly FAIL at the raw bit reaching
// the record, or the raw repair fixing the host's oven) and `oven_repair_hold_off` on the client (repair FAILs at its own
// fix() running).

// True for the six arms above.
bool IsJudgeArm();

// The arms' pass on each peer. A single read when off. Game thread.
void Tick(coop::net::Session* session);

// Where the walk to the box stands. Failed says why through the drill's INVALID line, once.
enum class Walk { Going, Arrived, Failed };

// The client's director walk to within reach of `oven`'s switch box, started on the first call and polled by each: the
// bot director moves the player, never a teleport. Game thread.
Walk WalkToBox(void* oven);

// Back to the first step. Net disconnect.
void OnDisconnect();

}  // namespace coop::dev::oven_judge_drill
