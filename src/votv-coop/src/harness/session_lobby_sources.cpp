// harness/session_lobby_sources.cpp -- the sources of the lobby heartbeat's player count and its players' links (see
// harness/session_runtime.h): reads of the session object this module owns, taken on the announcer's worker thread.

#include "harness/session_runtime.h"

#include "coop/net/lobby_links.h"
#include "coop/net/session.h"
#include "coop/session/session_manager.h"

namespace harness::session_runtime {
namespace {

// The lobby's live player count for the heartbeat, on the announcer's worker thread, so atomics
// only: running() and connectedPeerCount() (the peerConns_ and ready arrays); the session is
// process-lifetime. Not role(), a plain field, and only a host announces. +1 is the host itself;
// before the session starts (the lobby is announced while the world loads) 1 is the honest
// answer. A joiner is undercounted for its admission round trip (the pending band holds no seat),
// invisible at a 30 s cadence.
int LobbyPlayerCount() {
    if (!Session().running()) return 1;
    return Session().connectedPeerCount() + 1;
}

// The host's players counted by the link it measures on each (the host's own slot is none), for the lobby
// heartbeat beside the count; a read of each connection's status, which the transport allows off its thread.
coop::net::lobby::LobbyLinks LobbyLinkCounts() {
    coop::net::lobby::LobbyLinks l;
    if (!Session().running()) return l;
    for (int slot = 1; slot < coop::net::kMaxPeers; ++slot) {
        switch (Session().LinkKindForSlot(slot)) {
            case coop::net::LinkKind::Relayed: ++l.relayed; break;
            case coop::net::LinkKind::Direct:  ++l.direct; break;
            case coop::net::LinkKind::Lan:     ++l.lan; break;
            default: break;
        }
    }
    return l;
}

}  // namespace

void InstallLobbyHeartbeatSources() {
    coop::session_manager::SetPlayerCountSource(&LobbyPlayerCount);
    coop::session_manager::SetLinksSource(&LobbyLinkCounts);
}

}  // namespace harness::session_runtime
