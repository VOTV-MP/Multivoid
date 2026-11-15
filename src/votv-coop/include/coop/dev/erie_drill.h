// coop/dev/erie_drill.h -- [dev] the Erie plush's drills (coop/world/erie_sync) as steps of the event-fire drill, reached through
// coop/dev/gamemode_drill's dispatch. No player is moved. Every 5 s each peer says what it holds in `[GAP4]` lines.
//
// No drill runs the morph or grants an achievement: nothing feeds the plush, calls `fuck`, `anger` or `lib.progressAchievement`;
// the host's plush has `fullness` written to -100, and the live forms are spawned directly. The achievement step is unrun.
//
//   erieQuest  the HOST spawns a plush, a paper and a pencil; the CLIENT runs its copy's hit handler with the paper, then the
//              pencil (refused there and asked of the host, which runs it: `write` runs once both are in and mints the notes
//              and a food). The plush's flags and the notes must reach the client. The host destroys the plush at 50 s.
//   erieForms  the HOST spawns a live Erie (`state` 2: it neither wanders nor steals), an erieDrag_C that lands as a live Erie,
//              and an erieChop_C that ends in two; the CLIENT counts each form, tries a live Erie of its own (cancelled at the
//              spawn; only with VOTVCOOP_ERIE_DRILL_MINT=1) and calls `reducerep` on its copy, whose parked graph must not move
//              `rep`. The host moves its live Erie at 25 s and ends every form at 80 s. The angry Erie is not spawned.

#pragma once

#include <cstdint>
#include <string>

namespace coop::dev::erie_drill {

bool IsDrill(const std::string& name);
void ClientStep(const std::string& name);
void HostStep(const std::string& name, uint64_t sinceFireMs);
void OnDisconnect();

}  // namespace coop::dev::erie_drill
