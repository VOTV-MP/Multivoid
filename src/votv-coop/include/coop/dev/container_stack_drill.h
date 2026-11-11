// coop/dev/container_stack_drill.h -- [dev] a client's take of part of a stack, and its merge of two stacks, in a shared
// world container, on the 0.10 inventory screen's own widgets (coop/props/container_act_stack: PRE takeFromStack, PRE and
// POST addToStack, POST beginDrag). Both peers run it with the same value (ini container_stack_drill, env
// VOTVCOOP_CONTAINER_STACK_DRILL=1); lines are tagged [CSTACK-DRILL]. It MUTATES the container it uses.
//
//   HOST   -- once a client is world-ready, picks a world container, clears it and seeds it with three records of one
//             item: a stack of 6 and two stacks of 3, says the container's element id in the lobby chat, and reads its
//             records at each word of the client's: after the take, [5, 3, 3]; after the merge, two records, 5 and 6.
//   CLIENT -- walks to the container with the director and opens it on the inventory screen. Leg TAKE: it presses the
//             drag with the right button held on the stack of 6 (the screen's dragRMB, then its own beginDrag on that
//             stack's item widget), which runs the widget's takeFromStack(1): the stack must read 5, the bag one record
//             more, and both must still hold at a second read. Leg MERGE: it runs addToStack on one stack of 3 with the
//             other's entry, then removes the other as the canvas's combine action does: the pair must read one stack of
//             6, and still do so at the second read. Each leg's word goes to the host in the lobby chat.
// The client's DONE line carries its verdict; the host's PASS or FAIL line is the authority's.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::container_stack_drill {

bool IsEnabled();

// Game thread, once per pump tick; a single bool read when off.
void Tick(coop::net::Session* session);

// The container, the walk and the steps belong to one world and one session.
void OnDisconnect();

}  // namespace coop::dev::container_stack_drill
