// coop/interactables/tv_link_sync.h -- a web video a player puts on a television plays on that set
// for everyone (TvLink).
//
// The game's television opens a web address through its own openLink, and nothing of that crossed:
// each player's set played its own list. A post watch on openLink sends the address a set was just
// asked for, and a receiver hands it to its own copy of the set, named by the prop's key. A link is
// also typed here: an address entered in the search box of the set's remote (Enter) is played and
// sent. A YouTube address is sent as typed and resolved by each peer (coop/interactables/
// tv_link_resolver); the play each peer starts is its own, so two screens can be seconds apart. The
// remote's play and stop buttons and its playback slider cross as a pause, a resume or a seek. The
// host sends a joiner each playing set's address and position, and the joiner's set seeks there.
// `tv_links=0` in multivoid.ini turns the lane off on that peer.

#pragma once

#include <cstdint>
#include <string>

namespace coop::net {
class Session;
struct TvLinkPayload;
}  // namespace coop::net

namespace coop::tv_link_sync {

// Session up: the two watches, once.
void Install(coop::net::Session* session);

// The links that wait for their set (a joiner's replay racing its world load). Game thread.
void Tick();

// TvLink from the wire (router: event_dispatch_signal.cpp).
void OnLink(const coop::net::TvLinkPayload& p, uint8_t senderSlot);

// HOST: each set's last address, to a joiner.
void QueueConnectBroadcastForSlot(int slot);

// A player on this machine asked `tv` for `link`: sent to the others and played here. False when
// the address is refused or the set is not a television.
bool PlayLocal(void* tv, const std::wstring& link);

// A pause, resume or seek (coop::net::kTvOp*) this player did to `tv` at `seconds`: sent to the others. The
// caller has already done it here. False when it is not sent (no session, an unkeyed set, or too soon after
// the last one).
bool ControlLocal(void* tv, uint8_t op, double seconds);

// How many addresses this peer has handed to a set's openLink, and how many pauses, resumes and seeks it has
// applied from the wire or for a mid-video join (a drill reads them).
uint64_t Opened();
uint64_t Controls();

void OnDisconnect();

}  // namespace coop::tv_link_sync
