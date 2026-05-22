// coop/event_feed.h -- coop session events surfaced to the on-screen feed.
//
// Gameplay/network layer (principle 7): decides WHAT lines the player sees and when,
// from coop/session state -- "X joined", "X left the game", connection errors. It
// drives the engine-side overlay through ue_wrap::hud_feed (which owns the widget).
//
// Mirrors MTA (research/findings/mta-chat-joinquit-reliability-2026-05-23.md): joins
// announce a nickname over the RELIABLE channel; the disconnect message is generated
// LOCALLY from the cause (MTA's quit-reason-enum pattern), not sent as a string.
//
// Game thread only (it calls hud_feed + reads the session).

#pragma once

#include <string>

namespace coop {

class RemotePlayer;
namespace net { class Session; }

namespace event_feed {

// The local player's display name (sent in our Join). From votv-coop.ini "net.nick".
void SetLocalNickname(const std::wstring& nick);

// Per-tick pump (game thread): on connect, announce ourselves (reliable Join with our
// nickname); on disconnect, post the peer's departure; drain delivered reliable
// messages (a peer Join -> "<nick> joined" + label the remote player). `remote` may be
// null before the puppet spawns.
// On disconnect it resets the announce flag so a reconnect re-announces.
void Update(net::Session& session, RemotePlayer* remote);

}  // namespace event_feed
}  // namespace coop
