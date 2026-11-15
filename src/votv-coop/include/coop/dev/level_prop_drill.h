// coop/dev/level_prop_drill.h -- [dev] the level's door, wandering props, shrimp eater and footprints as four steps of the event-fire drill, reached through
// coop/dev/gamemode_drill's dispatch. Each is named after what the host forces; no player is moved.
//
//   gapDoor   the White Door (TheDoor_C): the host calls open() at 8 s, close() at 16 s, moves the door 15 m at 24 s and opens it
//             again at 30 s, closes it at 40 s. The client's copy must follow each (pose and open state), and the client's own
//             rolls (tp, open, close) show as refusals in the `[GAP2]` counters.
//   gapWander the Rorschach decal, the Ghost Log, the Rubber Duck and one base mannequin: the host writes each 15 m away at 8 to
//             20 s and 15 m back at 40 to 52 s. A client's `follows the host` line with the copy's old distance says the prop lane
//             had not already carried the prop.
//   gapShrimp the shrimp eater: the host stands a shrimp prop far from its player at 6 s, writes the eater's `lastloc` far away
//             at 9 s and calls eat() at 10 s, 20 s and 30 s; both peers count the shrimp and the empty packs.
//   gapPrints the footprints (hidden at the start): the CLIENT calls show() at its 12 s (refused there, asked of the host, who
//             shows them); the host writes them hidden at 30 s and the client's copy must follow.
// Every 5 s each peer says what it holds in `[GAP2]` lines.

#pragma once

#include <cstdint>
#include <string>

namespace coop::dev::level_prop_drill {

bool IsDrill(const std::string& name);
void ClientStep(const std::string& name);
void HostStep(const std::string& name, uint64_t sinceFireMs);
void OnDisconnect();

}  // namespace coop::dev::level_prop_drill
