// coop/props/container_act_selftest.cpp -- the un-gated selftest of a client's act on a world container, run once per
// session start: the codec (round trips and every refusal of a hostile blob), one record's hash, the index clamp, the
// record lookup, the holds, the host's judge and the order it reads in, the commit's rows and the client's reply
// decision, each from a stated input -- states no honest run lines up. Pure: no engine call, no session.

#include "coop/items/save_record_wire.h"
#include "coop/props/container_act_host.h"
#include "coop/props/container_act_request.h"
#include "coop/props/container_act_state.h"
#include "coop/props/container_act_wire.h"
#include "coop/props/container_record_edit.h"
#include "coop/props/container_slice_wire.h"

#include "ue_wrap/core/log.h"

#include <cstring>
#include <string>
#include <vector>

namespace coop::props::container_act_host {

namespace {

namespace H   = coop::props::container_act_host::internal;
namespace caw = coop::props::container_act_wire;
namespace cas = coop::props::container_act_state;
namespace cre = coop::props::container_record_edit;
namespace cr  = coop::props::container_reach;
namespace SR  = ue_wrap::save_record;
using coop::net::ContainerActFamily;
using coop::net::ContainerActReason;
using coop::net::ContainerActReplyPayload;

constexpr uint8_t kTake = static_cast<uint8_t>(ContainerActFamily::Take);
constexpr uint8_t kPutRecord = static_cast<uint8_t>(ContainerActFamily::PutRecord);
constexpr uint8_t kPutProp = static_cast<uint8_t>(ContainerActFamily::PutProp);

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

bool SameRecord(const SR::SaveRecord& a, const SR::SaveRecord& b) {
    std::vector<uint8_t> x, y;
    coop::save_record_wire::SerSave(x, a);
    coop::save_record_wire::SerSave(y, b);
    return x == y && a.className == b.className && a.key == b.key && a.ints == b.ints && a.strings == b.strings;
}

// Every read passes: the grant. A check breaks one field to reach one branch.
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

}  // namespace

void RunSelftest() {
    int pass = 0, total = 0;
    auto check = [&](bool ok, const char* what) {
        ++total;
        if (ok) { ++pass; return; }
        UE_LOGE("container_act_selftest FAIL: %s", what);
    };

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

        check(!H::TruthDue(1000, 2999), "truth: 1999 ms after a send is not due");
        check(H::TruthDue(1000, 3000), "truth: 2000 ms after a send is due");
        check(H::SlotTruthDue(0, 1), "truth: a slot that was sent none is due");
        check(!H::SlotTruthDue(1000, 1499), "truth: 499 ms after any send to the slot is not due");
        check(H::SlotTruthDue(1000, 1500), "truth: 500 ms after any send to the slot is due");

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

    // ---- the put's replay: the guards before the verb, and where the container stored the record ----
    {
        namespace RQ = coop::props::container_act_request::internal;
        check(RQ::CheckPutR2(true, true, true, true, true, true) == RQ::PutCheck::Replay, "check-put-r2: all true replays");
        check(RQ::CheckPutR2(false, true, true, true, true, true) == RQ::PutCheck::Abort,
              "check-put-r2: a dead container aborts");
        check(RQ::CheckPutR2(true, false, true, true, true, true) == RQ::PutCheck::Abort,
              "check-put-r2: another container on the screen aborts");
        check(RQ::CheckPutR2(true, true, false, true, true, true) == RQ::PutCheck::Abort,
              "check-put-r2: a hovered slot aborts");
        check(RQ::CheckPutR2(true, true, true, false, true, true) == RQ::PutCheck::Abort,
              "check-put-r2: a disabled take button aborts");
        check(RQ::CheckPutR2(true, true, true, true, false, true) == RQ::PutCheck::Abort,
              "check-put-r2: another selected index aborts");
        check(RQ::CheckPutR2(true, true, true, true, true, false) == RQ::PutCheck::Abort,
              "check-put-r2: another record aborts");
        check(RQ::CheckPutR3(true, true, true) == RQ::PutCheck::Replay, "check-put-r3: all true replays");
        check(RQ::CheckPutR3(false, true, true) == RQ::PutCheck::Abort, "check-put-r3: a dead container aborts");
        check(RQ::CheckPutR3(true, false, true) == RQ::PutCheck::Abort, "check-put-r3: another hand actor aborts");
        check(RQ::CheckPutR3(true, true, false) == RQ::PutCheck::Abort, "check-put-r3: another record aborts");

        // A..C are three different records by hash; X is the one the verb stored.
        constexpr uint64_t A = 10, B = 20, C = 30, X = 99;
        check(RQ::StoredIndex({A, B}, {X, A, B}) == std::optional<size_t>(0), "stored-index: at the front");
        check(RQ::StoredIndex({A, B, C}, {A, X, B, C}) == std::optional<size_t>(1), "stored-index: in the middle");
        check(RQ::StoredIndex({A, B}, {A, B, X}) == std::optional<size_t>(2), "stored-index: at the end");
        check(RQ::StoredIndex({}, {X}) == std::optional<size_t>(0), "stored-index: into an empty container");
        check(RQ::StoredIndex({A, B}, {A, A, B}) == std::optional<size_t>(1),
              "stored-index: beside an identical record the first difference is the index");
        check(RQ::StoredIndex({A, B}, {A, B, B}) == std::optional<size_t>(2),
              "stored-index: an identical record at the end lands at the end");
        check(!RQ::StoredIndex({A, B}, {A, B}).has_value(), "stored-index: a count that did not rise is none");
        check(!RQ::StoredIndex({A, B}, {A, X, C, B}).has_value(), "stored-index: a count that rose by two is none");
        check(!RQ::StoredIndex({A, B}, {A, X, C}).has_value(), "stored-index: a rest that does not line up is none");
        check(!RQ::StoredIndex({A, B, C}, {A, B}).has_value(), "stored-index: a count that fell is none");
        check(!RQ::StoredIndex({}, {}).has_value(), "stored-index: two empty containers are none");
    }

    // ---- the put's judge: every term and the order it reads in ----
    {
        const auto putReady = [] {
            H::PutInputs p;
            p.base = Ready();
            p.maxVol = 100.f;
            p.currVol = 40.f;
            p.volume = 10.f;
            return p;
        };
        Tally why = Tally::kCount;
        H::PutInputs p = putReady();
        check(H::DecidePut(p, &why) == H::Verdict::Grant && why == Tally::kCount, "put: every term passing grants");
        p.base.worldReady = false;
        check(H::DecidePut(p) == H::Verdict::Wait, "put: a world not ready waits");
        p = putReady();
        p.base.holdsWait = true;
        p.locked = true;
        check(H::DecidePut(p) == H::Verdict::Wait, "put: the holds beat a locked container");
        p = putReady();
        p.base.reach = cr::Reach::NoBody;
        p.base.container = H::Container::Gone;
        check(H::DecidePut(p) == H::Verdict::Wait, "put: NoBody beats Gone");
        p = putReady();
        p.base.reach = cr::Reach::Unreachable;
        p.base.container = H::Container::Gone;
        check(H::DecidePut(p) == H::Verdict::Gone, "put: Gone beats Unreachable");
        p = putReady();
        p.base.container = H::Container::NotReady;
        check(H::DecidePut(p) == H::Verdict::Wait, "put: a container not ready waits");
        p = putReady();
        p.base.reach = cr::Reach::Unreachable;
        p.termsRead = false;
        check(H::DecidePut(p) == H::Verdict::Reach, "put: out of reach beats the terms");
        p = putReady();
        p.termsRead = false;
        p.locked = true;
        check(H::DecidePut(p) == H::Verdict::Wait, "put: terms that did not read wait");
        p = putReady();
        p.locked = true;
        p.slowed = true;
        p.nameOk = false;
        check(H::DecidePut(p) == H::Verdict::Locked, "put: a locked container beats the slowed sender");
        p = putReady();
        p.slowed = true;
        p.nameOk = false;
        check(H::DecidePut(p) == H::Verdict::Lapsed, "put: a slowed sender is told Lapsed, before the record is read");
        p = putReady();
        p.nameOk = false;
        p.classResolved = false;
        check(H::DecidePut(p, &why) == H::Verdict::Refused && why == Tally::PutNoName,
              "put: a record with no name is refused, counted put_no_name");
        p = putReady();
        p.classResolved = false;
        p.propClass = false;
        check(H::DecidePut(p, &why) == H::Verdict::Refused && why == Tally::PutClassUnresolved,
              "put: a class that does not resolve is refused, counted put_class_unresolved");
        p = putReady();
        p.propClass = false;
        p.filterHolds = true;
        check(H::DecidePut(p, &why) == H::Verdict::Refused && why == Tally::PutNotProp,
              "put: a class that is no prop is refused, counted put_not_prop");
        p = putReady();
        p.filterRead = false;
        p.mailboxContainer = true;
        check(H::DecidePut(p) == H::Verdict::Wait, "put: a filter that did not read waits");
        p = putReady();
        p.filterHolds = true;
        p.mailboxContainer = true;
        check(H::DecidePut(p, &why) == H::Verdict::Refused && why == Tally::PutFiltered,
              "put: a filtered class is refused, counted put_filtered");
        p = putReady();
        p.mailboxContainer = true;
        p.rowRefuses = true;
        check(H::DecidePut(p, &why) == H::Verdict::Refused && why == Tally::PutIntoMailbox,
              "put: a container into a mailbox is refused, counted put_into_mailbox");
        p = putReady();
        p.rowRead = false;
        p.countAtCap = true;
        check(H::DecidePut(p) == H::Verdict::Wait, "put: a list row that did not read waits");
        p = putReady();
        p.rowRefuses = true;
        p.countAtCap = true;
        check(H::DecidePut(p, &why) == H::Verdict::Refused && why == Tally::PutRowRefuses,
              "put: a row that is not collectable or heavy is refused, counted put_row_refuses");
        p = putReady();
        p.countAtCap = true;
        p.volumeKnown = false;
        check(H::DecidePut(p, &why) == H::Verdict::NoSpace && why == Tally::kCount,
              "put: the record cap is NoSpace, and beats an unknown name");
        p = putReady();
        p.bytesOver = true;
        check(H::DecidePut(p) == H::Verdict::NoSpace, "put: the byte ceiling is NoSpace");
        p = putReady();
        p.volumeRead = false;
        check(H::DecidePut(p) == H::Verdict::Wait, "put: a volume that did not read waits");
        p = putReady();
        p.volumeKnown = false;
        check(H::DecidePut(p, &why) == H::Verdict::Refused && why == Tally::PutUnknownName,
              "put: an unknown name is refused, counted put_unknown_name");
        p = putReady();
        p.volume = 60.f;
        check(H::DecidePut(p) == H::Verdict::Grant, "put: a volume that exactly fills the container fits");
        p.volume = 60.5f;
        check(H::DecidePut(p) == H::Verdict::NoSpace, "put: a volume past the container's room is NoSpace");
        p.infinite = true;
        p.volumeRead = false;
        p.volumeKnown = false;
        check(H::DecidePut(p) == H::Verdict::Grant, "put: an infinite container reads no volume and grants");
    }

    // ---- the put's commit: the hold's rule, the stored record, the record's name ----
    {
        cas::State s;
        s.Open(MakeHold(cas::HoldKind::Reservation, 1, 5, 100, 1000));
        check(H::CommitMatches(s.Match(1, 5), 100, true, 1), "put-commit: a reservation's done with a record matches");
        check(!H::CommitMatches(s.Match(1, 5), 100, false, 1),
              "put-commit: a reservation's done without a record does not match");
        check(H::CommitMatches(s.Match(1, 5), 100, false, 0), "put-commit: a reservation's abort matches");
        check(!H::CommitMatches(s.Match(1, 5), 100, true, 0), "put-commit: a reservation's abort with a record does not match");
        check(!H::CommitMatches(s.Match(1, 5), 101, true, 1), "put-commit: a reservation named by another eid does not match");
        check(!H::CommitMatches(s.Match(1, 6), 100, true, 1), "put-commit: an unknown id does not match");
        s.TakeAll();
        s.Open(MakeHold(cas::HoldKind::Escrow, 1, 5, 100, 1000));
        check(H::CommitMatches(s.Match(1, 5), 100, false, 0), "put-commit: an escrow's abort without a record matches");
        check(!H::CommitMatches(s.Match(1, 5), 100, true, 1), "put-commit: an escrow's done with a record does not match");

        SR::SaveRecord asked = Rec(L"prop_apple_C", 1, L"x");
        asked.names = {{L"apple", L"tag"}};
        SR::SaveRecord stored = asked;
        stored.bools = {{1}};   // the container appended its fridge flag
        check(H::StoredMatches(asked, stored), "stored: the same class and name match whatever else the container added");
        stored.className = L"prop_pear_C";
        check(!H::StoredMatches(asked, stored), "stored: another class does not match");
        stored = asked;
        stored.names = {{L"pear", L"tag"}};
        check(!H::StoredMatches(asked, stored), "stored: another name does not match");
        stored = asked;
        stored.names.clear();
        check(!H::StoredMatches(asked, stored), "stored: a stored record with no name does not match");
        check(!H::StoredMatches(stored, asked), "stored: an asked record with no name does not match");

        std::wstring name;
        SR::SaveRecord bare;
        check(!SR::RecordName(bare, name), "name: a record with no names group has none");
        bare.names = {{}};
        check(!SR::RecordName(bare, name), "name: an empty names group has none");
        check(SR::RecordName(asked, name) && name == L"apple", "name: names[0][0] is the prop's name");
    }

    // ---- the put's byte slack: the largest record a client may ask still packs once the fridge flag is added ----
    {
        const size_t limit = caw::kMaxRecordBytes - caw::kPutStoredSlack;
        // `r` padded in its string to the largest size a client may ask (the grammar's strings step by two bytes).
        const auto largest = [&](SR::SaveRecord r) {
            r.names = {{L"apple", L"tag"}};
            r.strings = {{L""}};
            r.strings[0][0].assign((limit - caw::RecordBytes(r)) / 2, L'x');
            return r;
        };
        // The commit that carries `r` as the container stored it.
        const auto commitOf = [](const SR::SaveRecord& r) {
            caw::Commit c;
            c.id = 1;
            c.committed = 1;
            c.eid = 2;
            c.hasRecord = true;
            c.index = 0;
            c.record = r;
            return c;
        };
        caw::Commit back;
        const char* why = "";

        SR::SaveRecord noGroup = largest(Rec(L"prop_apple_C", 1, L""));
        check(caw::RecordBytes(noGroup) <= limit && limit - caw::RecordBytes(noGroup) <= 1,
              "slack: the test record is the largest the client may ask");
        noGroup.bools.clear();
        noGroup.bools.push_back({1});   // a record with no bools group gains one: the dearest the flag can cost
        check(!caw::PackCommit(commitOf(noGroup)).empty() &&
                  caw::ParseCommit(caw::PackCommit(commitOf(noGroup)), back, &why) == caw::Parse::Ok,
              "slack: the largest record with a new bools group packs into a commit and parses");

        SR::SaveRecord grouped = Rec(L"prop_apple_C", 1, L"");
        grouped.bools = {{0, 1, 0}};
        grouped = largest(grouped);
        check(caw::RecordBytes(grouped) <= limit, "slack: the grouped test record is within the ask limit");
        grouped.bools[0].push_back(1);   // a record with a bools group gains one byte
        check(!caw::PackCommit(commitOf(grouped)).empty() &&
                  caw::ParseCommit(caw::PackCommit(commitOf(grouped)), back, &why) == caw::Parse::Ok,
              "slack: the largest record with a longer bools group packs into a commit and parses");
    }

    // ---- the tombstones and the slowed sender ----
    {
        cas::State s;
        check(!s.HasLapsed() && s.TombstoneCount() == 0, "lapse: a new table has none");
        cas::Hold h = MakeHold(cas::HoldKind::Reservation, 2, 7, 300, 1000);
        h.record.names = {{L"apple", L"tag"}};
        s.Lapse(h, 31000);
        check(s.HasLapsed() && s.TombstoneCount() == 1 && s.Slowed(2, 31000) && !s.Slowed(3, 31000),
              "lapse: a lapsed reservation leaves a tombstone and slows its own slot");
        check(!s.ConsumeTombstone(2, 8, 300) && !s.ConsumeTombstone(3, 7, 300) && !s.ConsumeTombstone(2, 7, 301) &&
                  s.TombstoneCount() == 1,
              "lapse: a tombstone is named by slot, id and eid together");
        const std::optional<cas::Tombstone> t = s.ConsumeTombstone(2, 7, 300);
        check(t && t->className == h.record.className && t->name == L"apple" && s.TombstoneCount() == 0,
              "lapse: a tombstone keeps the asked class and name and is consumed");
        check(!s.ConsumeTombstone(2, 7, 300), "lapse: a tombstone is consumed once");
        check(!s.Slowed(2, 31001), "lapse: consuming the tombstone ends the slowing");

        s.Lapse(h, 31000);
        check(s.Slowed(2, 60999) && !s.Slowed(2, 61000), "lapse: a slowing lasts 30 s");
        s.SweepLapsed(60999);
        check(s.TombstoneCount() == 1, "lapse: a tombstone 1 ms short of 30 s stays");
        s.SweepLapsed(61000);
        check(s.TombstoneCount() == 0 && !s.HasLapsed(), "lapse: a tombstone at 30 s is swept, and the slowing with it");

        for (uint16_t id = 1; id <= 5; ++id) {
            h.id = id;
            s.Lapse(h, 1000 + id);
        }
        check(s.TombstoneCount() == 4 && !s.ConsumeTombstone(2, 1, 300) && s.ConsumeTombstone(2, 2, 300).has_value(),
              "lapse: at most 4 per slot, the oldest of the slot evicted");
        cas::Hold other = MakeHold(cas::HoldKind::Reservation, 3, 9, 301, 0);
        s.Lapse(other, 2000);
        check(s.TombstoneCount() == 4 && s.Slowed(3, 2000), "lapse: another slot's tombstone is not evicted by the first's");
        s.DropLapsedOf(2);
        check(s.TombstoneCount() == 1 && !s.Slowed(2, 2000) && s.Slowed(3, 2000),
              "lapse: a departed slot's tombstones and slowing are dropped, the others' kept");
        s.DropLapsed();
        check(!s.HasLapsed() && !s.Slowed(3, 2000), "lapse: a disconnect drops all");

        cas::State held;
        held.Open(MakeHold(cas::HoldKind::Reservation, 1, 5, 100, 0));
        check(held.HoldOn(100) && !held.HoldOn(101), "state: HoldOn names a container any hold stands on");
    }

    if (pass == total) UE_LOGI("container_act_selftest: ALL PASS (%d checks)", total);
}

}  // namespace coop::props::container_act_host
