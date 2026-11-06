// coop/props/prop_record_wire.cpp -- see coop/props/prop_record_wire.h.

#include "coop/props/prop_record_wire.h"

#include "coop/items/save_record_wire.h"

namespace coop::props::prop_record_wire {
namespace {

namespace SR = ue_wrap::save_record;
namespace W  = coop::save_record_wire;

bool Fail(const char** why, const char* text) {
    if (why) *why = text;
    return false;
}

}  // namespace

std::vector<uint8_t> RecordWire(const SR::SaveRecord& r) {
    std::vector<uint8_t> rec;
    W::SerSave(rec, r);
    return rec;
}

size_t RecordBytes(const SR::SaveRecord& r) { return RecordWire(r).size(); }

bool AppRecord(std::vector<uint8_t>& b, const SR::SaveRecord& r) {
    const std::vector<uint8_t> rec = RecordWire(r);
    if (rec.size() > kMaxRecordBytes) return false;
    W::AppU16(b, static_cast<uint16_t>(rec.size()));
    b.insert(b.end(), rec.begin(), rec.end());
    return true;
}

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

bool PutNameOk(const SR::SaveRecord& r) {
    std::wstring name;
    return SR::RecordName(r, name) && name.size() <= kMaxPutNameChars;
}

}  // namespace coop::props::prop_record_wire
