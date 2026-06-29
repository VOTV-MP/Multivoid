// coop/chat_sync.h -- the T-chat wire half (v60, ReliableKind::ChatMessage).
//
// User req 2026-06-11 ("chat on T, per rule 1"): a REAL peer chat. The UI half
// (ui/chat_input: the T-opened input bar, Enter sends, ESC closes) hands typed
// text to QueueSend; this module broadcasts it (host relays a client's line to
// the other clients -- the firefly relay shape) and every receiver pushes
// "<nick>: <text>" into coop::chat_feed (which already owns the display rules
// the user asked for: lines auto-expire after a TTL, so the feed disappears by
// itself when nobody has written for a while).
//
// Threading: QueueSend is callable from the RENDER thread (the ImGui input bar
// submits there) -- it GT::Post()s the actual send + local echo onto the game
// thread (Session::SendReliable + chat_feed::Push are game-thread paths).
// OnReliable runs on the game thread (event_feed drain).

#pragma once

#include <cstdint>
#include <string>

namespace coop::net {
class Session;
struct ChatMessagePayload;
}  // namespace coop::net

namespace coop::chat_sync {

// Store the session pointer (net_pump Install per pump tick; cheap).
void Install(coop::net::Session* session);

// True while a session exists + is connected -- the T key only opens the chat
// input during a coop session (chat is meaningless solo). Any thread.
bool SessionActive();

// Queue a chat line for send + local echo. UTF-8 in, trimmed + length-capped
// inside; empty/whitespace-only lines are dropped. RENDER-thread safe.
void QueueSend(const std::string& utf8Text);

// Receiver: a peer's chat line arrived (event_feed dispatch). Pushes
// "<nick>: <text>" into chat_feed. Game thread.
void OnReliable(const coop::net::ChatMessagePayload& payload, uint8_t senderPeerSlot);

// Session teardown: drop the session pointer.
void OnDisconnect();

}  // namespace coop::chat_sync
