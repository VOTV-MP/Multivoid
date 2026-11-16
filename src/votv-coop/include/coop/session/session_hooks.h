// coop/session/session_hooks.h -- what the net library asks of the game it runs in: the DLL's answer
// to each NetHooks member (coop/net/net_hooks.h), passed to Session::Start.

#pragma once

#include "coop/net/net_hooks.h"

namespace coop::session_hooks {

// The host's own player element id, the host's pose ledger and the drive-merge counter.
const coop::net::NetHooks& Game();

}  // namespace coop::session_hooks
