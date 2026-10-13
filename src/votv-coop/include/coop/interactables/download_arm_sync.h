// coop/interactables/download_arm_sync.h -- the download machine's arm and reset: the host's, replayed on each client.
// The desk forms a download when its dishes stop (formDownload, which rolls the download's polarity when handed -1),
// and the gamemode's deleteActiveSignal resets it (the desk's "Signal data deleted" reset, which rolls it again, then
// the renderer's signal actor and the signal camera's trigger). The host is the machine's one author: the script
// gate's POSTs send each verb's outcome on DishArm. A client runs the same verb once, the host's decoded and polarity
// written into its parameters at the PREs while the replay runs, and refuses both outside one in the world it
// announced. A joiner gets a reset ahead of the desk's seed when its world was captured before one, and an armed
// machine's row after the catch's seed. MTA shape: the server's element data, applied on the client through the same
// setter (reference/mtasa-blue/Client/mods/deathmatch/logic/rpc/CElementRPCs.cpp:84-99, SetElementData).

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::download_arm_sync {

void Install(coop::net::Session* session);

// Game thread, per pump tick: the three watches driven to settled, then a bool.
void Tick();

// CLIENT: the host's arm or reset, replayed through the game's own verb. From the host alone.
void OnDishArm(const coop::net::DishArmPayload& p);

// HOST: the resets made before the capture of a joiner's world (save_transfer's capture instant); at its world-ready,
// the reset row ahead of the desk's seed, and the arm row after the catch's seed and the dishes'.
void CaptureJoinSnapshot(int peerSlot);
void CancelJoinSnapshot(int peerSlot);
void QueueConnectResetForSlot(int peerSlot);
void QueueConnectArmForSlot(int peerSlot);

void OnDisconnect();

// The lane's counters, for its drill and its session-end line.
struct Counts {
    uint64_t sentArm = 0, sentReset = 0;          // HOST
    uint64_t replayedArm = 0, replayedReset = 0;  // CLIENT
    uint64_t refused = 0;                          // CLIENT: its own verb, outside a replay, in its announced world
    uint64_t loadNative = 0;                       // CLIENT: its own verb run as a world's load
    uint64_t dropped = 0;                          // CLIENT: a row with nothing to replay on
    uint64_t replayOff = 0;                        // CLIENT: a replay whose outcome was not the host's
};
Counts LaneCounts();

}  // namespace coop::download_arm_sync
