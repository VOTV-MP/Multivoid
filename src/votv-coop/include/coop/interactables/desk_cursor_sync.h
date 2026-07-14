// coop/desk_cursor_sync.h -- v109: the coords-panel LIVE cursor as a continuous
// motion stream (fixes the 3Hz-reliable-snap jaggy).
//
// The reliable DishAimState (console_state_sync) carries the COMMITTED-coord
// identity (discrete triangulation locks); THIS lane carries the live sweep
// (ui_coordinates.viewCoordinate) as an unreliable 60Hz pose stream, interpolated
// on the mirror over a 50ms LerpWindow and written via WriteCursorOnly (a pure
// viewCoordinate memcpy -- the widget's own Tick repaints). Exact sibling shape of
// the hand-item motion stream (MsgType::HandPose). Host-authority: the desk CLAIM
// (device_occupancy) gates who may stream; the cursor content is client-authored-
// passthrough (like player pose). See docs / the /qf design thread.
//
// One concept = one folder: this lives with the other desk/device interactables.

#pragma once

namespace coop::net { class Session; }

namespace coop::desk_cursor_sync {

void Install(coop::net::Session* session);

// Game thread, per pump tick. SENDER (desk-claim holder): publish viewCoordinate
// via Session::SetLocalDeskCursor (net thread fans out). RECEIVER (everyone else):
// interpolate the holder's streamed cursor and WriteCursorOnly; on the holder's
// release, replay the desk's native intComs_unfocused (reset-on-release).
void Tick();

void OnDisconnect();

}  // namespace coop::desk_cursor_sync
