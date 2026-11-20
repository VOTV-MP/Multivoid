// coop/dev/hand_record_drill.h -- [dev] a held item's own record reaches the other players' mirrors of that hand
// (coop/player/hand_record_sync). hand_record_drill=can|hand|joiner|forge|hostile|keyless|oversize|bounds|nonfinite|box|display on
// both peers; the host says the one verdict `rig: VERDICT hand-record`. can: the client opens a can, eats, drops it, eats
// from it; the host's copy carries each state; then it picks it up and drops it again, and both copies must carry the state
// it was dropped with (the adoption's reader: the host publishing for that Key meanwhile is the FAIL). hand: the client
// opens and eats twice, then empties its hand; the host's mirror shows each state once its record has settled. joiner: the
// host holds an opened can before the client joins; the client's mirror shows it at the snapshot and its copy goes when
// the host empties its hand. forge, hostile, keyless, oversize, bounds, nonfinite: a forged record (another class, a class
// that resolves to none, no key, over the ceiling, a string past the bounds, a NaN) is refused at its counted line; a
// record of the client stored after it is the FAIL. box: a held order box's record (its classes[] names its items) is
// stored. display: the host holds a floppy disc, whose loadData turns physics on; the client reads its mirror one drill tick
// after the record is written into it, and a mirror that simulates physics or collides is the FAIL. Reds:
// hand_record_relay_off and hand_record_clear_off are read by the joiner arm, can_adopt_off by the can arm,
// hand_display_redo_off (the client) by the display arm.
// Nobody is moved: the item goes into the hand through the hold slot and the game's own updateHold.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::hand_record_drill {

// Per frame. A latched read when the drill is off.
void Tick(coop::net::Session* session);

// Session end: the steps and the marks belong to one session.
void OnDisconnect();

}  // namespace coop::dev::hand_record_drill
