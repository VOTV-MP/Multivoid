// coop/props/printer_sync.h -- a 3D print and its printed object, the host's (docs/kitchen-and-media.md).
//
// VOTV-MP/Multivoid#49. A printedObject_C is no prop_C; it builds its mesh from the player's own model file
// (coop/props/printer_models).
//   PRINT. Runs on the host only when every connected player holds the same model folder. A client's `print` is refused
//   on its copy and asked of the host (PrintAsk); the host asks each client for its hash (PrintCheck, PrintCheckReply,
//   5 s), then prints, or puts a chat line naming who lacks which model on the host, the asker and each lacking player.
//   OBJECT. The host gives it a keyless prop element, so the held-prop stream carries it, and sends its model, hash,
//   light, bites and rest pose (PrintedState). A client spawns it, or adopts its save-loaded copy by key, and builds it
//   from its own file when the hash matches; otherwise it keeps a hidden placeholder and tells the host, which puts the
//   chat line on the host and that player. A client's light toggle is asked of the host; its bite is told to the host.
// Game thread.
#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct PrintAskPayload;
struct PrintCheckPayload;
struct PrintCheckReplyPayload;
struct PrintNoticePayload;
struct PrintedStatePayload;
struct PrintedIntentPayload;
}  // namespace coop::net

namespace coop::printer_sync {

void Install(coop::net::Session* session);

// The watches, the host's enrolment and rest reads, its checks' deadlines. Once per pump tick.
void Tick();

void OnAsk(int senderSlot, const coop::net::PrintAskPayload& p);                 // host
void OnCheck(const coop::net::PrintCheckPayload& p);                             // client
void OnCheckReply(int senderSlot, const coop::net::PrintCheckReplyPayload& p);   // host
void OnNotice(const coop::net::PrintNoticePayload& p);                           // client
void OnState(const coop::net::PrintedStatePayload& p);                           // client
void OnIntent(int senderSlot, const coop::net::PrintedIntentPayload& p);         // host

// HOST: every printed object, to a joiner at its world-ready.
void SendJoin(int slot);

void OnDisconnect();

}  // namespace coop::printer_sync
