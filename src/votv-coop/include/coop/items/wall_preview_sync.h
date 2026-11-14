// coop/items/wall_preview_sync.h -- the wall builder's aiming preview reaches every peer.
//
// While a player holds a wall builder (prop_wallbuilder_C and its children), its tick moves the tool's own preview
// along that player's aim (setPivotLoc): `pivot_a` where the aim lands, and while the left button is held (`editing`)
// the `cube` that is the wall to be placed and `pivot_b`, its far corner. It ran on the holder's machine alone, so the
// others saw the tool and no preview until the wall was placed. Here the holder sends its preview -- `editing` and the
// three components' world transforms -- when it changes, at most ten times a second, and once more when it ends
// (WallPreview): a client's to the host, which checks its mirror of that hand is a wall builder and sends it on, the
// host's own to everyone. Each peer shows it on its mirror of that hand, every frame while it holds, so the preview
// stays where the holder aims as the puppet moves. A display only: the wall is placed by the holder's addWall, as
// before (coop/world/build_sync). Game thread.
#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct WallPreviewPayload;
}  // namespace coop::net

namespace coop::wall_preview_sync {

// Keep the session. Idempotent.
void Install(coop::net::Session* session);

// This peer's held wall builder read and sent on change; every peer's preview shown on its mirror. Per frame.
void Tick();

// A peer's preview, its length checked by the dispatcher.
void OnPreview(int senderSlot, const coop::net::WallPreviewPayload& p);

// A peer left: its preview goes.
void OnPeerLeft(uint8_t slot);

// Session end.
void OnDisconnect();

}  // namespace coop::wall_preview_sync
