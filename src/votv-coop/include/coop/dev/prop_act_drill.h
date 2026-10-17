// coop/dev/prop_act_drill.h -- [dev] the act drill: a client's take or use of a shared prop asks the host first
// (ini prop_act_drill=honest|race|depart|edge, env VOTVCOOP_PROP_ACT_DRILL; BOTH peers, the same value).
//
//   HOST   -- spawns each step's target on the floor in front of its own player, waits until the target is announced,
//             says a ready line (the step and the target's eid) in the lobby chat, then judges its own counters when
//             the client says its done line. Its PASS line ends the phase.
//   CLIENT -- reads the line, resolves the mirror by eid, walks within reach with the director, runs the verb by
//             reflection as a press would (the watch catches it), waits until no request is pending, judges what its
//             own copies show, and says its done line.
// honest: one client takes and uses each family of shared prop. A FAIL is the lane failing; every wait on the other
// peer ends on a chat line or a counter, and its bound is a bound that FAILs the step, never a wait.

#pragma once

#include "coop/element/element.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace coop::net { class Session; }

namespace coop::dev::prop_act_drill {

// Session start, game thread: says which phase is armed (a phase this build has no steps for is said too).
void Install(coop::net::Session* s);

// Advances this peer's steps; a latched read when off. Game thread.
void Tick(coop::net::Session* s);

// The session ended: the drill starts over.
void OnDisconnect();

// What the core (prop_act_drill.cpp) and a phase's steps (prop_act_drill_<phase>.cpp) share: the core runs the
// step machine of both peers over a phase's table, and the phase says what each step is and how the host judges it.
namespace detail {

inline constexpr int kClientSlot = 1;   // the client a single-client phase drives

enum class Verb : uint8_t { Pocket, Hold, Use, Soap };   // putObjectInventory2, Hold Object, actionOptionIndex 4, playerUsedOn

// Where a consumed target ends, on the client's own copies: the inventory gains one of its class, the hand holds one
// of its class, or nowhere (an eaten or used-up prop).
enum class Ends : uint8_t { Inventory, Hand, Gone };

struct Row {
    const char* name;           // the step's name in every line
    const wchar_t* path;        // the target's class path, which the host spawns
    const wchar_t* leaf;        // its class name, as an inventory record and the hand's actor carry it
    Verb verb;
    Ends ends;
    const char* (*setup)(void* player, void* mirror);   // client, before the act: null when done, else why not; or null
};

// The host's counters for the client's slot when it said the step was ready.
struct Baseline {
    uint32_t destroys = 0;      // CommitsOf(slot, Destroy)
    uint32_t rawDestroys = 0;   // DrainedFrom(slot, PropDestroy)
};

enum class Judged : uint8_t { Wait, Pass, Fail };

// The host's verdict on a step once its client said it was done; `why` says what a Wait is on or what failed, and on a
// Pass what held. Wait is called again each check until the step's bound.
using JudgeFn = Judged (*)(const Row& row, coop::element::ElementId eid, const Baseline& base, std::string* why);

struct Phase {
    const char* name;
    const Row* rows;
    size_t count;
    JudgeFn judge;
};

const Phase& Honest();   // prop_act_drill_honest.cpp

}  // namespace detail
}  // namespace coop::dev::prop_act_drill
