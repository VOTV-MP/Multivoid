// coop/props/container_act_selftest_put.cpp -- the put's share of the container act selftest: the replay guards of both
// put routes and where the container stored the record, the judge of a put and the order it reads in, a reservation's
// commit rule, the bounds on a done, the byte slack of the stored form, the held actor's hash, and the tombstones and
// the slowed sender. Pure: no engine call, no session. Called from RunSelftest (container_act_selftest.cpp).

#include "coop/items/save_record_wire.h"
#include "coop/props/container_act_host.h"
#include "coop/props/container_act_request.h"
#include "coop/props/container_act_selftest_internal.h"
#include "coop/props/container_act_state.h"
#include "coop/props/container_act_wire.h"
#include "coop/props/container_slice_wire.h"

#include <cstring>
#include <string>
#include <vector>

namespace coop::props::container_act_host::selftest {
namespace {

namespace H   = coop::props::container_act_host::internal;
namespace caw = coop::props::container_act_wire;
namespace cas = coop::props::container_act_state;
namespace cr  = coop::props::container_reach;
namespace SR  = ue_wrap::save_record;

// A put that passes every term: Ready() with the defaults of the put's own terms.
H::PutInputs PutInputsReady() {
    H::PutInputs p;
    p.base = Ready();
    return p;
}

}  // namespace

void RunPutSelftest(Checks& check) {
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
        check(H::DecidePut(p, &why) == H::Verdict::NoSpace && why == Tally::kCount, "put: the record cap is NoSpace");
        p = putReady();
        p.bytesOver = true;
        check(H::DecidePut(p) == H::Verdict::NoSpace, "put: the byte ceiling is NoSpace");
        p = putReady();
        p.volumeRead = false;
        check(H::DecidePut(p) == H::Verdict::Wait, "put: a volume that did not read waits");
        p = putReady();
        p.volumeKnown = false;
        p.countAtCap = true;
        check(H::DecidePut(p, &why) == H::Verdict::Refused && why == Tally::PutUnknownName,
              "put: an unknown name is refused before the record cap, counted put_unknown_name");
        p = putReady();
        p.volume = 60.f;
        check(H::DecidePut(p) == H::Verdict::Grant, "put: a volume that exactly fills the container fits");
        p.volume = 60.5f;
        check(H::DecidePut(p) == H::Verdict::NoSpace, "put: a volume past the container's room is NoSpace");
        p.infinite = true;
        const bool infiniteGrants = H::DecidePut(p) == H::Verdict::Grant;
        p.volumeKnown = false;
        check(infiniteGrants && H::DecidePut(p) == H::Verdict::Refused,
              "put: an infinite container ignores the volume but not an unknown name");
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
        check(s.Slowed(2, 31001), "lapse: consuming the tombstone does not end the slowing");

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

    // ---- the commit's re-check: sizes, names, a held actor's hash, the slowing of an abandoned reservation ----
    {
        namespace RQ = coop::props::container_act_request::internal;
        namespace csw = coop::props::container_slice_wire;

        SR::SaveRecord hand = Rec(L"prop_apple_C", 1, L"x");
        hand.names = {{L"apple", L"tag"}};
        SR::SaveRecord moved = hand;
        moved.xform = {0.1f, 0.2f, 0.3f, 0.9f, 50.f, 60.f, 70.f, 1.f, 1.f, 1.f};
        moved.floats[0][0] = 3.25f;
        check(RQ::PutHandHash(hand) == RQ::PutHandHash(moved),
              "hand-hash: a record that only moved and aged hashes the same");
        check(csw::RecordHash(hand) != csw::RecordHash(moved), "hand-hash: the plain record hash does see the move");
        SR::SaveRecord changed = hand;
        changed.strings[0][0] = L'y';
        check(RQ::PutHandHash(hand) != RQ::PutHandHash(changed), "hand-hash: any other change hashes differently");
        SR::SaveRecord noFloats = hand;
        noFloats.floats.clear();
        check(RQ::PutHandHash(noFloats) != 0, "hand-hash: a record with no floats hashes");

        const size_t asked = caw::RecordBytes(hand);
        SR::SaveRecord big = hand;
        check(H::DoneSizeOk(asked, hand), "done-size: the asked record itself is within the bound");
        big.strings = {{std::wstring(5, L'x')}};
        check(H::DoneSizeOk(asked, big), "done-size: a record a little larger than asked is within the slack");
        big.strings = {{std::wstring(caw::kPutStoredSlack, L'x')}};
        check(!H::DoneSizeOk(asked, big), "done-size: a record larger than asked by more than the slack is refused");

        SR::SaveRecord named = hand;
        named.names = {{std::wstring(caw::kMaxPutNameChars, L'a')}};
        check(H::PutNameOk(named), "name-bound: a name at the bound is accepted");
        named.names = {{std::wstring(caw::kMaxPutNameChars + 1, L'a')}};
        check(!H::PutNameOk(named), "name-bound: a name past the bound is refused");
        named.names.clear();
        check(!H::PutNameOk(named), "name-bound: a record with no name is refused");

        std::vector<SR::SaveRecord> three = {hand, moved, changed};
        check(csw::SliceBytes(three) == csw::Pack(7, 0, three).size() && csw::SliceBytes({}) == csw::Pack(7, 0, {}).size(),
              "slice-bytes: the size without packing is the packed size");

        H::PutInputs w;
        w = PutInputsReady();
        w.termsRead = false;
        check(std::strcmp(H::PutWaitCause(w), "the container's terms did not read") == 0, "wait-cause: terms");
        w = PutInputsReady();
        w.filterRead = false;
        check(std::strcmp(H::PutWaitCause(w), "the container's filter did not read") == 0, "wait-cause: filter");
        w = PutInputsReady();
        w.rowRead = false;
        check(std::strcmp(H::PutWaitCause(w), "the list_props row did not read") == 0, "wait-cause: row");
        w = PutInputsReady();
        w.volumeRead = false;
        check(std::strcmp(H::PutWaitCause(w), "the volume of the name did not read") == 0, "wait-cause: volume");
        w = PutInputsReady();
        w.base.holdsWait = true;
        check(std::strcmp(H::PutWaitCause(w), "a hold stands in its way") == 0, "wait-cause: a hold");
        check(std::strcmp(TallyName(Tally::PutDoneLost), "put_done_lost") == 0 &&
                  std::strcmp(TallyName(Tally::OwedLost), "owed_lost") == 0,
              "tally: the commit re-check's and the owed give-up's names");

        cas::State s;
        check(cas::kSlowAfterMs == 10000, "slow: a reservation is slowed after the client's own 10 s reply wait");
        s.Slow(4, 1000);
        check(s.Slowed(4, 1000) && s.Slowed(4, 30999) && !s.Slowed(4, 31000) && s.HasLapsed() &&
                  s.TombstoneCount() == 0,
              "slow: an abandoned reservation slows its slot for 30 s and leaves no tombstone");
        s.SweepLapsed(31000);
        check(!s.HasLapsed(), "slow: the mark is swept at its end");
        s.Slow(4, 0);
        s.Lapse(MakeHold(cas::HoldKind::Reservation, 4, 3, 50, 0), 100);
        s.ConsumeTombstone(4, 3, 50);
        check(s.Slowed(4, 101), "slow: the slot's own late commit does not end the slowing");
        s.DropLapsedOf(4);
        check(!s.Slowed(4, 101), "slow: the slot's departure ends the slowing");
    }
}

}  // namespace coop::props::container_act_host::selftest
