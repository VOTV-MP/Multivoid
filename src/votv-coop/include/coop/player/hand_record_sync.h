// coop/player/hand_record_sync.h -- what another player's held item shows: its own save record on its hand mirror.
//
// A player's hand item reaches the others as a class and a name (HandItem), and each peer spawns a display-only mirror
// of it on the puppet from the class alone, so the mirror showed the class's defaults: a held can opened and half
// eaten was full and shut in the other players' view, an extinguisher's charge read full. Here the hand's owner reads
// its hand item's record through the item's own getData when the item or its record changes, at most four times a
// second, and sends it (HandRecord, chunked): a client to the host, which checks the sender's hand holds that class,
// applies it and sends it on to everyone; the host its own to everyone. Each peer writes it into its mirror of that
// hand through the item's own loadData (ue_wrap/actors/save_record's ApplyRecord, the leaf state only), again whenever
// the mirror is made anew, and a joiner gets every hand's record at its ready edge. The mirror stays display only; the
// hand's owner is the authority of its hand, as for HandItem. Game thread.
#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct BlobChunkPayload;
}  // namespace coop::net

namespace coop::hand_record_sync {

// Keep the session. Idempotent.
void Install(coop::net::Session* session);

// This peer's hand read and sent on change; every stored record written into its mirror. Per frame.
void Tick();

// HOST, at a joiner's world-ready edge: every hand's record, to that slot.
void SendJoinSnapshotForSlot(int slot);

// A chunk of a hand record, from `senderSlot`.
void OnChunk(const coop::net::BlobChunkPayload& c, uint8_t senderSlot);

// A peer left: its record and its half-assemblies go.
void OnPeerLeft(uint8_t slot);

// Session end.
void OnDisconnect();

}  // namespace coop::hand_record_sync
