// coop/dev/equip_drill.h -- [dev] every empty equipment slot takes an item, on the host and on a client alike.
// equip_drill=run on both peers: once this peer's world is settled (the host's `rig: READY hosting`, a client's streams
// running), counts the empty slots of saveSlot.equipment, then equips a list of wearables (two fanny packs among them, and
// items the game refuses as a second of their type) through the game's own AddEquipment until the list ends.
// `rig: VERDICT equip-host|equip-client PASS` when every slot that was empty took an item; FAIL when a slot is still
// empty after the list, which is the game finding no free slot where one stands empty; INVALID when no slot was empty.
// Each peer says its own verdict. A client joined with a fresh profile is the case that padded its slots on the join; the
// red is equip_blank_zeroed=1 on the client, which writes those slots as the old zeroed blank.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::equip_drill {

// Advances this peer's steps; a latched read when off. Game thread.
void Tick(coop::net::Session* s);

// The session ended: the drill starts over.
void OnDisconnect();

}  // namespace coop::dev::equip_drill
