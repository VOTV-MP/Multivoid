// coop/dev/fireext_drill.h -- drill: a fire extinguisher taken off its wall mount (ini
// fireext_drill=off|carry|short|client|join|runaway / env VOTVCOOP_FIREEXT_DRILL; BOTH peers).
//
// One peer acts, both watch. The actor walks to the nearest mounted extinguisher by the bot director and grabs it through the
// use key's chain (playerGrabbed_pre, useAction, playerGrabbed). carry: the host carries it some metres and lets go; the
// client runs the laptop exit's setPropProps once on its copy, which the drive must re-latch. short: the host lets go at once,
// then re-sends the closed hold's pose, which every receiver must drop. client: the client carries. join: the host takes it
// off while a joiner loads. runaway: the host lets go and damages it through its own addDamage; the client says its copy's
// rest pose and whether it ever thrust, and the host says rig: VERDICT fireext-runaway PASS|FAIL|INVALID (coop/dev/
// rest_compare). The host's leg turns it upside down so the thrust flies it (over 300 cm, else INVALID); reds
// fireext_claim_off (host) and fireext_cancel_off (client). Each step waits on a state the actor reads; the last prints
// `ACTOR DONE`, a refused one `INVALID`. The evidence is the watch: each peer prints every extinguisher it has and each change.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::fireext_drill {

// True unless the row is `off`.
bool IsEnabled();

// Advance this peer's steps and its watch. Every pump tick in a world. Game thread.
void Tick(coop::net::Session* session);

// The session ended: back to the first step, the watch emptied.
void OnDisconnect();

}  // namespace coop::dev::fireext_drill
