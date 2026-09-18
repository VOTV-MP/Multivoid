// coop/props/drivebox_record.h -- the drive box (Aprop_box_C, the "data case") republishes its
// own save record when its state changes in place. Overview: docs/props.md.
//
// prop_save_data carries a prop's record on every BIRTH path -- a spawn, a drop intent, the join
// snapshot -- and on none other, so a prop whose save state changes while it sits there never
// tells anyone. The drive box is the class where that shows: opening it spawns a prop_boxCap_C in
// the presser's hands and sets `opened`, and both that flag and `drives_in` live in its record.
// A guest's open therefore left every other peer with a closed box (its own lid still on, plus
// the guest's newborn one on the floor), and a drive put in or taken out left them with stale
// contents until something touched the box again.
//
// The verb watched is the box's own upd(): every mutation ends there and no tick or look-at path
// calls it. The watch is a script_gate one, not a ProcessEvent observer -- the box calls its own
// upd() from inside its Blueprint, a route ProcessEvent never sees. The .cpp carries the rest.

#pragma once

namespace coop::net { class Session; }

namespace coop::props::drivebox_record {

// Watch the box's refresh verb and cache the session. Idempotent; retried until the class loads.
// Game thread.
void Install(coop::net::Session* session);

// Session teardown: the per-actor coalescing state is dropped.
void OnDisconnect();

}  // namespace coop::props::drivebox_record
