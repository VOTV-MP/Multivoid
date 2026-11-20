// coop/world/flood_sync.h -- the basement flood is the host's: its burst pipes and its water level, on every peer.
//
// A burst pipe (burstWaterPipe_C) keeps one bit, `damaged`: its State turns its spray, sound and tick on or off, its
// tick raises its flood volume (waterVolume_basementFlooder_C, whose level is getLevel/setLevel) through addWater, and
// a toolbox's toolboxFix mends it. Each copy ran it alone, so a flood never reached a client and a client's repair
// never reached the host (Ghezra/Multivoid#40).
//
// CLIENT: its pipes run no tick and take no damage of their own; its player's repair runs on its copy and is asked of
// the host (FloodIntent), which mends its pipe with State for a living sender in reach with a toolbox, the pipe
// damaged. HOST: a pipe's bit is sent from the pipe's own State, the call every route to a burst or a mend runs (the
// hit's addDamage, the repair's toolboxFix, a save's loadData), so the send is the event and not a read. The flood's
// level is read four times a second (flood_sync.cpp says why that one stays a read) and a level that moved by a whole
// unit is sent (FloodState); a refused repair is answered with the pipe's state, and a joiner gets them all. A client
// applies them through State and setLevel with updateWaterData. Both are level-placed, named by object name. Game thread.

#pragma once

namespace coop::net {
class Session;
struct FloodPayload;
}  // namespace coop::net

namespace coop::flood_sync {

// Store the session. Idempotent; called at each session start.
void Install(coop::net::Session* session);

// The host's level read; the watches retried until registered. Game thread, once per pump tick.
void Tick();

// A client's receiver: a pipe's state or a volume's level, from the host.
void OnState(const coop::net::FloodPayload& p);

// The host's receiver: a client's repair of a pipe.
void OnIntent(int senderSlot, const coop::net::FloodPayload& p);

// HOST: every pipe and flood volume, to a joiner at its ready edge.
void SendJoin(int slot);

// The session ended: everything is forgotten.
void OnDisconnect();

}  // namespace coop::flood_sync
