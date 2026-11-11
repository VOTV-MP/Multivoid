// coop/props/container_slice_wire.cpp -- see coop/props/container_slice_wire.h.

#include "coop/props/container_slice_wire.h"

#include "coop/items/inventory_item_wire.h"
#include "coop/items/save_record_wire.h"
#include "coop/net/blob_chunks.h"

namespace coop::props::container_slice_wire {
namespace {

namespace II = ue_wrap::inventory_item;
namespace IW = coop::inventory_item_wire;
namespace W  = coop::save_record_wire;

constexpr uint8_t kOpContents = 0;
constexpr size_t kHeader = sizeof(uint8_t) + sizeof(uint32_t) + sizeof(uint16_t);   // op, eid and the count

}  // namespace

std::vector<uint8_t> Pack(uint32_t eid, const std::vector<II::Item>& recs) {
    std::vector<uint8_t> b;
    b.push_back(kOpContents);
    W::AppU32(b, eid);
    W::AppU16(b, static_cast<uint16_t>(recs.size()));
    for (const auto& r : recs) IW::SerItem(b, r);
    return b;
}

uint64_t RecordsHash(uint32_t eid, const std::vector<II::Item>& recs) { return coop::blob_chunks::Fnv64(Pack(eid, recs)); }

uint64_t RecordHash(const II::Item& r) {
    std::vector<uint8_t> b;
    IW::SerItem(b, r);
    return coop::blob_chunks::Fnv64(b);
}

uint64_t HashBytes(const std::vector<uint8_t>& recordWire) { return coop::blob_chunks::Fnv64(recordWire); }

size_t SliceBytes(const std::vector<II::Item>& recs) {
    size_t total = kHeader;
    std::vector<uint8_t> scratch;
    for (const II::Item& r : recs) {
        scratch.clear();
        IW::SerItem(scratch, r);
        total += scratch.size();
    }
    return total;
}

bool ParseHeader(const std::vector<uint8_t>& b, size_t& o, uint32_t& outEid, uint16_t& outCount) {
    uint8_t op = 0;
    if (!W::RdU8(b, o, op) || op != kOpContents) return false;
    return W::RdU32(b, o, outEid) && W::RdU16(b, o, outCount);
}

bool ParseRecords(const std::vector<uint8_t>& b, size_t& o, uint16_t count, std::vector<II::Item>& out,
                  const char** outWhy) {
    if (outWhy) *outWhy = "";
    if (count > kMaxRecords || !W::Feasible(count, b, o)) {
        if (outWhy) *outWhy = "the declared record count is past the cap or past the bytes";
        return false;
    }
    out.assign(count, II::Item{});
    for (auto& r : out) {
        if (!IW::DeItem(b, o, r)) {
            if (outWhy) *outWhy = "malformed record stream";
            out.clear();
            return false;
        }
    }
    return true;
}

}  // namespace coop::props::container_slice_wire
