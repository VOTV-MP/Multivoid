// coop/props/prop_act_selftest.cpp -- the un-gated selftest of a client's take or use of a shared prop, run once per
// session start: the host's judge (every verdict, the order of its reads, each verb's fit term by term), the reply a
// verdict sends, the commit's rows and the client's reply decision, each from a stated input -- states no honest run
// lines up.

#include "coop/props/prop_act_host.h"
#include "coop/props/prop_act_request.h"
#include "coop/props/snack_act.h"

#include "ue_wrap/core/log.h"

namespace coop::prop_act_host {

namespace {
namespace H = coop::prop_act_host::internal;
namespace R = coop::prop_act_request::internal;
using coop::element::IntentOutcome;
using coop::net::PropActReplyPayload;

// Every read passes and the reach is Ok: the grant. A check breaks one field to reach one branch.
H::ActInputs Ready() {
    H::ActInputs in;
    in.worldReady = true;
    in.holdsReservation = false;
    in.live = true;
    in.familyFits = true;
    in.heldByOther = false;
    in.hasBody = true;
    in.reach = IntentOutcome::Ok;
    return in;
}

bool SameReply(const std::optional<PropActReplyPayload>& r, uint16_t id, uint8_t granted, uint8_t reason) {
    return r && r->id == id && r->granted == granted && r->reason == reason;
}

constexpr uint8_t kPocket = static_cast<uint8_t>(coop::net::PropActFamily::Pocket);
constexpr uint8_t kUse = static_cast<uint8_t>(coop::net::PropActFamily::Use);
constexpr uint8_t kHold = static_cast<uint8_t>(coop::net::PropActFamily::Hold);

// An act the game would perform: every read passed and every term of the three fits holds.
H::ActTraits Fitting() {
    H::ActTraits t{};
    t.read = true;
    t.simulating = true;
    t.canCollect = true;
    t.canHold = true;
    t.meshRoot = true;
    t.volumeFits = true;
    t.saveClass = true;
    t.equipRow = true;
    t.offered = true;
    return t;
}

// One term of a verb's fit, broken alone: what it says, the break, and the tally FirstRefusal names for it.
struct Term {
    const char* what;
    void (*breaks)(H::ActTraits&);
    Tally refusal;
};

const Term kPocketTerms[] = {
    {"pocket: an unread fact refuses", [](H::ActTraits& t) { t.read = false; }, Tally::FitUnread},
    {"pocket: a refused collect refuses", [](H::ActTraits& t) { t.collectRefused = true; }, Tally::FitCollectRefused},
    {"pocket: a child actor refuses", [](H::ActTraits& t) { t.child = true; }, Tally::FitChild},
    {"pocket: neither frozen, simulating nor driven refuses", [](H::ActTraits& t) { t.simulating = false; },
     Tally::FitPhysics},
    {"pocket: a prop the data row does not collect refuses", [](H::ActTraits& t) { t.canCollect = false; },
     Tally::FitCanCollect},
    {"pocket: a heavy prop refuses", [](H::ActTraits& t) { t.heavy = true; }, Tally::FitHeavy},
    {"pocket: a root that is not a static mesh refuses", [](H::ActTraits& t) { t.meshRoot = false; },
     Tally::FitMeshRoot},
    {"pocket: a volume past the bound refuses", [](H::ActTraits& t) { t.volumeFits = false; }, Tally::FitVolume},
    {"pocket: a class that saves nothing refuses", [](H::ActTraits& t) { t.saveClass = false; },
     Tally::FitSaveClass},
};

const Term kHoldTerms[] = {
    {"hold: an unread fact refuses", [](H::ActTraits& t) { t.read = false; }, Tally::FitUnread},
    {"hold: a refused pickup refuses", [](H::ActTraits& t) { t.pickupRefused = true; }, Tally::FitPickupRefused},
    {"hold: neither frozen, simulating nor driven refuses", [](H::ActTraits& t) { t.simulating = false; },
     Tally::FitPhysics},
    {"hold: a child actor that is not frozen refuses", [](H::ActTraits& t) { t.child = true; }, Tally::FitChild},
    {"hold: a heavy prop that is not frozen refuses", [](H::ActTraits& t) { t.heavy = true; }, Tally::FitHeavy},
    {"hold: a prop the data row does not hold refuses", [](H::ActTraits& t) { t.canHold = false; },
     Tally::FitCanHold},
    {"hold: a prop that ignores the save refuses", [](H::ActTraits& t) { t.ignoreSave = true; },
     Tally::FitIgnoreSave},
    {"hold: a class that saves nothing refuses", [](H::ActTraits& t) { t.saveClass = false; },
     Tally::FitSaveClass},
    {"hold: a name with no equip row refuses", [](H::ActTraits& t) { t.equipRow = false; }, Tally::FitEquipRow},
};

// The hold terms a frozen prop still meets: broken with `frozen` set, each must still refuse.
const Term kFrozenHoldTerms[] = {
    {"hold: a frozen prop with a refused pickup refuses", [](H::ActTraits& t) { t.pickupRefused = true; },
     Tally::FitPickupRefused},
    {"hold: a frozen prop that ignores the save refuses", [](H::ActTraits& t) { t.ignoreSave = true; },
     Tally::FitIgnoreSave},
    {"hold: a frozen prop of a class that saves nothing refuses", [](H::ActTraits& t) { t.saveClass = false; },
     Tally::FitSaveClass},
    {"hold: a frozen prop with no equip row refuses", [](H::ActTraits& t) { t.equipRow = false; },
     Tally::FitEquipRow},
};

const Term kUseTerms[] = {
    {"use: an unread fact refuses", [](H::ActTraits& t) { t.read = false; }, Tally::FitUnread},
    {"use: a class the game offers no eat on refuses", [](H::ActTraits& t) { t.offered = false; },
     Tally::FitNoUseRow},
    {"use: a food body that is not edible refuses",
     [](H::ActTraits& t) { t.foodBody = true; t.notEdible = true; }, Tally::FitNotEdible},
};
}  // namespace

void RunSelftest() {
    int pass = 0, total = 0;
    auto check = [&](bool ok, const char* what) {
        ++total;
        if (ok) { ++pass; return; }
        UE_LOGE("prop_act selftest FAIL: %s", what);
    };

    // Decide: the pass, then each read broken alone.
    check(H::Decide(Ready()) == H::ActVerdict::Grant, "decide: every read passing grants");
    {
        H::ActInputs in = Ready();
        in.worldReady = false;
        check(H::Decide(in) == H::ActVerdict::Wait, "decide: a world not ready waits");
        in.live = false;
        check(H::Decide(in) == H::ActVerdict::Wait, "decide: a not-ready slot with a dead prop waits, not gone");
    }
    {
        H::ActInputs in = Ready();
        in.holdsReservation = true;
        check(H::Decide(in) == H::ActVerdict::Wait, "decide: a slot holding a reservation waits");
        in.live = false;
        check(H::Decide(in) == H::ActVerdict::Wait, "decide: a held reservation with a dead prop waits, not gone");
    }
    {
        H::ActInputs in = Ready();
        in.hasBody = false;
        check(H::Decide(in) == H::ActVerdict::Wait, "decide: a slot with no body waits");
        in.live = false;
        check(H::Decide(in) == H::ActVerdict::Wait, "decide: no body with a dead prop waits (the body is read first)");
    }
    {
        H::ActInputs in = Ready();
        in.live = false;
        check(H::Decide(in) == H::ActVerdict::Gone, "decide: a body with a dead prop is gone");
        in.familyFits = false;
        in.heldByOther = true;
        in.reach = IntentOutcome::OutOfReach;
        check(H::Decide(in) == H::ActVerdict::Gone, "decide: a dead prop is gone before hold and reach");
    }
    {
        H::ActInputs in = Ready();
        in.familyFits = false;
        check(H::Decide(in) == H::ActVerdict::WrongFamily, "decide: a family that does not fit is refused");
        in.heldByOther = true;
        in.reach = IntentOutcome::OutOfReach;
        check(H::Decide(in) == H::ActVerdict::Held, "decide: the hold is read before the family");
    }
    {
        H::ActInputs in = Ready();
        in.heldByOther = true;
        check(H::Decide(in) == H::ActVerdict::Held, "decide: a prop held by another is refused");
        in.reach = IntentOutcome::OutOfReach;
        check(H::Decide(in) == H::ActVerdict::Held, "decide: the hold is read before the reach");
    }
    {
        H::ActInputs in = Ready();
        in.reach = IntentOutcome::OutOfReach;
        check(H::Decide(in) == H::ActVerdict::Reach, "decide: a target out of reach is refused");
        in.reach = IntentOutcome::NoTarget;
        check(H::Decide(in) == H::ActVerdict::Reach, "decide: a missing target is refused as reach");
        in.reach = IntentOutcome::NoBody;
        check(H::Decide(in) == H::ActVerdict::Wait, "decide: a reach that finds no body, with a body read, waits");
    }

    // The act is read last: a hold or a reach that refuses is answered before the family.
    {
        H::ActInputs in = Ready();
        in.familyFits = false;
        in.reach = IntentOutcome::OutOfReach;
        check(H::Decide(in) == H::ActVerdict::Reach, "decide: the reach is read before the family");
        in.reach = IntentOutcome::NoBody;
        check(H::Decide(in) == H::ActVerdict::Wait, "decide: a reach that finds no body waits before the family");
        in.reach = IntentOutcome::Ok;
        in.heldByOther = true;
        check(H::Decide(in) == H::ActVerdict::Held, "decide: a prop held by another is held before the family");
    }

    // The fits: the baseline, each term broken alone, and the cases that fit by another term.
    check(H::PocketFits(Fitting()), "fits: the baseline pocket fits");
    check(H::HoldFits(Fitting()), "fits: the baseline hold fits");
    check(H::UseFits(Fitting()), "fits: the baseline use fits");
    check(!H::FirstRefusal(kPocket, Fitting()), "refusal: a pocket that fits names no term");
    check(!H::FirstRefusal(kHold, Fitting()), "refusal: a hold that fits names no term");
    check(!H::FirstRefusal(kUse, Fitting()), "refusal: a use that fits names no term");
    for (const Term& term : kPocketTerms) {
        H::ActTraits t = Fitting();
        term.breaks(t);
        check(!H::PocketFits(t), term.what);
        check(H::FirstRefusal(kPocket, t) == term.refusal, term.what);
    }
    for (const Term& term : kHoldTerms) {
        H::ActTraits t = Fitting();
        term.breaks(t);
        check(!H::HoldFits(t), term.what);
        check(H::FirstRefusal(kHold, t) == term.refusal, term.what);
    }
    for (const Term& term : kUseTerms) {
        H::ActTraits t = Fitting();
        term.breaks(t);
        check(!H::UseFits(t), term.what);
        check(H::FirstRefusal(kUse, t) == term.refusal, term.what);
    }
    {
        // A frozen prop is held whatever it weighs and whatever its row says, as the game's Hold Object does.
        H::ActTraits t = Fitting();
        t.frozen = true;
        t.heavy = true;
        t.canHold = false;
        t.child = true;
        t.simulating = false;
        check(H::HoldFits(t), "fits: a frozen heavy hold that the row does not hold still fits");
        check(!H::FirstRefusal(kHold, t), "refusal: a frozen hold names no term");
        // A frozen prop is pocketed, but never a heavy one.
        H::ActTraits p = Fitting();
        p.frozen = true;
        p.simulating = false;
        check(H::PocketFits(p), "fits: a frozen pocket fits");
        p.heavy = true;
        check(!H::PocketFits(p), "fits: a frozen heavy pocket does not fit");
    }
    {
        // The frozen skip leaves the other terms in force: the game's frozen branch jumps to the ignoreSave test.
        for (const Term& term : kFrozenHoldTerms) {
            H::ActTraits t = Fitting();
            t.frozen = true;
            term.breaks(t);
            check(!H::HoldFits(t), term.what);
            check(H::FirstRefusal(kHold, t) == term.refusal, term.what);
        }
        // The pocket has no frozen arm for a child actor.
        H::ActTraits p = Fitting();
        p.frozen = true;
        p.child = true;
        check(!H::PocketFits(p), "fits: a frozen child actor does not pocket");
        check(H::FirstRefusal(kPocket, p) == Tally::FitChild, "refusal: a frozen child pocket is named child");
    }
    {
        // A prop a peer drags has its host copy parked with physics off: it fits by the drive.
        H::ActTraits p = Fitting();
        p.simulating = false;
        p.driven = true;
        check(H::PocketFits(p), "fits: a pocket fits only by a peer's drive");
        check(H::HoldFits(p), "fits: a hold fits only by a peer's drive");
    }
    {
        H::ActTraits t = Fitting();
        t.foodBody = true;
        check(H::UseFits(t), "fits: an edible food body fits");
        t.foodBody = false;
        t.notEdible = true;
        check(H::UseFits(t), "fits: notEdible is read for a food body alone");
    }
    {
        H::ActTraits t = Fitting();
        t.heavy = true;
        check(H::FirstRefusal(kPocket, t) == Tally::FitHeavy, "refusal: a heavy pocket is named heavy");
        t = Fitting();
        t.simulating = false;
        check(H::FirstRefusal(kPocket, t) == Tally::FitPhysics, "refusal: a pocket with no physics is named so");
        t = Fitting();
        t.read = false;
        t.heavy = true;
        check(H::FirstRefusal(kPocket, t) == Tally::FitUnread, "refusal: an unread fact is named before any term");
        t = Fitting();
        t.collectRefused = true;
        t.heavy = true;
        check(H::FirstRefusal(kPocket, t) == Tally::FitCollectRefused, "refusal: the first term in order is named");
    }
    check(!H::KnownFamily(0), "family: 0 is not a verb");
    check(H::KnownFamily(1) && H::KnownFamily(2) && H::KnownFamily(3), "family: 1..3 are verbs");
    check(!H::KnownFamily(4), "family: 4 is not a verb");
    check(H::FirstRefusal(0, Fitting()) == Tally::FitBadFamily, "refusal: a byte that is no verb is named");
    // A bad verb byte that reached the fit would be refused whatever the traits say (the judge's second defence).
    constexpr uint8_t kBadVerbs[] = {0, 4, 255};
    for (const uint8_t bad : kBadVerbs) {
        check(!H::KnownFamily(bad), "family: a byte outside 1..3 is no verb");
        check(H::FirstRefusal(bad, Fitting()) == Tally::FitBadFamily,
              "refusal: a fitting trait set does not rescue a bad verb");
    }

    // ReplyFor: Wait sends nothing; every other verdict sends one reply with its reason.
    check(!H::ReplyFor(H::ActVerdict::Wait, 7).has_value(), "reply: a wait sends nothing");
    check(SameReply(H::ReplyFor(H::ActVerdict::Grant, 7), 7, 1, 0), "reply: a grant is granted with reason 0");
    check(SameReply(H::ReplyFor(H::ActVerdict::Gone, 8), 8, 0, 1), "reply: gone is reason 1");
    check(SameReply(H::ReplyFor(H::ActVerdict::WrongFamily, 9), 9, 0, 2), "reply: wrong family is reason 2");
    check(SameReply(H::ReplyFor(H::ActVerdict::Held, 10), 10, 0, 3), "reply: held is reason 3");
    check(SameReply(H::ReplyFor(H::ActVerdict::Reach, 11), 11, 0, 4), "reply: reach is reason 4");

    // DecideCommit: (matched, committed byte, liveAndSame, a snack, flags).
    check(H::DecideCommit(false, 1, true, false, 0) == H::CommitStep::BadCommit, "commit: no matching grant is a bad commit");
    check(H::DecideCommit(false, 0, true, false, 0) == H::CommitStep::BadCommit, "commit: no matching grant beats an abort");
    check(H::DecideCommit(true, 0, true, false, 0) == H::CommitStep::Abort, "commit: a matched 0 aborts");
    check(H::DecideCommit(true, 0, false, false, 0) == H::CommitStep::Abort, "commit: an abort does not read the prop");
    check(H::DecideCommit(true, 3, true, true, 0) == H::CommitStep::BadCommit, "commit: a byte past 2 is a bad commit");
    check(H::DecideCommit(true, 255, false, false, 0) == H::CommitStep::BadCommit, "commit: a bad byte beats a dead prop");
    check(H::DecideCommit(true, 1, false, false, 0) == H::CommitStep::TargetGone, "commit: a dead or replaced prop is gone");
    check(H::DecideCommit(true, 1, true, false, 0) == H::CommitStep::Destroy, "commit: a matched live prop is destroyed");
    check(H::DecideCommit(true, 2, true, false, 0) == H::CommitStep::BadCommit, "commit: a 2 on a prop that is no snack is bad");
    check(H::DecideCommit(true, 1, true, false, 1) == H::CommitStep::BadCommit, "commit: a flag on a prop that is no snack is bad");
    check(H::DecideCommit(true, 1, true, true, 1) == H::CommitStep::SnackEnded, "commit: a snack's end carries the spent bit");
    check(H::DecideCommit(true, 1, true, true, 0) == H::CommitStep::BadCommit, "commit: a snack's end with no bit is bad");
    check(H::DecideCommit(true, 1, true, true, 3) == H::CommitStep::BadCommit, "commit: a snack's end carries the spent bit alone");
    check(H::DecideCommit(true, 2, true, true, 0) == H::CommitStep::SnackApplied, "commit: a full eater's 2 carries no bit");
    check(H::DecideCommit(true, 2, true, true, 3) == H::CommitStep::SnackApplied, "commit: a 2 may carry both bits");
    check(H::DecideCommit(true, 2, true, true, 4) == H::CommitStep::BadCommit, "commit: a bit past the two is bad");
    check(H::DecideCommit(true, 2, false, true, 0) == H::CommitStep::TargetGone, "commit: a gone snack is gone");

    // The snack's bounded apply: (host uses, host open, beer, flags) -> what the host's copy holds.
    namespace S = coop::snack_act;
    const auto bound = [](int32_t uses, bool open, bool beer, uint8_t flags) {
        return S::internal::Bound(uses, open, beer, flags);
    };
    check(bound(4, false, false, S::kFlagSpent).uses == 3, "snack: a spent use takes one from a snack");
    check(bound(4, false, false, 0).uses == 4, "snack: no bit changes nothing");
    check(bound(0, false, false, S::kFlagSpent).uses == 0, "snack: a copy with no use cannot spend one");
    check(bound(8, false, true, S::kFlagSpent).uses == 8, "snack: a closed beer cannot spend a use");
    check(!bound(8, false, true, S::kFlagSpent).open, "snack: a spent report does not open a bottle");
    check(bound(8, false, true, S::kFlagOpened).open && bound(8, false, true, S::kFlagOpened).uses == 8,
          "snack: an opened report opens a closed beer and spends nothing");
    check(bound(8, false, true, S::kFlagOpened | S::kFlagSpent).uses == 8 &&
              bound(8, false, true, S::kFlagOpened | S::kFlagSpent).open,
          "snack: an open and a spend reported together from a closed beer open it and spend nothing");
    check(bound(8, true, true, S::kFlagSpent).uses == 7, "snack: an open beer spends a use");
    check(bound(8, true, true, S::kFlagOpened).open, "snack: an open beer stays open");
    check(!bound(4, false, false, S::kFlagOpened).open, "snack: only a beer is opened");
    check(bound(1, true, true, S::kFlagSpent).uses == 0, "snack: the last use reaches 0");
    check(S::internal::WellFormedSnackCommit(1, S::kFlagSpent) && !S::internal::WellFormedSnackCommit(1, 0) &&
              S::internal::WellFormedSnackCommit(2, 0) && !S::internal::WellFormedSnackCommit(0, 0),
          "snack: the commit words are well formed only as the wire states them");
    const S::CopyState three{true, 3, false}, two{true, 2, false}, openBeer{true, 3, true};
    check(S::CommitFor(true, true, three, S::CopyState{}).committed == 1 &&
              S::CommitFor(true, true, three, S::CopyState{}).flags == S::kFlagSpent,
          "snack: an ended copy commits 1 with the spent bit");
    check(S::CommitFor(true, false, three, two).committed == S::kCommitApplied &&
              S::CommitFor(true, false, three, two).flags == S::kFlagSpent,
          "snack: a spent use commits 2 with the spent bit");
    check(S::CommitFor(true, false, three, three).committed == S::kCommitApplied &&
              S::CommitFor(true, false, three, three).flags == 0,
          "snack: a full eater commits 2 with no bit");
    check(S::CommitFor(true, false, three, openBeer).flags == S::kFlagOpened, "snack: an opened bottle commits the opened bit");
    check(S::CommitFor(false, false, three, three).committed == 0 && S::CommitFor(true, false, S::CopyState{}, two).committed == 0,
          "snack: a replay that did not run, or whose copy is unread, aborts");

    // AfterDestroy.
    check(H::AfterDestroy(true) == H::AfterStep::FanOut, "after: a destroyed prop fans out");
    check(H::AfterDestroy(false) == H::AfterStep::DestroyFailed, "after: a prop still standing is a failure");

    // DecideReply: (a record is open, its id, the reply).
    check(R::DecideReply(false, 5, PropActReplyPayload{5, 1, 0}) == R::ReplyAction::Stale,
          "decide-reply: no open record is stale");
    check(R::DecideReply(true, 5, PropActReplyPayload{6, 1, 0}) == R::ReplyAction::Stale,
          "decide-reply: a foreign id is stale");
    check(R::DecideReply(true, 5, PropActReplyPayload{5, 1, 0}) == R::ReplyAction::Granted,
          "decide-reply: a matching grant is granted");
    for (uint8_t reason = 1; reason <= 5; ++reason) {
        check(R::DecideReply(true, 5, PropActReplyPayload{5, 0, reason}) == R::ReplyAction::Refused,
              "decide-reply: a refusal with reason 1..5 is refused");
    }
    check(R::DecideReply(true, 5, PropActReplyPayload{5, 0, 0}) == R::ReplyAction::BadReason,
          "decide-reply: not granted with reason 0 is a bad reason");
    check(R::DecideReply(true, 5, PropActReplyPayload{5, 0, 6}) == R::ReplyAction::BadReason,
          "decide-reply: reason 6 is a bad reason");
    check(R::DecideReply(true, 5, PropActReplyPayload{5, 1, 3}) == R::ReplyAction::BadReason,
          "decide-reply: granted with a reason is a bad reason");

    if (pass == total) UE_LOGI("prop_act selftest: ALL PASS (%d checks)", total);
}

}  // namespace coop::prop_act_host
