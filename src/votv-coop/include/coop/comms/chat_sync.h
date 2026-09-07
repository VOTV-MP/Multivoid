// coop/chat_sync.h -- the T-chat wire half, HOST-AUTHORED.
//
// The UI half, ui/chat_input -- the T-opened input bar, Enter sends, ESC closes -- hands typed text
// to QueueSend, and every receiver renders "<nick>: <text>" through coop::chat_feed.
//
// The authority is the host, not the peers. A client's line reaches the host as an INTENT
// (ChatMessage, client to host only); the host commits it to the lobby's record in coop::chat_log
// with a monotone lineSeq and broadcasts an authored ChatLine -- preceded by a ChatSpeaker -- to
// every ready client INCLUDING the origin. The reason is the ORDER, and it is a threading fact
// rather than a preference: a relay fires on the NET thread at receive time, before the reliable
// inbox drains on the game thread, so there is no point on a relay path where a lineSeq exists to
// stamp. The commit and the broadcast have to be ONE act, at ONE authority, on ONE thread. MTA
// reaches the same shape from the other direction: CConsoleCommands.cpp broadcasts a player's own
// line back to them with no exclude argument, exactly as this does.

#pragma once

#include <cstdint>
#include <string>

namespace coop::net {
class Session;
struct ChatMessagePayload;
struct ChatLinePayload;
struct ChatSpeakerPayload;
}  // namespace coop::net

namespace coop::chat_sync {

// Store the session pointer (net_pump Install per pump tick; cheap).
void Install(coop::net::Session* session);

// True while a session exists + is connected -- the T key only opens the chat
// input during a coop session (chat is meaningless solo). Any thread.
bool SessionActive();

// Queue a chat line. UTF-8 in, trimmed and length-capped inside; an empty or whitespace-only line
// is dropped. On the HOST this commits and broadcasts; on a CLIENT it sends the intent and waits
// for the host's authored row, so a client's own line appears after one round trip rather than
// instantly. No optimistic echo is drawn: echoing locally and reconciling against the authored row
// by a client-side message id is purely additive, and stays unbuilt.
//
// RENDER-thread safe -- the ImGui input bar submits there, and this posts onto the game thread.
// Every OnReliable below runs on the game thread, in the event_feed drain.
void QueueSend(const std::string& utf8Text);

// HOST receiver: a client's chat INTENT. Validates, commits, broadcasts. A client that
// somehow receives one drops it -- nothing sends ChatMessage to a client any more.
// Game thread.
void OnReliable(const coop::net::ChatMessagePayload& payload, uint8_t senderPeerSlot);

// CLIENT receivers: the host's authored row, and the speaker binding that precedes it.
// Game thread.
void OnChatSpeaker(const coop::net::ChatSpeakerPayload& payload);
void OnChatLine(const coop::net::ChatLinePayload& payload);

// HOST: seed a joining peer with the lobby's chat record, oldest first, ONE reliable
// message per line -- never a blob, which would make this the fourth lane where a
// single packet can kill a joining client. Rides ConnectReplayForSlot at world-ready.
void QueueConnectBroadcastForSlot(int slot);

// HOST: a slot turned over -- it must be re-seeded before it hears anything live.
void OnSlotDisconnected(int slot);

// Session lifecycle: drop the record and the client's applied range. Called beside
// chat_feed::Reset() at BOTH of its sites. Game thread.
void Reset();

// Session teardown: drop the session pointer.
void OnDisconnect();

}  // namespace coop::chat_sync
