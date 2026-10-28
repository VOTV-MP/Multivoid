// coop/props/container_slice_wire.cpp -- see coop/props/container_slice_wire.h.

#include "coop/props/container_slice_wire.h"

#include "coop/items/save_record_wire.h"
#include "coop/net/blob_chunks.h"

namespace coop::props::container_slice_wire {
namespace {

namespace SR = ue_wrap::save_record;
namespace W  = coop::save_record_wire;

constexpr uint8_t kOpContents = 0;

}  // namespace

std::vector<uint8_t> Pack(uint32_t eid, uint64_t baseHash,
                          const std::vector<SR::SaveRecord>& recs) {
    std::vector<uint8_t> b;
    b.push_back(kOpContents);
    W::AppU32(b, eid);
    W::AppU64(b, baseHash);
    W::AppU16(b, static_cast<uint16_t>(recs.size()));
    for (const auto& r : recs) W::SerSave(b, r);
    return b;
}

uint64_t ContentHash(uint32_t eid, const std::vector<SR::SaveRecord>& recs) {
    return coop::blob_chunks::Fnv64(Pack(eid, 0, recs));
}

uint64_t RecordHash(const SR::SaveRecord& r) {
    std::vector<uint8_t> b;
    W::SerSave(b, r);
    return coop::blob_chunks::Fnv64(b);
}

bool ParseHeader(const std::vector<uint8_t>& b, size_t& o, uint32_t& outEid,
                 uint64_t& outBaseHash) {
    uint8_t op = 0;
    if (!W::RdU8(b, o, op) || op != kOpContents) return false;
    if (!W::RdU32(b, o, outEid)) return false;
    return W::RdU64(b, o, outBaseHash);
}

bool ParseRecords(const std::vector<uint8_t>& b, size_t& o, std::vector<SR::SaveRecord>& out,
                  const char** outWhy) {
    if (outWhy) *outWhy = "";
    uint16_t n = 0;
    if (!W::RdU16(b, o, n)) {
        if (outWhy) *outWhy = "no record count";
        return false;
    }
    if (n > kMaxRecords || !W::Feasible(n, b, o)) {
        if (outWhy) *outWhy = "the declared record count is past the cap or past the bytes";
        return false;
    }
    out.assign(n, SR::SaveRecord{});
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
