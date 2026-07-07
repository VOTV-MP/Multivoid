// coop/blob_chunks.h -- chunked variable-length blob transport over the
// reliable lane, shared by every chunked-row kind (EmailAppend v64,
// SavedSignalAppend + CompData v65). Extracted from coop/email_sync (RULE 2:
// one implementation) preserving its audited semantics verbatim:
//   I-3: SendBlob returns true only if EVERY chunk was accepted -- a partial
//        send must not be treated as sent; the caller retries the whole blob
//        next poll under a FRESH seq, and the receiver's dangling
//        half-assembly is TTL-swept.
//   C-1: a mismatched part (a rejoining sender reusing (slot,seq) against a
//        stale half-assembly) erases the stale entry and RESTARTS from the
//        incoming chunk when it is a stream start (chunkIdx==0), including
//        single-chunk completion; only a true mid-stream orphan drops.
//
// Also home to the content-hash primitive (FNV-1a 64 over a serialized blob)
// the delete mirrors key on.
//
// Game thread throughout.

#pragma once

#include "coop/net/protocol.h"

#include <chrono>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

namespace coop::net { class Session; }

namespace coop::blob_chunks {

// FNV-1a 64 -- the cross-peer content identity of a serialized row.
inline uint64_t Fnv64(const uint8_t* p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}
inline uint64_t Fnv64(const std::vector<uint8_t>& b) { return Fnv64(b.data(), b.size()); }

// Ship one blob as BlobChunkPayload chunks under `kind` with per-sender id
// `seq`. True only if every chunk was accepted (I-3).
bool SendBlob(coop::net::Session* s, coop::net::ReliableKind kind, uint32_t seq,
              const std::vector<uint8_t>& blob);

// Slot-targeted variant: ship the blob's chunks to ONE peer (peerSlot) via
// Session::SendReliableToSlot instead of the default route. Used by the HOST to send a
// specific client its per-player inventory (v73 Inc 4, host->client). Same all-or-nothing
// semantics as SendBlob (I-3). True only if every chunk was accepted by that slot.
bool SendBlobToSlot(coop::net::Session* s, int peerSlot, coop::net::ReliableKind kind,
                    uint32_t seq, const std::vector<uint8_t>& blob);

// Per-kind reassembler; each sync module owns one (keys are per-sender, and
// kinds ride separate Assembler instances, so seq spaces never collide).
class Assembler {
public:
    // Feed one chunk (validates the part fields). Returns true exactly when a
    // blob completed; the assembled bytes are moved into outBlob.
    bool OnChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot,
                 std::vector<uint8_t>& outBlob);

    // TTL-sweep stale half-assemblies (call ~1 Hz).
    void Sweep(std::chrono::steady_clock::time_point now, std::chrono::seconds ttl);

    void Clear();

private:
    struct Assembly {
        uint8_t expectChunks = 0, gotChunks = 0;
        std::vector<uint8_t> blob;
        std::chrono::steady_clock::time_point started{};
    };
    std::map<std::pair<uint8_t, uint32_t>, Assembly> map_;
};

}  // namespace coop::blob_chunks
