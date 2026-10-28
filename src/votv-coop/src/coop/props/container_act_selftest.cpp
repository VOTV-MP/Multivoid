// coop/props/container_act_selftest.cpp -- the un-gated selftest of a client's act on a world container, run once per
// session start: the codec (round trips and every refusal of a hostile blob), one record's hash, the index clamp, the
// record lookup, the holds, the host's judge and the order it reads in, the commit's rows and the client's reply
// decision, each from a stated input -- states no honest run lines up. Pure: no engine call, no session. The put's
// checks are container_act_selftest_put.cpp's, called at the end of RunSelftest.

#include "coop/items/save_record_wire.h"
#include "coop/props/container_act_host.h"
#include "coop/props/container_act_request.h"
#include "coop/props/container_act_selftest_internal.h"
#include "coop/props/container_act_state.h"
#include "coop/props/container_act_wire.h"
#include "coop/props/container_record_edit.h"
#include "coop/props/container_slice_wire.h"
#include "coop/props/container_truth.h"

#include "ue_wrap/core/log.h"

#include <cstring>
#include <string>
#include <vector>

namespace coop::props::container_act_host {

namespace H   = coop::props::container_act_host::internal;
namespace caw = coop::props::container_act_wire;
namespace cas = coop::props::container_act_state;
namespace cre = coop::props::container_record_edit;
namespace cr  = coop::props::container_reach;
namespace CS  = coop::props::container_truth;
namespace SR  = ue_wrap::save_record;

namespace selftest {

void Checks::operator()(bool ok, const char* what) {
    ++total;
    if (ok) { ++pass; return; }
    UE_LOGE("container_act_selftest FAIL: %s", what);
}

SR::SaveRecord Rec(const wchar_t* cls, int32_t n, const wchar_t* text) {
    SR::SaveRecord r;
    r.className = cls;
    r.key = L"key";
    r.xform = {0.f, 0.f, 0.f, 1.f, 10.f, 20.f, 30.f, 1.f, 1.f, 1.f};
    r.ints = {{n, 2}};
    r.floats = {{1.5f}};
    r.strings = {{text}};
    return r;
}

H::Inputs Ready() {
    H::Inputs in;
    in.worldReady = true;
    in.served = true;
    in.holdsWait = false;
    in.reach = cr::Reach::Reached;
    in.container = H::Container::Ready;
    in.found = true;
    return in;
}

cas::Hold MakeHold(cas::HoldKind kind, uint8_t slot, uint16_t id, uint32_t eid, uint64_t grantedMs) {
    cas::Hold h;
    h.kind = kind;
    h.slot = slot;
    h.id = id;
    h.eid = eid;
    h.index = 2;
    h.record = Rec(L"prop_x_C", 1, L"held");
    h.grantedMs = grantedMs;
    return h;
}

}  // namespace selftest

namespace {

using namespace selftest;
using coop::net::ContainerActFamily;
using coop::net::ContainerActReason;
using coop::net::ContainerActReplyPayload;

constexpr uint8_t kTake = static_cast<uint8_t>(ContainerActFamily::Take);
constexpr uint8_t kPutRecord = static_cast<uint8_t>(ContainerActFamily::PutRecord);
constexpr uint8_t kPutProp = static_cast<uint8_t>(ContainerActFamily::PutProp);

bool SameRecord(const SR::SaveRecord& a, const SR::SaveRecord& b) {
    std::vector<uint8_t> x, y;
    coop::save_record_wire::SerSave(x, a);
    coop::save_record_wire::SerSave(y, b);
    return x == y && a.className == b.className && a.key == b.key && a.ints == b.ints && a.strings == b.strings;
}

}  // namespace

void RunSelftest() {
    selftest::Checks check;

    // ---- the codec: round trips ----
    {
        caw::Request in;
        in.id = 7;
        in.family = kTake;
        in.eid = 9;
        in.index = 3;
        in.hash = 0x1122334455667788ull;
        caw::Request out;
        const char* why = "";
        check(caw::ParseRequest(caw::PackRequest(in), out, &why) == caw::Parse::Ok, "codec: a take parses");
        check(out.id == 7 && out.family == kTake && out.eid == 9 && out.index == 3 && out.hash == in.hash,
              "codec: a take round-trips field for field");

        in = caw::Request{};
        in.id = 8;
        in.family = kPutRecord;
        in.eid = 10;
        in.record = Rec(L"prop_apple_C", 4, L"juice");
        check(caw::ParseRequest(caw::PackRequest(in), out, &why) == caw::Parse::Ok, "codec: a put record parses");
        check(out.id == 8 && out.family == kPutRecord && out.eid == 10 && SameRecord(out.record, in.record),
              "codec: a put record round-trips field for field");

        in = caw::Request{};
        in.id = 9;
        in.family = kPutProp;
        in.eid = 11;
        in.propEid = 0xCAFE;
        check(caw::ParseRequest(caw::PackRequest(in), out, &why) == caw::Parse::Ok, "codec: a put prop parses");
        check(out.id == 9 && out.family == kPutProp && out.eid == 11 && out.propEid == 0xCAFE,
              "codec: a put prop round-trips field for field");

        caw::Commit c;
        c.id = 12;
        c.committed = 1;
        c.eid = 13;
        caw::Commit cout;
        check(caw::ParseCommit(caw::PackCommit(c), cout, &why) == caw::Parse::Ok && cout.id == 12 &&
                  cout.committed == 1 && cout.eid == 13 && !cout.hasRecord,
              "codec: a commit without a record round-trips");
        c.hasRecord = true;
        c.index = 5;
        c.record = Rec(L"prop_pear_C", 6, L"core");
        check(caw::ParseCommit(caw::PackCommit(c), cout, &why) == caw::Parse::Ok && cout.hasRecord &&
                  cout.index == 5 && SameRecord(cout.record, c.record),
              "codec: a commit with a record round-trips field for field");
        c.committed = 0;
        check(caw::PackCommit(c).size() == 8, "codec: an abort packs no record tail");
    }

    // ---- the codec: a hostile blob ----
    {
        caw::Request rq;
        caw::Commit cm;
        const char* why = "";
        caw::Request take;
        take.id = 7;
        take.family = kTake;
        take.eid = 9;
        take.index = 1;
        take.hash = 2;
        const std::vector<uint8_t> goodTake = caw::PackRequest(take);
        caw::Request put;
        put.id = 8;
        put.family = kPutRecord;
        put.eid = 10;
        put.record = Rec(L"prop_apple_C", 4, L"juice");
        const std::vector<uint8_t> goodPut = caw::PackRequest(put);

        std::vector<uint8_t> b(goodTake.begin(), goodTake.begin() + 7);
        check(caw::ParseRequest(b, rq, &why) == caw::Parse::Malformed, "codec: a short head is malformed");
        b = goodTake;
        b[0] = 0;
        b[1] = 0;
        check(caw::ParseRequest(b, rq, &why) == caw::Parse::Malformed, "codec: id 0 is malformed");
        b = goodTake;
        b[4] = b[5] = b[6] = b[7] = 0;
        check(caw::ParseRequest(b, rq, &why) == caw::Parse::Malformed, "codec: eid 0 is malformed");
        b = goodTake;
        b[3] = 1;
        check(caw::ParseRequest(b, rq, &why) == caw::Parse::Malformed, "codec: a pad byte is malformed");
        b = goodTake;
        b.push_back(0);
        check(caw::ParseRequest(b, rq, &why) == caw::Parse::Malformed, "codec: a byte after a take is malformed");
        b = goodTake;
        b.pop_back();
        check(caw::ParseRequest(b, rq, &why) == caw::Parse::Malformed, "codec: a short take tail is malformed");

        b = goodPut;
        b[8] = b[9] = 0;
        b.resize(10);
        check(caw::ParseRequest(b, rq, &why) == caw::Parse::Malformed, "codec: n == 0 is malformed");
        b = goodPut;
        ++b[8];
        check(caw::ParseRequest(b, rq, &why) == caw::Parse::Malformed,
              "codec: a record shorter than its n is malformed");
        b = goodPut;
        b.push_back(0);
        check(caw::ParseRequest(b, rq, &why) == caw::Parse::Malformed,
              "codec: one trailing byte after a record is malformed");
        b = goodPut;
        b[8] = 0xFF;
        b[9] = 0xFF;
        check(caw::ParseRequest(b, rq, &why) == caw::Parse::Malformed,
              "codec: n past the byte cap is malformed before any parse");

        // A valid record past the cap, packed by hand with its true n: refused for its size, not its shape.
        caw::Request big;
        big.id = 8;
        big.family = kPutRecord;
        big.eid = 10;
        big.record = Rec(L"prop_apple_C", 4, L"");
        big.record.strings = {{std::wstring(6000, L'x')}};
        std::vector<uint8_t> bigRec;
        coop::save_record_wire::SerSave(bigRec, big.record);
        check(bigRec.size() > caw::kMaxRecordBytes && bigRec.size() <= 0xFFFF,
              "codec: the long record is over the cap and still fits a u16 n");
        b.assign(goodPut.begin(), goodPut.begin() + 8);
        coop::save_record_wire::AppU16(b, static_cast<uint16_t>(bigRec.size()));
        b.insert(b.end(), bigRec.begin(), bigRec.end());
        check(caw::ParseRequest(b, rq, &why) == caw::Parse::Malformed && std::strcmp(why, "record past the byte cap") == 0,
              "codec: a valid record past the byte cap, with its true n, is malformed for its size");
        check(caw::PackRequest(big).empty(), "codec: a request with a record past the cap packs to nothing");

        // A real record and 4 junk bytes, n equal to the whole remainder: the record ends before its n does.
        b = goodPut;
        b[8] = static_cast<uint8_t>(b[8] + 4);
        b.insert(b.end(), {1, 2, 3, 4});
        check(caw::ParseRequest(b, rq, &why) == caw::Parse::Malformed &&
                  std::strcmp(why, "record shorter than its length") == 0,
              "codec: a record that leaves junk inside its n is malformed");

        caw::Commit good;
        good.id = 12;
        good.committed = 1;
        good.eid = 13;
        good.hasRecord = true;
        good.index = 1;
        good.record = Rec(L"prop_pear_C", 6, L"core");
        std::vector<uint8_t> gb = caw::PackCommit(good);
        gb[2] = 2;
        check(caw::ParseCommit(gb, cm, &why) == caw::Parse::Malformed, "codec: committed == 2 is malformed");
        gb[2] = 0;
        check(caw::ParseCommit(gb, cm, &why) == caw::Parse::Malformed,
              "codec: a tail on a committed == 0 commit is malformed");
        gb = caw::PackCommit(good);
        gb.push_back(0);
        check(caw::ParseCommit(gb, cm, &why) == caw::Parse::Malformed, "codec: a trailing byte on a commit is malformed");
        gb = caw::PackCommit(good);
        gb.resize(9);
        check(caw::ParseCommit(gb, cm, &why) == caw::Parse::Malformed, "codec: a cut commit tail is malformed");
        good.record = big.record;
        check(caw::PackCommit(good).empty(), "codec: a commit with a record past the cap packs to nothing");

        caw::Request unk;
        unk.id = 5;
        unk.family = 9;
        unk.eid = 6;
        check(caw::ParseRequest(caw::PackRequest(unk), rq, &why) == caw::Parse::UnknownFamily && rq.id == 5,
              "codec: family 9 is UnknownFamily with its id kept");
    }

    // ---- RecordHash, NeuterIfNested, ClampIndex ----
    {
        namespace csw = coop::props::container_slice_wire;
        const SR::SaveRecord a = Rec(L"prop_apple_C", 4, L"juice");
        SR::SaveRecord b = Rec(L"prop_apple_C", 4, L"juice");
        check(csw::RecordHash(a) == csw::RecordHash(b), "hash: equal records hash equal");
        b.strings[0][0] = L'k';
        check(csw::RecordHash(a) != csw::RecordHash(b), "hash: one changed character changes the hash");

        // A plain record is unchanged. A container record's index clears to -1 through the engine's class test,
        // which this pure run cannot ask.
        SR::SaveRecord plain;
        plain.ints = {{7}};
        cre::internal::NeuterIfNested(plain);
        check(plain.ints.size() == 1 && plain.ints[0].size() == 1 && plain.ints[0][0] == 7,
              "neuter: a record with no class is unchanged");

        check(cre::internal::ClampIndex(5, 3) == 3, "clamp: past the end clamps to the length");
        check(cre::internal::ClampIndex(2, 3) == 2, "clamp: inside the array is kept");
        check(cre::internal::ClampIndex(3, 3) == 3, "clamp: the length itself is the end");
        check(cre::internal::ClampIndex(65535, 0) == 0, "clamp: an empty array clamps to 0");
    }

    // ---- FindRecord ----
    {
        check(H::FindRecord({10, 20, 30}, 1, 20) == std::optional<size_t>(1), "find: the index matches");
        check(H::FindRecord({10, 20, 30}, 0, 20) == std::optional<size_t>(1), "find: a stale index finds the first match");
        check(H::FindRecord({5, 5, 7}, 1, 5) == std::optional<size_t>(1),
              "find: duplicate hashes give the index's when it matches");
        check(H::FindRecord({5, 5, 7}, 2, 5) == std::optional<size_t>(0),
              "find: duplicate hashes with a stale index give the first");
        check(!H::FindRecord({10, 20, 30}, 1, 99).has_value(), "find: no match is none");
        check(H::FindRecord({10, 20, 30}, 9, 30) == std::optional<size_t>(2), "find: an index past the end finds the hash");
        check(!H::FindRecord({10, 20, 30}, 9, 99).has_value(), "find: an index past the end and no match is none");
        check(!H::FindRecord({}, 0, 1).has_value(), "find: an empty container is none");
    }

    // ---- the holds ----
    {
        cas::State s;   // its own table: the host's live one is never touched
        check(s.Open(MakeHold(cas::HoldKind::Escrow, 1, 5, 100, 1000)), "state: a first open stands");
        check(!s.Open(MakeHold(cas::HoldKind::Escrow, 1, 6, 101, 1000)), "state: a second open by one slot is refused");
        check(s.Count() == 1, "state: the refused open added nothing");
        check(s.Match(1, 5) != nullptr && s.Count() == 1, "state: Match finds the hold and removes nothing");
        check(s.Match(1, 6) == nullptr && s.Match(2, 5) == nullptr, "state: Match needs the slot and the id");
        check(s.SlotHolds(1) && !s.SlotHolds(2), "state: SlotHolds names the slot");
        check(s.EscrowOn(100) && !s.EscrowOn(101), "state: EscrowOn names the container");
        check(!s.WaitsOnHolds(false, 100, 2), "wait: a take beside an escrow does not wait");
        check(s.WaitsOnHolds(true, 100, 2), "wait: a put beside an escrow waits");
        check(s.WaitsOnHolds(false, 999, 1), "wait: the holder's own next act waits");
        check(s.Open(MakeHold(cas::HoldKind::Reservation, 3, 9, 200, 31000)), "state: a reservation opens");
        check(s.WaitsOnHolds(false, 200, 2), "wait: any family beside another slot's reservation waits");
        check(s.ReservedByOther(200, 2) && !s.ReservedByOther(200, 3), "state: ReservedByOther excludes its own");
        check(!s.WaitsOnHolds(false, 300, 4), "wait: an unheld container does not wait");
        check(s.HeldEids().size() == 2, "state: HeldEids lists each container once");

        check(s.TakeExpired(30999).empty(), "state: a hold 1 ms short of its deadline stays");
        check(s.TakeExpired(31000).size() == 1 && s.Count() == 1, "state: a hold at its deadline expires");
        check(s.Open(MakeHold(cas::HoldKind::Escrow, 1, 6, 100, 1000)), "state: the freed slot opens again");
        check(s.TakeForSlot(3).size() == 1 && s.Count() == 1, "state: TakeForSlot takes the slot's hold");
        check(s.TakeForEid(100).size() == 1 && s.Count() == 0, "state: TakeForEid takes the container's holds");
        check(s.Open(MakeHold(cas::HoldKind::Escrow, 1, 7, 100, 0)) &&
                  s.Open(MakeHold(cas::HoldKind::Escrow, 2, 8, 101, 0)),
              "state: two slots hold at once");
        check(s.Close(1, 9) == std::nullopt, "state: Close of an unknown id is none");
        check(s.Close(1, 7).has_value() && s.Count() == 1, "state: Close removes and returns the match");
        check(s.TakeAll().size() == 1 && s.Count() == 0, "state: TakeAll empties the holds");
    }

    // ---- Decide and the order it reads in ----
    {
        check(H::Decide(Ready()) == H::Verdict::Grant, "decide: every read passing grants");
        H::Inputs in = Ready();
        in.worldReady = false;
        in.served = false;
        check(H::Decide(in) == H::Verdict::Wait, "decide: not ready beats WrongFamily");
        in = Ready();
        in.served = false;
        in.holdsWait = true;
        check(H::Decide(in) == H::Verdict::WrongFamily, "decide: WrongFamily beats the holds");
        in = Ready();
        in.holdsWait = true;
        in.reach = cr::Reach::Unreachable;
        check(H::Decide(in) == H::Verdict::Wait, "decide: the holds beat the reach");
        in = Ready();
        in.reach = cr::Reach::NoBody;
        in.container = H::Container::Gone;
        check(H::Decide(in) == H::Verdict::Wait, "decide: NoBody beats Gone");
        in = Ready();
        in.reach = cr::Reach::Unreachable;
        in.container = H::Container::Gone;
        check(H::Decide(in) == H::Verdict::Gone, "decide: Gone beats Unreachable");
        in = Ready();
        in.reach = cr::Reach::Unreachable;
        in.container = H::Container::NotReady;
        check(H::Decide(in) == H::Verdict::Wait, "decide: a container not ready waits, even out of reach");
        in = Ready();
        in.container = H::Container::NotReady;
        check(H::Decide(in) == H::Verdict::Wait, "decide: a container not ready waits");
        in = Ready();
        in.reach = cr::Reach::Unreachable;
        check(H::Decide(in) == H::Verdict::Reach, "decide: out of reach is refused");
        in = Ready();
        in.found = false;
        check(H::Decide(in) == H::Verdict::Gone, "decide: a record not found is gone");
    }

    // ---- the commit ----
    {
        cas::State s;
        s.Open(MakeHold(cas::HoldKind::Escrow, 1, 5, 100, 1000));
        check(H::CommitMatches(s.Match(1, 5), 100, false, 1), "commit: the right id and eid match");
        check(!H::CommitMatches(s.Match(1, 5), 101, false, 1) && s.Count() == 1,
              "commit: the right id and a wrong eid do not match and the hold stands");
        check(!H::CommitMatches(s.Match(1, 5), 100, true, 1), "commit: a record on an escrow commit does not match");
        check(!H::CommitMatches(s.Match(1, 6), 100, false, 1), "commit: an unknown id does not match");
        s.TakeAll();
        s.Open(MakeHold(cas::HoldKind::Reservation, 1, 5, 100, 1000));
        check(!H::CommitMatches(s.Match(1, 5), 100, false, 1),
              "commit: a reservation done without a record does not match");
        s.TakeAll();

        check(H::DecideCommit(false, 1) == H::CommitStep::BadCommit, "commit: unmatched and committed is bad");
        check(H::DecideCommit(false, 0) == H::CommitStep::BadCommit, "commit: unmatched and aborted is bad");
        check(H::DecideCommit(true, 1) == H::CommitStep::Done, "commit: matched and committed is done");
        check(H::DecideCommit(true, 0) == H::CommitStep::Abort, "commit: matched and aborted aborts");

        check(!CS::TruthDue(1000, 2999), "truth: 1999 ms after a send is not due");
        check(CS::TruthDue(1000, 3000), "truth: 2000 ms after a send is due");
        check(CS::SlotTruthDue(0, 1), "truth: a slot that was sent none is due");
        check(!CS::SlotTruthDue(1000, 1499), "truth: 499 ms after any send to the slot is not due");
        check(CS::SlotTruthDue(1000, 1500), "truth: 500 ms after any send to the slot is due");

        CS::RefusalOnce once;
        bool allSaid = true;
        for (uint32_t i = 0; i < CS::kMaxRefusalLines; ++i)
            allSaid = allSaid && once.Note(i) == CS::RefusalOnce::Say::Refusal;
        check(allSaid, "truth: the first 256 refused containers are each said");
        check(once.Note(0) == CS::RefusalOnce::Say::Quiet, "truth: a container already said stays quiet");
        check(once.Note(1000) == CS::RefusalOnce::Say::Capped, "truth: the 257th container is answered capped");
        check(once.Note(1001) == CS::RefusalOnce::Say::Quiet, "truth: the cap is said once");
        check(once.Note(1000) == CS::RefusalOnce::Say::Quiet, "truth: a capped container stays quiet");

        check(!H::WaitExpired(1000, 15999), "wait: a head 1 ms short of the bound stays");
        check(H::WaitExpired(1000, 16000), "wait: a head at the bound is ended");
    }

    // ---- the client's reply decision: (a record is open, its id, the reply) ----
    {
        namespace RQ = coop::props::container_act_request::internal;
        check(RQ::DecideReply(false, 5, ContainerActReplyPayload{5, 1, 0}) == RQ::ReplyAction::Stale,
              "decide-reply: no open record is stale");
        check(RQ::DecideReply(true, 5, ContainerActReplyPayload{6, 1, 0}) == RQ::ReplyAction::Stale,
              "decide-reply: a foreign id is stale");
        check(RQ::DecideReply(true, 5, ContainerActReplyPayload{5, 1, 0}) == RQ::ReplyAction::Granted,
              "decide-reply: a matching grant is granted");
        for (uint8_t reason = 1; reason < coop::net::kContainerActReasons; ++reason) {
            check(RQ::DecideReply(true, 5, ContainerActReplyPayload{5, 0, reason}) == RQ::ReplyAction::Refused,
                  "decide-reply: a refusal with reason 1..8 is refused");
        }
        check(static_cast<uint8_t>(ContainerActReason::Lapsed) == 8 && coop::net::kContainerActReasons == 9 &&
                  RQ::DecideReply(true, 5, ContainerActReplyPayload{5, 0, 8}) == RQ::ReplyAction::Refused,
              "decide-reply: Lapsed is reason 8, the ninth counted, and a refusal");
        check(RQ::DecideReply(true, 5, ContainerActReplyPayload{5, 0, 0}) == RQ::ReplyAction::BadReason,
              "decide-reply: not granted with reason 0 is a bad reason");
        check(RQ::DecideReply(true, 5, ContainerActReplyPayload{5, 0, coop::net::kContainerActReasons}) ==
                  RQ::ReplyAction::BadReason,
              "decide-reply: the reason past the last is a bad reason");
        check(RQ::DecideReply(true, 5, ContainerActReplyPayload{5, 1, 3}) == RQ::ReplyAction::BadReason,
              "decide-reply: granted with a reason is a bad reason");
    }

    // ---- the take's replay: the check before the press, and the commit after it ----
    {
        namespace RQ = coop::props::container_act_request::internal;
        check(RQ::CheckTake(true, true, true, true, true) == RQ::TakeCheck::Replay, "check-take: all true replays");
        check(RQ::CheckTake(true, false, true, true, true) == RQ::TakeCheck::Abort, "check-take: a dead slot aborts");
        check(RQ::CheckTake(true, true, false, true, true) == RQ::TakeCheck::Abort,
              "check-take: another container on the screen aborts");
        check(RQ::CheckTake(true, true, true, false, true) == RQ::TakeCheck::Abort, "check-take: another id aborts");
        check(RQ::CheckTake(true, true, true, true, false) == RQ::TakeCheck::Abort, "check-take: another record aborts");
        check(RQ::CheckTake(false, true, true, true, true) == RQ::TakeCheck::Abort, "check-take: a stale grant aborts");
        check(RQ::TakeCommitted(false, true, 3, 2) == 0, "take-committed: a press that did not run is abort");
        check(RQ::TakeCommitted(true, true, 3, 3) == 0, "take-committed: a press that took nothing is abort");
        check(RQ::TakeCommitted(true, true, 3, 2) == 1, "take-committed: one record fewer is done");
        check(RQ::TakeCommitted(true, true, 3, 1) == 1, "take-committed: two records fewer is done");
        check(RQ::TakeCommitted(true, false, 3, 0) == 1, "take-committed: a press whose count was unread is done");
    }

    RunPutSelftest(check);

    if (check.pass == check.total) UE_LOGI("container_act_selftest: ALL PASS (%d checks)", check.total);
}

}  // namespace coop::props::container_act_host
