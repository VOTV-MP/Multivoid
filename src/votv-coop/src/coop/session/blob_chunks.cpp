// coop/blob_chunks.cpp -- see coop/blob_chunks.h.

#include "coop/session/blob_chunks.h"

#include "coop/net/session.h"

#include "ue_wrap/log.h"

#include <cstring>

namespace coop::blob_chunks {

namespace {
// Shared chunker for SendBlob / SendBlobToSlot. `sendOne(p)` does the per-chunk transport
// (broadcast vs slot-targeted); everything else (chunk count, 255-cap, all-or-nothing) is
// identical (RULE 2 -- one chunking implementation).
template <class SendOne>
bool ChunkAndSend(coop::net::ReliableKind kind, uint32_t seq,
                  const std::vector<uint8_t>& blob, SendOne sendOne) {
    const size_t maxData = sizeof(coop::net::BlobChunkPayload{}.data);
    const uint8_t chunks =
        static_cast<uint8_t>(blob.empty() ? 1 : (blob.size() + maxData - 1) / maxData);
    if (blob.size() > maxData * 255) {
        UE_LOGW("blob_chunks: blob of %zu bytes exceeds the 255-chunk cap (kind %u) -- dropped",
                blob.size(), static_cast<unsigned>(kind));
        return false;
    }
    size_t off = 0;
    for (uint8_t i = 0; i < chunks; ++i) {
        coop::net::BlobChunkPayload p{};
        p.blobSeq = seq;
        p.chunkIdx = i;
        p.chunks = chunks;
        const size_t take = blob.size() - off > maxData ? maxData : blob.size() - off;
        p.chunkLen = static_cast<uint16_t>(take);
        if (take) std::memcpy(p.data, blob.data() + off, take);
        off += take;
        if (!sendOne(p)) {
            UE_LOGW("blob_chunks: chunk %u/%u send refused (kind %u, seq=%u) -- "
                    "blob will retry under a fresh seq",
                    static_cast<unsigned>(i), static_cast<unsigned>(chunks),
                    static_cast<unsigned>(kind), seq);
            return false;
        }
    }
    return true;
}
}  // namespace

bool SendBlob(coop::net::Session* s, coop::net::ReliableKind kind, uint32_t seq,
              const std::vector<uint8_t>& blob) {
    return ChunkAndSend(kind, seq, blob,
                        [&](const coop::net::BlobChunkPayload& p) {
                            return s->SendReliable(kind, &p, sizeof(p));
                        });
}

bool SendBlobToSlot(coop::net::Session* s, int peerSlot, coop::net::ReliableKind kind,
                    uint32_t seq, const std::vector<uint8_t>& blob) {
    return ChunkAndSend(kind, seq, blob,
                        [&](const coop::net::BlobChunkPayload& p) {
                            return s->SendReliableToSlot(peerSlot, kind, &p, sizeof(p));
                        });
}

bool Assembler::OnChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot,
                        std::vector<uint8_t>& outBlob) {
    if (p.chunks == 0 || p.chunkIdx >= p.chunks || p.chunkLen > sizeof(p.data))
        return false;

    const auto now = std::chrono::steady_clock::now();
    const auto key = std::make_pair(senderSlot, p.blobSeq);
    auto& a = map_[key];
    if (a.expectChunks == 0) {
        a.expectChunks = p.chunks;
        a.started = now;
        a.blob.reserve(static_cast<size_t>(p.chunks) * sizeof(p.data));
    }
    if (a.expectChunks != p.chunks || a.gotChunks != p.chunkIdx) {
        // Mismatched part: the C-1 restart semantics (see header).
        map_.erase(key);
        if (p.chunkIdx != 0) return false;
        auto& fresh = map_[key];
        fresh.expectChunks = p.chunks;
        fresh.started = now;
        fresh.blob.reserve(static_cast<size_t>(p.chunks) * sizeof(p.data));
        fresh.blob.insert(fresh.blob.end(), p.data, p.data + p.chunkLen);
        fresh.gotChunks = 1;
        if (fresh.gotChunks == fresh.expectChunks) {
            outBlob = std::move(fresh.blob);
            map_.erase(key);
            return true;
        }
        return false;
    }
    a.blob.insert(a.blob.end(), p.data, p.data + p.chunkLen);
    ++a.gotChunks;
    if (a.gotChunks < a.expectChunks) return false;

    outBlob = std::move(a.blob);
    map_.erase(key);
    return true;
}

void Assembler::Sweep(std::chrono::steady_clock::time_point now, std::chrono::seconds ttl) {
    for (auto it = map_.begin(); it != map_.end();) {
        if (now - it->second.started > ttl) it = map_.erase(it);
        else ++it;
    }
}

void Assembler::Clear() { map_.clear(); }

}  // namespace coop::blob_chunks
