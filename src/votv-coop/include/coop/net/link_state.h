// coop/net/link_state.h -- where a session's links stand: the connection state, the stage of a client's
// dial and what the host said when a link ended. Plain values the session publishes; the join screen
// and the pump read them.

#pragma once

#include "coop/net/end_reason.h"  // the code a close carries to the peer

#include <cstdint>
#include <string>

namespace coop::net {

enum class ConnState : uint8_t { Disconnected, Handshaking, Connected };

// Where a client's host link stands, published by the net thread at its own callback edges so the
// join screen can name the stage a joiner is waiting in; the pump reads it each tick. Idle until
// the dial, Admitted once the host's slot assignment finished the link.
enum class LinkStage : uint8_t { Idle, Dialing, FindingRoute, SocketUp, Admitted };

// What the host, or the transport, said when a client's link ended: the code decoded from the
// transport's end reason, and the text that came with it. None until a close is captured.
struct HostClose {
    EndReason   code = EndReason::None;
    std::string text;
};

}  // namespace coop::net
