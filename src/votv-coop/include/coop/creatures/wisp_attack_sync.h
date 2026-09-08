// coop/wisp_attack_sync.h -- Killer Wisp coop, host side.
//
// killerwisp_C always grabs and kills the LOCAL player: its blueprint verbs hit getMainPlayer(),
// never the `Target` it acquired. So on the host, once per net-pump tick, this module picks each
// wisp's victim among the host pawn and the reachable puppets in range and writes it back to
// `Target`, closes the wisp on a puppet victim itself (the blueprint would never grab one), and
// on contact relays the grab to the victim's slot and the tear to everyone, then despawns the
// wisp so it cannot re-grab. Across that window the host, whom the blueprint grabs anyway, is
// held harmless: the limb damage is cancelled, the health re-pinned, the ragdoll gate held, and
// the native grab aborted with the wisp's own releasePlayer verb. A wisp that kills a kerfur or
// a hound instead is watched for that NPC's blueprint-internal self-destroy, which no observer
// sees, so the mirrors despawn with it.
//
// Each stage carries its reasoning beside the code in wisp_attack_sync.cpp. Game thread.

#pragma once

namespace coop::net { class Session; }

namespace coop::wisp_attack_sync {

// Cache the session and install the AddPlayerDamage PRE-cancel interceptor. Idempotent, and the
// interceptor resolves only once mainPlayer_C has loaded, so call this every net-pump tick like
// the other Install()s.
void Install(coop::net::Session* session);

// Host per-tick detect, neutralise and relay. A cheap no-op off the host and while no wisp is
// closing on a client. Walks the tracked Npc set, which is small, never GUObjectArray. Game
// thread.
void Tick();

// Clear per-session state -- the victim picks, closing windows, relayed edges, pending despawns
// and the damage cancel -- and drop the ragdoll hold, so a teardown inside the window cannot
// leave the local player permanently un-ragdollable.
void OnDisconnect();

}  // namespace coop::wisp_attack_sync
