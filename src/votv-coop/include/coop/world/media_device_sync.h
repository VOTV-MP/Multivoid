// coop/world/media_device_sync.h -- one track, position and play state for a placed radio or TV on every peer, and a TV's
// channel and picture.
//
// Each device plays through its own media player, picks its own next track and starts when its own player presses, so
// the host's is the state: it sends what it reads, and a client's copy is moved to it. A client whose player has the
// device's interface open sends what its player changed. A track crosses as a hash, and a peer without it in its own
// catalog, or with the Custom Content rule off, stays silent. The sound stays each peer's own. The shape follows Watchers
// of the Void's radio lane.

#pragma once

namespace coop::net {
class Session;
struct MediaDeviceStatePayload;
}  // namespace coop::net

namespace coop::media_device_sync {

void Install(coop::net::Session* session);

// Per gameplay tick: the poll, four times a second. Game thread.
void Tick();

// The host's state of a device, or on the host a client's edit. Validated at the router.
void OnState(int senderSlot, const coop::net::MediaDeviceStatePayload& p);

// HOST: a joiner reached its ready edge. Every device goes out again.
void QueueConnectBroadcastForSlot(int slot);

void OnDisconnect();

}  // namespace coop::media_device_sync
