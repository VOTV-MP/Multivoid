// coop/creatures/kerfur_dress_sync.h -- an Omega's outfit is the host's, and a pat is seen by every player.
//
// The outfit is the Omega's `drip` rows (ue_wrap/actors/kerfur_dress); each copy's equipItem and unequipItem changed its
// own alone, so an item one player put on was seen by nobody else (Ghezra/Multivoid#38). A pat ran its montage on the
// patter's copy alone. HOST: reads every Omega's outfit twice a second and sends a changed one (KerfurDressState), every
// one to a joiner; runs a client's put-on (the row added) and take-off (its own unequipItem, whose item spawns on the
// host) for a living sender in reach. CLIENT: its put-on runs on its copy and is told to the host; its take-off is
// refused on its copy and asked of the host; it writes the host's outfit with updateDrip. A pat, either role's, is sent,
// and every other copy plays its look and meow. Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct KerfurDressPayload;
}  // namespace coop::net

namespace coop::kerfur_dress_sync {

// Store the session. Idempotent; called at each session start.
void Install(coop::net::Session* session);

// The host's reads; the watches retried until registered. Game thread, once per pump tick.
void Tick();

// This machine's player patted `kerfur` (kerfur_command_seat's actionName watch, before the body runs).
void OnLocalPat(void* kerfur);

// A client's receiver: an Omega's outfit, or a pat to show.
void OnState(const coop::net::KerfurDressPayload& p);

// The host's receiver: a client's put-on, take-off or pat.
void OnIntent(int senderSlot, const coop::net::KerfurDressPayload& p);

// HOST: every Omega's outfit, to a joiner at its ready edge.
void SendJoin(int slot);

// The session ended: everything is forgotten.
void OnDisconnect();

}  // namespace coop::kerfur_dress_sync
