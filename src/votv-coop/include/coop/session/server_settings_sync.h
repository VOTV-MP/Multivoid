// coop/session/server_settings_sync.h -- the host's replicated server-scope rows reach every
// client: the snapshot when a slot is ready, a delta after a change, one row per message
// (ReliableKind::ServerSetting). A changed notify row is announced: the host prints one chat line,
// and each client prints its own when the row arrives with the announced bit. The session layer
// itself is the config's (coop/config/config.h); this module is its lifecycle seams and its wire.

#pragma once

#include "coop/net/session.h"

namespace coop::server_settings_sync {

// Once, at boot, before the TimelineThread exists: the sender's handle -- the process's one
// long-lived Session, never cleared -- and the stop listener on it (OnSessionEnd).
void BindSession(coop::net::Session& session);

// Once, at boot, after BindSession: the sender follows every replicated row.
void SubscribeRows();

// A session is about to start (TimelineThread, before the transport starts): the config's session
// layer begins -- unless a session is already running, which Start would refuse: then the running
// one's layer is kept and a warning names it.
void OnSessionStart(bool host);

// The session stopped, whoever stopped it, on whatever thread: the config's session layer ends.
void OnSessionEnd();

// Game thread, every pump tick: the host sends the snapshot to each slot that is ready and has not
// had it, every replicated row.
void HostTick(coop::net::Session& session);

// Game thread, the state family's receiver: a client takes one row of the host's session values.
void HandleServerSetting(const coop::net::Session::ReliableMessage& msg);

}  // namespace coop::server_settings_sync
