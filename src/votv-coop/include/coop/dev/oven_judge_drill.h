// coop/dev/oven_judge_drill.h -- [dev] the host's judge of a client's oven steps and repair
// (coop/interactables/oven_repair_judge), and the client's walk to the oven's switch box.
//
// Arms of `oven_progress_drill`, run on the host and on the clients of one session; client 1 acts, the host reads:
//   repair    -- client 1 is walked to the box by the director, takes the screws and the lid on its repair screen as the
//                steps raise them, then runs the oven's fix(). The host's judge grants it and every peer reads the oven
//                fixed; the client's own fix() never runs (its `fixed` is true only after the host's grant).
//   reach     -- client 1 stays where it spawned, far from the box, and sends a raw screw bit. The host's reach refusal
//                counter moves and its record never holds the bit; the host then raises a bit of its own as a sentinel on
//                the same ordered lane, and client 2 (run with `--clients 2`) must read the sentinel without client 1's bit.
//   order     -- client 1 is walked to the box and sends a raw lid bit with no screw in: the host's order refusal counter
//                moves and its record never holds the lid.
//   fixearly  -- client 1 is walked to the box and sends a raw repair with no lid in the host's record: the order counter
//                moves and the host's oven is not fixed.

#pragma once

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::dev::oven_judge_drill {

// Every verdict is one line, `[OVEN-DRILL]` with the peer's tag and `PASS`, `FAIL` or `INVALID`, decided at the event
// that settles it: a counter moving on the arm's own reason, the refused bit reaching the host's record (FAIL), the
// client's own fix() running (FAIL). A bound only says INVALID. Red arms: `oven_judge_off` on the host (reach, order and
// fixearly FAIL at the raw bit reaching the record, or the raw repair fixing the host's oven) and `oven_repair_hold_off`
// on the client (repair FAILs at its own fix() running).

// True for the four arms above.
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
