// coop/interactables/atv_passenger.h -- the ATV's passenger seats: up to three riders beside the game's own driver seat.
//
// The game's ATV has one seat, and sitting in it possesses the rig. A passenger does not: its own player stays possessed,
// attached to the rig's root body at one of three seats (ue_wrap/devices/atv_passenger), movement off, collision off so it
// cannot push the rig, the camera free, and the driver's controls untouched. Every machine sits each passenger where the
// host's table says, the puppet of a remote one included, attached to that machine's own copy of the ATV, so a passenger
// rides the rig instead of trailing the pose stream behind it.
//
// The table is the host's (atv_authority.h: AtvEntry::passengerSlot, judged by OnPassengerRequest and JudgePassengerLocal) and
// reaches every client as AtvPassengerTable. This module is the rest: this machine's own body against the table (the use
// press that boards or leaves, the leaving causes, the request a client sends and its yield to a refusal), the host's send
// of the table, and the remote riders' seats, which coop/player/puppet_seat attaches. Game thread only.

#pragma once

#include <cstdint>
#include <string>

namespace coop::net {
class Session;
struct AtvPassengerAnswerPayload;
struct AtvPassengerTablePayload;
}  // namespace coop::net

namespace coop::atv_passenger {

// Per tick, in a world with a session: the host sends each table that changed; this machine's own body is read against
// the table (a rider is kept honest: the ATV alive and whole, the player alive and on its seat, no leaving key pressed);
// a client asks the host for what its body did and yields to what the host decided; and each remote rider's seat is told
// to its body's owner (coop/player/puppet_seat) when the table's answer for it changed. Called every net-pump tick.
void Tick();

// CLIENT receivers, from event_feed: the host's verdict on this client's latest request, and an ATV's table.
void OnAnswer(const coop::net::AtvPassengerAnswerPayload& a);
void OnTable(const coop::net::AtvPassengerTablePayload& t);

// HOST: every ATV's table that has a rider, to the joiner `peerSlot`, at its connect replay (after the ATVs' own state).
void SendTablesToSlot(coop::net::Session& s, int peerSlot);

// The use press (InpActEvt_use) at an ATV whose driver seat another player holds: this player boards the first free
// passenger seat when it may (the sit verb's own gates: empty hands, not falling; and a seat is free), acting at once
// and, on a client, asking the host. True when it boarded, false when it did not (the caller denies the press as it denied
// every press at an occupied ATV). Game thread.
bool BoardFromUse(void* atv);

// The use press while this machine's player is a passenger: it leaves. True when it was one (the press is spent).
bool LeaveFromUse();

// Whether this machine's player is a passenger, and the key of the ATV and the seat it holds there.
bool LocalIsPassenger();

// Leave the passenger seat now, for a cause outside this module (a teleport of this body): placed beside the ATV
// unless `place` is false, and told to the host. A no-op when this player is not a passenger. Game thread.
void LeaveLocal(const char* why, bool place = true);

// A session ended or the world changed: this machine's rider is released where it is, every puppet is let go.
void OnDisconnect();

// What a drill reads: this machine's own seat, and one ATV's table as this machine holds it.
struct LocalView {
    bool         seated = false;
    int          seat = -1;
    std::wstring key;
    void*        atv = nullptr;
};
LocalView ReadLocal();

// The seat a slot holds on the ATV filed under `key`, as this machine's table has it; -1 for none, or no such ATV. Whether
// that slot's puppet (or this machine's player, for its own slot) is attached to this machine's copy of the ATV.
int SeatOfSlot(const std::wstring& key, uint8_t slot, bool* outAttached = nullptr);

}  // namespace coop::atv_passenger
