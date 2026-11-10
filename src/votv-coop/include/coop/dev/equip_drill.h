// coop/dev/equip_drill.h -- [dev] every empty equipment slot takes an item, on the host and on a client alike.
// equip_drill=run on both peers: 15 s after this peer's world is up, counts the empty slots of saveSlot.equipment, then
// equips a list of wearables (two fanny packs among them, and items the game refuses as a second of their type) through the
// game's own AddEquipment until the list ends. DONE PASS when every slot that was empty took an item; FAIL when a slot is
// still empty after the list, which is the game finding no free slot where one stands empty. Each peer logs its own
// verdict line, tagged [EQUIP-DRILL]. A client joined with a fresh profile is the case that padded its slots on the join.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::equip_drill {

// Advances this peer's steps; a latched read when off. Game thread.
void Tick(coop::net::Session* s);

// The session ended: the drill starts over.
void OnDisconnect();

}  // namespace coop::dev::equip_drill
