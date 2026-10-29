// coop/props/container_slice_wire.cpp -- see coop/props/container_slice_wire.h.

#include "coop/props/container_slice_wire.h"

#include "coop/items/save_record_wire.h"
#include "coop/net/blob_chunks.h"

namespace coop::props::container_slice_wire {
namespace {

namespace SR = ue_wrap::save_record;
namespace W  = coop::save_record_wire;

constexpr uint8_t kOpContents = 0;
constexpr size_t kFlagsOffset = sizeof(uint8_t) + sizeof(uint32_t) + sizeof(uint16_t);   // after op, eid and the count
constexpr size_t kHeader = kFlagsOffset + sizeof(uint8_t);

}  // namespace

std::vector<uint8_t> Pack(uint32_t eid, const std::vector<SR::SaveRecord>& recs, uint8_t flags) {
    std::vector<uint8_t> b;
    b.push_back(kOpContents);
    W::AppU32(b, eid);
    W::AppU16(b, static_cast<uint16_t>(recs.size()));
    W::AppU8(b, flags);
    for (const auto& r : recs) W::SerSave(b, r);
    return b;
}

uint64_t ContentHash(uint32_t eid, const std::vector<SR::SaveRecord>& recs, uint8_t flags) {
    return coop::blob_chunks::Fnv64(Pack(eid, recs, flags));
}

uint64_t RecordsHash(uint32_t eid, const std::vector<SR::SaveRecord>& recs) { return ContentHash(eid, recs, 0); }

uint64_t RecordHash(const SR::SaveRecord& r) {
    std::vector<uint8_t> b;
    W::SerSave(b, r);
    return coop::blob_chunks::Fnv64(b);
}

uint64_t HashBytes(const std::vector<uint8_t>& recordWire) { return coop::blob_chunks::Fnv64(recordWire); }

size_t SliceBytes(const std::vector<SR::SaveRecord>& recs) {
    size_t total = kHeader;
    std::vector<uint8_t> scratch;
    for (const SR::SaveRecord& r : recs) {
        scratch.clear();
        W::SerSave(scratch, r);
        total += scratch.size();
    }
    return total;
}

bool SetFlags(std::vector<uint8_t>& blob, uint8_t flags) {
    if (blob.size() < kHeader) return false;
    blob[kFlagsOffset] = flags;
    return true;
}

bool ParseHeader(const std::vector<uint8_t>& b, size_t& o, uint32_t& outEid, uint16_t& outCount, uint8_t& outFlags) {
    uint8_t op = 0;
    if (!W::RdU8(b, o, op) || op != kOpContents) return false;
    return W::RdU32(b, o, outEid) && W::RdU16(b, o, outCount) && W::RdU8(b, o, outFlags);
}

bool ParseRecords(const std::vector<uint8_t>& b, size_t& o, uint16_t count, std::vector<SR::SaveRecord>& out,
                  const char** outWhy) {
    if (outWhy) *outWhy = "";
    if (count > kMaxRecords || !W::Feasible(count, b, o)) {
        if (outWhy) *outWhy = "the declared record count is past the cap or past the bytes";
        return false;
    }
    out.assign(count, SR::SaveRecord{});
    for (auto& r : out) {
        if (!W::DeSave(b, o, r)) {
            if (outWhy) *outWhy = "malformed record stream";
            out.clear();
            return false;
        }
    }
    return true;
}

}  // namespace coop::props::container_slice_wire
