// coop/items/broom_stroke.h -- a broom stroke is the host's, whoever swings the broom.
//
// A stroke reads three things of its holder -- the segment `arm` returns from the local camera, and
// the heading and velocity the push adds up -- then turns every chip pile in reach into a clump,
// empties the dispenser piles there and pushes the physics bodies there. On a client each of those
// is a second author of the host's world, so a client refuses its own stroke at the swing montage's
// notify and sends the host the three reads. The host runs the game's own stroke on its mirror of
// that client's broom, with the client's puppet as the holder and each read answered with the
// client's value, where the puppet would give its display heading and a velocity rebuilt from its
// speed. MTA runs the game's code for a remote ped the same way, with that ped's pad and camera
// switched in (reference/mtasa-blue/Client/multiplayer_sa/multiplayer_keysync.cpp, SwitchContext).
// What the stroke then does leaves the host on the channels that already carry it: the swept clumps
// (coop/props/trash_sweep), the dispensed trash and the pushed bodies (coop/items/broom_push), and
// the dispenser piles' counters and deaths (coop/props/trash_pile_sync). Game thread.

#pragma once

#include <cstdint>

namespace coop::net { class Session; struct BroomStrokePayload; }

namespace coop::broom_stroke {

// Watch the stroke's notify, `arm` and the dispenser pile's `broomed` by name. Idempotent and
// latched: a refused registration, or one that never goes live, is said once and not retried.
// Game thread.
void Install(coop::net::Session* session);

// HOST receiver: the client in `senderSlot` swung its broom and read `stroke` of its holder. A stroke
// whose heading is a unit vector and whose velocity is at most terminal waits in that client's queue
// for Tick; past a full queue it is refused. Called from event_feed's reliable drain. Game thread.
void OnBroomStroke(coop::net::Session& session, const coop::net::BroomStrokePayload& stroke,
                   uint8_t senderSlot);

// HOST, per gameplay tick: run at most one queued stroke per client, no faster than its stroke rate,
// on that client's broom mirror -- when the client holds a broom here and both ends of the segment
// are in its reach. The first such stroke installs the heading and velocity seams. Game thread.
void Tick(coop::net::Session& session);

// A client left `slot`: its stroke rate and its queued strokes go with it, so the next occupant
// starts with a full bucket and an empty queue. Game thread.
void OnPeerLeft(uint8_t slot);

// Zero the tallies and the stroke rate buckets so a second session's numbers are its own. Game
// thread.
void OnSessionStart();

// Drop the cached session and report the tally: with no session nothing is refused and a
// single-player broom sweeps exactly as the game wrote it. Game thread.
void OnDisconnect();

}  // namespace coop::broom_stroke
