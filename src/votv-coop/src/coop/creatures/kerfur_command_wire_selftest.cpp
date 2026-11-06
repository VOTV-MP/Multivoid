// coop/creatures/kerfur_command_wire_selftest.cpp -- the un-gated selftest of a kerfur command's bytes, run once per
// process (shape: coop/player/hand_history_selftest.cpp). Pure: no engine call, no session. The hostile contract of
// kerfur_command_wire.h is asserted line by line, since no honest run sends a forged blob: a head alone, a valid tail,
// and each way a blob is Malformed -- a short head, an unknown flag bit, a nonzero pad, a record on a command that takes
// none, a stray byte after the head and after the record, a record past the byte cap, a chunk header declaring more chunks
// than a command needs -- and the disc record's leaf bound.

#include "coop/creatures/kerfur_command_wire.h"

#include "coop/creatures/kerfur_command.h"
#include "coop/items/save_record_wire.h"
#include "coop/props/prop_record_wire.h"
#include "ue_wrap/actors/floppy_disc.h"
#include "ue_wrap/core/log.h"

#include <cstring>
#include <mutex>
#include <vector>

namespace coop::kerfur_command_wire {
namespace {

namespace prw = coop::props::prop_record_wire;
namespace SR  = ue_wrap::save_record;
using coop::kerfur_command::Command;

constexpr uint8_t kGetReports = static_cast<uint8_t>(Command::GetReports);

struct Tally {
    int pass = 0;
    int total = 0;
    void operator()(bool ok, const char* what) {
        ++total;
        if (ok) { ++pass; return; }
        UE_LOGE("kerfur_command_wire selftest FAIL: %s", what);
    }
};

SR::SaveRecord Disc(int32_t readWrites) {
    SR::SaveRecord r;
    r.className = L"prop_floppyDisc_C";
    r.key = L"k";
    r.xform = {0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f, 1.f, 1.f};
    r.names = {{L"floppy"}};
    r.ints = {{readWrites}};
    r.strings = {{L"data"}};
    return r;
}

std::vector<uint8_t> Blob(uint8_t command, uint8_t flags, uint8_t pad, const std::vector<uint8_t>& tail) {
    coop::net::KerfurCommandPayload head{};
    head.elementId = 7;
    head.requestId = 3;
    head.command = command;
    head.flags = flags;
    head._pad[0] = pad;
    std::vector<uint8_t> b(sizeof(head));
    std::memcpy(b.data(), &head, sizeof(head));
    b.insert(b.end(), tail.begin(), tail.end());
    return b;
}

std::vector<uint8_t> Tail(const SR::SaveRecord& r) {
    const std::vector<uint8_t> rec = prw::RecordWire(r);
    std::vector<uint8_t> t;
    coop::save_record_wire::AppU16(t, static_cast<uint16_t>(rec.size()));
    t.insert(t.end(), rec.begin(), rec.end());
    return t;
}

bool Malformed(const std::vector<uint8_t>& blob) {
    Request out;
    const char* why = "";
    return ParseBlob(blob, out, &why) == Parse::Malformed && why && why[0];
}

}  // namespace

bool RunSelftest() {
    static std::once_flag once;
    static bool verdict = false;
    std::call_once(once, [] {
        Tally check;

        // ---- a head alone, and a valid tail, round trip ----
        {
            Request in;
            in.head.elementId = 7;
            in.head.requestId = 3;
            in.head.command = static_cast<uint8_t>(Command::Follow);
            Request out;
            const std::vector<uint8_t> b = Pack(in);
            check(b.size() == kHeadBytes && ParseBlob(b, out, nullptr) == Parse::Ok && !out.hasRecord &&
                      out.head.requestId == 3 && out.head.command == in.head.command,
                  "head: a bare command is the head alone and parses back");
            in.head.command = kGetReports;
            in.hasRecord = true;
            in.record = Disc(5);
            const std::vector<uint8_t> t = Pack(in);
            Request back;
            check(!t.empty() && ParseBlob(t, back, nullptr) == Parse::Ok && back.hasRecord &&
                      back.record.className == in.record.className && back.record.ints == in.record.ints &&
                      back.head.flags == kFlagRecord,
                  "tail: a get_reports with a disc record parses back with its flag set and its record intact");
            in.head.command = static_cast<uint8_t>(Command::Follow);
            check(Pack(in).empty(), "tail: the packer builds no record on a command that takes none");
        }

        // ---- every Malformed ----
        const std::vector<uint8_t> tail = Tail(Disc(5));
        check(Malformed({}), "malformed: an empty blob");
        check(Malformed(std::vector<uint8_t>(kHeadBytes - 1, 0)), "malformed: a short head");
        check(Malformed(Blob(kGetReports, 0x02, 0, {})), "malformed: an unknown flag bit");
        check(Malformed(Blob(kGetReports, kFlagRecord | 0x04, 0, tail)), "malformed: an unknown flag bit beside the record flag");
        check(Malformed(Blob(kGetReports, 0, 1, {})), "malformed: a nonzero pad byte");
        check(Malformed(Blob(static_cast<uint8_t>(Command::Follow), kFlagRecord, 0, tail)),
              "malformed: a record on a command that takes none");
        check(Malformed(Blob(kGetReports, 0, 0, {0})), "malformed: a stray byte after a bare head");
        {
            std::vector<uint8_t> t = tail;
            t.push_back(0xAB);
            check(Malformed(Blob(kGetReports, kFlagRecord, 0, t)), "malformed: a stray byte after the record");
            std::vector<uint8_t> cut = tail;
            cut.pop_back();
            check(Malformed(Blob(kGetReports, kFlagRecord, 0, cut)), "malformed: a record shorter than its length");
            check(Malformed(Blob(kGetReports, kFlagRecord, 0, {})), "malformed: the record flag with no record");
            check(Malformed(Blob(kGetReports, kFlagRecord, 0, {0, 0})), "malformed: a zero-length record");
        }
        {
            SR::SaveRecord big = Disc(5);
            while (prw::RecordBytes(big) <= prw::kMaxRecordBytes) big.strings[0][0].push_back(L'x');
            check(Malformed(Blob(kGetReports, kFlagRecord, 0, Tail(big))), "malformed: a record past the byte cap");
            Request in;
            in.head.command = kGetReports;
            in.hasRecord = true;
            in.record = big;
            check(Pack(in).empty(), "malformed: the packer builds no record past the byte cap");
        }

        // ---- the chunk header bound ----
        {
            Request out;
            const char* why = "";
            coop::net::BlobChunkPayload c{};
            c.blobSeq = 1;
            c.chunks = static_cast<uint8_t>(kMaxChunks + 1);
            check(OnChunk(c, 1, out, &why) == Feed::Malformed,
                  "chunks: a header declaring more chunks than a command needs is refused at the first chunk");
            const std::vector<uint8_t> b = Blob(static_cast<uint8_t>(Command::Follow), 0, 0, {});
            coop::net::BlobChunkPayload one{};
            one.blobSeq = 2;
            one.chunks = 1;
            one.chunkLen = static_cast<uint16_t>(b.size());
            std::memcpy(one.data, b.data(), b.size());
            check(OnChunk(one, 1, out, &why) == Feed::Ok && out.head.requestId == 3,
                  "chunks: one chunk holding a head alone assembles and parses");
            check(kMaxChunks * kChunkBytes >= kMaxBlobBytes && (kMaxChunks - 1) * kChunkBytes < kMaxBlobBytes,
                  "chunks: the bound is the least that holds the ceiling");
            OnDisconnect();
        }

        // ---- the disc record's leaf values ----
        const char* why = "";
        check(ue_wrap::floppy_disc::DiscLeafOk(Disc(32), 32, &why), "leaf: readWrites at the class default is a disc");
        check(ue_wrap::floppy_disc::DiscLeafOk(Disc(0), 32, &why), "leaf: readWrites of zero is a spent disc");
        check(!ue_wrap::floppy_disc::DiscLeafOk(Disc(33), 32, &why), "leaf: readWrites past the class default");
        check(!ue_wrap::floppy_disc::DiscLeafOk(Disc(-1), 32, &why), "leaf: a negative readWrites");
        check(!ue_wrap::floppy_disc::DiscLeafOk(Disc(2147483647), 32, &why), "leaf: readWrites at INT_MAX");
        {
            SR::SaveRecord r = Disc(5);
            r.ints.clear();
            check(!ue_wrap::floppy_disc::DiscLeafOk(r, 32, &why), "leaf: a record with no ints group");
            r = Disc(5);
            r.strings.clear();
            check(!ue_wrap::floppy_disc::DiscLeafOk(r, 32, &why), "leaf: a record with no data array");
        }

        verdict = check.pass == check.total;
        if (verdict) UE_LOGI("kerfur_command_wire selftest: ALL PASS (%d checks)", check.total);
        else UE_LOGE("kerfur_command_wire selftest: %d of %d checks passed", check.pass, check.total);
    });
    return verdict;
}

}  // namespace coop::kerfur_command_wire
