// coop/creatures/wisp_attack_sync.h -- Killer Wisp coop, host side.
//
// killerwisp_C's grab and kill verbs always hit the LOCAL player: they call getMainPlayer(), never
// the `Target` the blueprint acquired. So on the host, once per net-pump tick, this module picks each
// wisp's victim among the host pawn and the puppets in range and reachable, and writes it back to
// `Target`, closes the wisp on a puppet victim itself (the blueprint would never grab one), and
// on contact relays the grab to the victim's slot and the tear to everyone, then despawns the
// wisp so it cannot re-grab. When the pick is a puppet and the blueprint grabs the host anyway, the host is
// held harmless for that window: that wisp's damage is refused at the script-body gate by the
// verb's own source argument, the health re-pinned, the ragdoll gate held, and
// the native grab aborted with the wisp's own releasePlayer verb. A wisp that kills a kerfur or
// a hound instead is watched for that NPC's blueprint-internal self-destroy, which no observer
// sees, so the mirrors despawn with it.
//
// Each stage carries its reasoning beside the code in wisp_attack_sync.cpp. Game thread.

#pragma once

namespace coop::net { class Session; }

namespace coop::wisp_attack_sync {

// Cache the session and watch the player's damage verb at the script-body gate. Idempotent; the
// watch registers by name and the gate resolves it, so it needs no wait for mainPlayer_C. Call
// this every net-pump tick like the other Install()s.
void Install(coop::net::Session* session);

// Host per-tick detect, neutralise and relay. A no-op off the host. On the host it walks the
// tracked Npc set -- small, never GUObjectArray -- and runs the victim selector for every killer
// wisp in it, which is a range and line-of-sight test per candidate plus one Target write,
// whether or not one is closing. Game thread.
void Tick();

// Clear per-session state -- the victim picks, closing windows, relayed edges, pending despawns
// and the damage cancel -- and drop the ragdoll hold, so a teardown inside the window cannot
// leave the local player permanently un-ragdollable.
void OnDisconnect();

}  // namespace coop::wisp_attack_sync
