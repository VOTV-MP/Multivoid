// coop/creatures/kerfur_command_wire.cpp -- see coop/creatures/kerfur_command_wire.h.

#include "coop/creatures/kerfur_command_wire.h"

#include "coop/creatures/kerfur_command.h"
#include "coop/net/blob_chunks.h"

#include <chrono>
#include <cstring>

namespace coop::kerfur_command_wire {
namespace {

namespace prw = coop::props::prop_record_wire;

// A half-assembled command is dropped after this long, swept once a second.
constexpr std::chrono::seconds kAssemblyTtl{10};
constexpr std::chrono::seconds kSweepEvery{1};

coop::blob_chunks::Assembler g_asm;
std::chrono::steady_clock::time_point g_nextSweep{};

bool Fail(const char** why, const char* text) {
    if (why) *why = text;
    return false;
}

}  // namespace

std::vector<uint8_t> Pack(const Request& r) {
    if (r.hasRecord && r.head.command != static_cast<uint8_t>(coop::kerfur_command::Command::GetReports)) return {};
    coop::net::KerfurCommandPayload head = r.head;
    head.flags = r.hasRecord ? kFlagRecord : 0;
    head._pad[0] = head._pad[1] = 0;
    std::vector<uint8_t> b(kHeadBytes);
    std::memcpy(b.data(), &head, kHeadBytes);
    if (r.hasRecord && !prw::AppRecord(b, r.record)) return {};
    return b;
}

Parse ParseBlob(const std::vector<uint8_t>& blob, Request& out, const char** why) {
    if (why) *why = "";
    out = Request{};
    if (blob.size() < kHeadBytes) {
        Fail(why, "short head");
        return Parse::Malformed;
    }
    std::memcpy(&out.head, blob.data(), kHeadBytes);
    if (out.head.flags & ~kFlagRecord) {
        Fail(why, "unknown flag bits");
        return Parse::Malformed;
    }
    if (out.head._pad[0] != 0 || out.head._pad[1] != 0) {
        Fail(why, "pad not zero");
        return Parse::Malformed;
    }
    out.hasRecord = (out.head.flags & kFlagRecord) != 0;
    if (!out.hasRecord) {
        if (blob.size() != kHeadBytes) {
            Fail(why, "bytes after the head");
            return Parse::Malformed;
        }
        return Parse::Ok;
    }
    if (out.head.command != static_cast<uint8_t>(coop::kerfur_command::Command::GetReports)) {
        Fail(why, "a record on a command that takes none");
        return Parse::Malformed;
    }
    size_t o = kHeadBytes;
    if (!prw::RdRecordToEnd(blob, o, out.record, why)) return Parse::Malformed;
    return Parse::Ok;
}

bool Send(coop::net::Session* session, const Request& r) {
    const std::vector<uint8_t> blob = Pack(r);
    return !blob.empty() &&
           coop::blob_chunks::SendBlob(session, coop::net::ReliableKind::KerfurCommand, r.head.requestId, blob);
}

Feed OnChunk(const coop::net::BlobChunkPayload& chunk, uint8_t senderSlot, Request& out, const char** why) {
    // A header that declares more chunks than the ceiling needs is refused before the assembler reserves for it, as MTA's
    // latent receiver refuses a declared size at the first part (CLatentReceiver.cpp:128-129).
    if (chunk.chunks > kMaxChunks) {
        Fail(why, "header declares more chunks than a command needs");
        return Feed::Malformed;
    }
    std::vector<uint8_t> blob;
    if (!g_asm.OnChunk(chunk, senderSlot, blob)) return Feed::Incomplete;
    if (blob.size() > kMaxBlobBytes) {
        Fail(why, "blob past the ceiling");
        return Feed::Malformed;
    }
    return ParseBlob(blob, out, why) == Parse::Ok ? Feed::Ok : Feed::Malformed;
}

void Sweep() {
    if (g_asm.Idle()) return;
    const auto now = std::chrono::steady_clock::now();
    if (now < g_nextSweep) return;
    g_nextSweep = now + kSweepEvery;
    g_asm.Sweep(now, kAssemblyTtl);
}

void OnPeerGone(uint8_t slot) { g_asm.ClearSlot(slot); }

void OnDisconnect() { g_asm.Clear(); }

}  // namespace coop::kerfur_command_wire
