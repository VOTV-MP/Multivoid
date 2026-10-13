// coop/net/session_config.h -- what a session is started with: its role, its transport topology and the
// Config Session::Start reads. Apart from session.h so a caller that only builds or passes a Config (the
// browser, the config layer, the join paths) does not need the Session class.

#pragma once

#include "coop/net/protocol.h"  // kDefaultPort

#include <cstdint>
#include <string>

namespace coop::net {

enum class Role : uint8_t { Host, Client };

// Transport topology; only Session::Start branches on it.
enum class Topology : uint8_t {
    LanDirect,  // CreateListenSocketIP / ConnectByIPAddress: a port-forwarded host or
                // one LAN; no signaling.
    P2P,        // CreateListenSocketP2P / ConnectP2PCustomSignaling + ICE through the
                // signaling server: a STUN hole-punch or a TURN relay.
};

struct Config {
    Role role = Role::Host;
    Topology topology = Topology::LanDirect;

    // --- LanDirect ---
    std::string peerIp = "127.0.0.1";   // client: the host's address
    uint16_t port = kDefaultPort;

    // --- P2P ---
    // signalingUrl: "host:port" of the signaling server; both peers connect outbound.
    std::string signalingUrl;
    // The bearer token the signaling server requires in the greeting; a client gets it with the
    // lobby's master entry, the host from its config.
    std::string signalingToken;
    // The host identity the client dials (`gen:<64 hex>`), as the host published it to the master;
    // it must equal the host's peer_identity::LocalIdentityString(). Client-only. This peer's own
    // identity is not a config field: peer_identity::InstallInto sets it process-wide before any
    // socket exists.
    std::string hostIdentity;
    // The lobby password: on a host the secret required (empty = open), on a client the one to
    // present. Never on the wire or in a log; a tag derived from it travels
    // (coop/net/lobby_password.h), and only to a host the client has bound to the identity it was
    // sent to.
    std::string lobbyPassword;
    // ICE candidate sources: STUN (hole-punch) and TURN (a coturn relay with short-lived REST
    // credentials); an empty string disables that path.
    std::string stunList;    // "host:port,host2:port"
    std::string turnList;    // "turn:host:port,..."
    std::string turnUser;    // parallel to turnList
    std::string turnPass;    // parallel to turnList
    int         turnTtlS = 0;  // the TURN credential's lifetime a master stated; 0 when none was
    // No candidate policy here: it is the player's net.ice setting, which Start reads for every
    // session whichever door it came through (coop/net/ice_policy.h).

    // True when the destination was named locally (a typed address, an ini, an autotest) rather
    // than advertised by the network. peer_admission tells "the player chose this address" from
    // "nobody advertised an identity" by it instead of inferring one from the other.
    bool selfAddressed = false;

    int sendHz = 60;
};

}  // namespace coop::net
