// coop/world/flood_sync.h -- the basement flood is the host's: its burst pipes and its water level, on every peer.
//
// A burst pipe (burstWaterPipe_C) keeps one bit, `damaged`: its State turns its spray, sound and tick on or off, its tick
// raises its flood volume (waterVolume_basementFlooder_C, getLevel/setLevel) through addWater, and a toolbox's toolboxFix
// mends it (Ghezra/Multivoid#40). HOST: a pipe's bit is sent from the pipe's own State, the call every route runs (the
// hit's addDamage, toolboxFix, a save's loadData), and the level from the volume's own addWater and setLevel once it has
// moved a whole unit (FloodState); a joiner gets every pipe and level. CLIENT: outside the lane's own apply it refuses a
// pipe's addDamage (a client's hit is dropped, not forwarded) and the volume's addWater and setLevel, so its level is the
// host's; a burst pipe's State, spray, sound and tick, the drain's tick and updateWaterData still run. Its player's
// repair runs on its copy and is asked of the host (FloodIntent), which mends its pipe for a living sender in reach with a
// toolbox, the pipe damaged. A judged and refused ask is answered with the pipe's state; one from a slot not world-ready,
// past four a second, or naming no pipe or another op is dropped unanswered. Level-placed, named by object name. Game thread.

#pragma once

#include <cstdint>
#include <string>

namespace coop::net {
class Session;
struct FloodPayload;
}  // namespace coop::net

namespace coop::flood_sync {

// Store the session. Idempotent; called at each session start.
void Install(coop::net::Session* session);

// The watches, retried until registered. Game thread, once per pump tick.
void Tick();

// A client's receiver: a pipe's state or a volume's level, from the host.
void OnState(const coop::net::FloodPayload& p);

// The host's receiver: a client's repair of a pipe.
void OnIntent(int senderSlot, const coop::net::FloodPayload& p);

// HOST: every pipe and flood volume, to a joiner at its ready edge.
void SendJoin(int slot);

// The session ended: everything is forgotten.
void OnDisconnect();

// The flood drill's reads of the lane (coop/dev/flood_drill): what it counted and how the pipes and the flood read.
namespace dev {

struct Counts {
    uint32_t pipesSent = 0;      // HOST: a pipe's state sent to every client
    uint32_t levelsSent = 0;     // HOST: a level sent to every client
    float    lastLevelSent = 0;  // HOST: that level
    uint32_t judged = 0;         // HOST: a client's repair judged (mended or refused)
    uint32_t mended = 0;         // HOST: of those, the pipe mended
    uint32_t refusedTarget = 0;  // HOST: of those, refused by the target check (the sender's reach of the pipe)
    uint32_t refusedOther = 0;   // HOST: of those, refused for another reason
    const char* lastWhy = "";    // HOST: the last refusal's reason
};
Counts Read();

// The lane's smallest level step, in the level's own units (the volume is 500 high): a flood that moves less is not sent.
float LevelEpsilon();

// The pipe at `rank` in object-name order, the same on every peer (0 the first); null when the world has fewer.
void* PipeByRank(size_t rank);

// Every pipe's bit in object-name order, '1' a burst one; false when the world has none.
bool PipeBits(std::string& out);

// The flood volume's level; false when the world has none.
bool LevelNow(float& out);

// Forget which bit each pipe's last send carried, as a pipe burst before this session's first send is: the next State of
// every pipe is told whatever its value.
void ForgetSent();

}  // namespace dev

}  // namespace coop::flood_sync
