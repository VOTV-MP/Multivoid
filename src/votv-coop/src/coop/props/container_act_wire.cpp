// coop/props/container_act_wire.cpp -- see coop/props/container_act_wire.h.

#include "coop/props/container_act_wire.h"

#include "coop/items/save_record_wire.h"
#include "coop/net/protocol.h"

namespace coop::props::container_act_wire {
namespace {

namespace SR = ue_wrap::save_record;
namespace W  = coop::save_record_wire;
using coop::net::ContainerActFamily;

constexpr size_t kHeadBytes = 8;

void AppHead(std::vector<uint8_t>& b, uint16_t id, uint8_t second, uint32_t eid) {
    W::AppU16(b, id);
    W::AppU8(b, second);
    W::AppU8(b, 0);
    W::AppU32(b, eid);
}

// `[u16 n][n bytes]` of one record; false (nothing appended) when it is past the cap the parser refuses.
bool AppRecord(std::vector<uint8_t>& b, const SR::SaveRecord& r) {
    std::vector<uint8_t> rec;
    W::SerSave(rec, r);
    if (rec.size() > kMaxRecordBytes) return false;
    W::AppU16(b, static_cast<uint16_t>(rec.size()));
    b.insert(b.end(), rec.begin(), rec.end());
    return true;
}

bool Fail(const char** why, const char* text) {
    if (why) *why = text;
    return false;
}

// `[u16 n][n bytes]` to the end of the blob: one record that fills the rest exactly.
bool RdRecordToEnd(const std::vector<uint8_t>& b, size_t& o, SR::SaveRecord& out, const char** why) {
    uint16_t n = 0;
    if (!W::RdU16(b, o, n)) return Fail(why, "no record length");
    if (n == 0) return Fail(why, "empty record");
    if (n > kMaxRecordBytes) return Fail(why, "record past the byte cap");
    if (b.size() - o != n) return Fail(why, "record length does not fill the blob");
    if (!W::DeSave(b, o, out)) return Fail(why, "malformed record");
    if (o != b.size()) return Fail(why, "record shorter than its length");
    return true;
}

// The head's shared checks. `second` is the family (request) or the committed byte (commit).
bool RdHead(const std::vector<uint8_t>& b, size_t& o, uint16_t& id, uint8_t& second, uint32_t& eid,
            const char** why) {
    uint8_t pad = 0;
    if (b.size() < kHeadBytes) return Fail(why, "short head");
    W::RdU16(b, o, id);
    W::RdU8(b, o, second);
    W::RdU8(b, o, pad);
    W::RdU32(b, o, eid);
    if (id == 0) return Fail(why, "id 0");
    if (eid == 0) return Fail(why, "eid 0");
    if (pad != 0) return Fail(why, "pad not zero");
    return true;
}

}  // namespace

std::vector<uint8_t> RecordWire(const SR::SaveRecord& r) {
    std::vector<uint8_t> rec;
    W::SerSave(rec, r);
    return rec;
}

size_t RecordBytes(const SR::SaveRecord& r) { return RecordWire(r).size(); }

std::vector<uint8_t> PackRequest(const Request& r) {
    std::vector<uint8_t> b;
    AppHead(b, r.id, r.family, r.eid);
    switch (static_cast<ContainerActFamily>(r.family)) {
        case ContainerActFamily::Take:
            W::AppU16(b, r.index);
            W::AppU64(b, r.hash);
            break;
        case ContainerActFamily::PutRecord:
            if (!AppRecord(b, r.record)) return {};
            break;
        default:
            break;   // an unknown family is the head alone
    }
    return b;
}

Parse ParseRequest(const std::vector<uint8_t>& b, Request& out, const char** why) {
    if (why) *why = "";
    size_t o = 0;
    out = Request{};
    if (!RdHead(b, o, out.id, out.family, out.eid, why)) return Parse::Malformed;
    switch (static_cast<ContainerActFamily>(out.family)) {
        case ContainerActFamily::Take:
            if (!W::RdU16(b, o, out.index) || !W::RdU64(b, o, out.hash)) {
                Fail(why, "short take tail");
                return Parse::Malformed;
            }
            break;
        case ContainerActFamily::PutRecord:
            if (!RdRecordToEnd(b, o, out.record, why)) return Parse::Malformed;
            break;
        default:
            return Parse::UnknownFamily;
    }
    if (o != b.size()) {
        Fail(why, "bytes after the tail");
        return Parse::Malformed;
    }
    return Parse::Ok;
}

std::vector<uint8_t> PackCommit(const Commit& c) {
    std::vector<uint8_t> b;
    AppHead(b, c.id, c.committed, c.eid);
    if (c.hasRecord && c.committed == 1) {
        W::AppU16(b, c.index);
        if (!AppRecord(b, c.record)) return {};
    }
    return b;
}

Parse ParseCommit(const std::vector<uint8_t>& b, Commit& out, const char** why) {
    if (why) *why = "";
    size_t o = 0;
    out = Commit{};
    if (!RdHead(b, o, out.id, out.committed, out.eid, why)) return Parse::Malformed;
    if (out.committed > 1) {
        Fail(why, "committed past 1");
        return Parse::Malformed;
    }
    if (o == b.size()) return Parse::Ok;
    if (out.committed == 0) {
        Fail(why, "a tail on an abort");
        return Parse::Malformed;
    }
    if (!W::RdU16(b, o, out.index)) {
        Fail(why, "short record tail");
        return Parse::Malformed;
    }
    if (!RdRecordToEnd(b, o, out.record, why)) return Parse::Malformed;
    out.hasRecord = true;
    return Parse::Ok;
}

}  // namespace coop::props::container_act_wire
