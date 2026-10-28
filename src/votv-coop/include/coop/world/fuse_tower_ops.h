// coop/world/fuse_tower_ops.h -- what a player does at a fuse tower (a coordinate tower or the radio tower), and the
// one roll a tower makes. A client never rolls a tower: its scramble (the dish's Scramble Radar Dish) and its
// lever's judgement are refused at the script gate on every route (the fuckuper's timer, an explosion, the radio
// tower's decay timer and reset lever, a coordinate tower's own load), so the host's world is the towers' author and
// its rows (coop/world/fuse_tower_rows) the client's. A client's press of a puzzle button, a lever (the radio
// tower's reset lever included) or the panel's retract is refused there too and sent to the host, which runs the
// tower's own use with the look-at answered. A pull or an insert moves a fuse into or out of the player's own hand,
// so the client's own game makes it and claims the slot; the host takes the claim when its own copy allows the
// move, and refuses it otherwise: a refused pull's fuse is reaped from the puller's hand
// (coop/interactables/floppybox_sync's pop), a refused insert's fuse given back where the inserter stands
// (coop/world/power_upgrade's refund), an insert taken only against a fuse the host saw the inserter spend at that
// tower. On the two easiest difficulties the game spends no fuse and the insert solves the puzzle, so a client's
// insert is refused at the gate and sent like a press, with no claim, and the host runs it. A tower is named by
// (kind, id). The host runs a sender's ops in its order, within reach, at a bounded
// rate. What differs by kind is coop/world/fuse_tower_kind's. Game thread throughout.

#pragma once

#include "coop/net/protocol.h"

#include <cstddef>
#include <cstdint>

namespace coop::net { class Session; }

namespace coop::fuse_tower_ops {

// A player's act at a tower is judged within this reach of the part it acts on: twice the default armLength,
// the door, keypad and generator lanes' reach.
inline constexpr float kTowerReachUU = 400.0f;

// Register the gates and the destroy listener. Idempotent. Session install.
void Install(coop::net::Session* session);

// Settle the gates' registration and say once whether they are live; take the ops that waited for the towers,
// their sender's body, a spend or their turn. Game thread, per frame.
void Tick();

// HOST: a client's op (router: event_dispatch_world.cpp).
void OnOp(const coop::net::FuseTowerPayload& p, uint8_t sender);

// A FuseTowerState payload shorter than the struct, from any sender (router: event_dispatch_world.cpp): dropped
// there, said here once a session.
void OnShortPayload(size_t got);

// HOST: each slot's acknowledgement into the rows, held below its oldest refused claim so its client keeps its claims
// until their own answers. The answer to `answerSlot` carries that claim's seq too (and acknowledges up to the one
// before the next); a broadcast (-1) carries none, so only a per-slot answer tells a refusal.
void FillAcks(coop::net::FuseTowerPayload& p, int answerSlot);

// HOST: the answer that carried `slot`'s oldest refused claim was taken by the send; it is told, and leaves. Whether
// the slot has more to tell.
void RefusedTold(uint8_t slot);
bool RefusedPending(uint8_t slot);

// CLIENT: the rows' answer to my ops: the claims they acknowledge leave, and the refused pull they tell of has its
// fuse reaped (each refused claim has an answer of its own).
void OnAcks(uint16_t ack, uint16_t refused);

// CLIENT: the host's row with my untaken claims on it, the row the host will send once it takes them.
void Overlay(coop::net::FuseTowerRow& row);

// HOST: a leaver's waiting ops and spends go with it. Game thread.
void OnPeerLeft(uint8_t slot);

// [dev] the tower drill's reads. CLIENT: my ops sent and my claims the host has not answered. HOST: the ops it
// refused. BOTH: the rolls and judgements this peer refused.
uint64_t OpsSent();
size_t PendingClaims();
uint64_t OpsRefused();
uint64_t RollsRefused();
// [dev] CLIENT: my last op's seq, the rows' last acknowledgement of mine, and the pulled fuses reaped from my hand.
uint16_t LastSentSeq();
uint16_t LastAck();
uint64_t Reaped();

// [dev] tower_drill=red, joinred, radiored, radiojoinred, radioresetred or radiofusered: a client's gates stand open
// and it applies no rows, as before the lane, so its towers roll on its own dice.
bool RedOpen();

void OnDisconnect();

}  // namespace coop::fuse_tower_ops
