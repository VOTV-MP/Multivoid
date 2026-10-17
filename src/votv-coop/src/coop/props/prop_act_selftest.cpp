// coop/props/prop_act_selftest.cpp -- the un-gated selftest of a client's take or use of a shared prop, run once per
// session start: the host's judge (every verdict, the order of its reads), the reply a verdict sends, the commit's
// rows and the client's reply decision, each from a stated input -- states no honest run lines up.

#include "coop/props/prop_act_host.h"
#include "coop/props/prop_act_request.h"

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
        check(H::Decide(in) == H::ActVerdict::Gone, "decide: a dead prop is gone before family, hold and reach");
    }
    {
        H::ActInputs in = Ready();
        in.familyFits = false;
        check(H::Decide(in) == H::ActVerdict::WrongFamily, "decide: a family that does not fit is refused");
        in.heldByOther = true;
        in.reach = IntentOutcome::OutOfReach;
        check(H::Decide(in) == H::ActVerdict::WrongFamily, "decide: the family is read before hold and reach");
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

    // ReplyFor: Wait sends nothing; every other verdict sends one reply with its reason.
    check(!H::ReplyFor(H::ActVerdict::Wait, 7).has_value(), "reply: a wait sends nothing");
    check(SameReply(H::ReplyFor(H::ActVerdict::Grant, 7), 7, 1, 0), "reply: a grant is granted with reason 0");
    check(SameReply(H::ReplyFor(H::ActVerdict::Gone, 8), 8, 0, 1), "reply: gone is reason 1");
    check(SameReply(H::ReplyFor(H::ActVerdict::WrongFamily, 9), 9, 0, 2), "reply: wrong family is reason 2");
    check(SameReply(H::ReplyFor(H::ActVerdict::Held, 10), 10, 0, 3), "reply: held is reason 3");
    check(SameReply(H::ReplyFor(H::ActVerdict::Reach, 11), 11, 0, 4), "reply: reach is reason 4");

    // DecideCommit: (matched, committed byte, liveAndSame).
    check(H::DecideCommit(false, 1, true) == H::CommitStep::BadCommit, "commit: no matching grant is a bad commit");
    check(H::DecideCommit(false, 0, true) == H::CommitStep::BadCommit, "commit: no matching grant beats an abort");
    check(H::DecideCommit(true, 0, true) == H::CommitStep::Abort, "commit: a matched 0 aborts");
    check(H::DecideCommit(true, 0, false) == H::CommitStep::Abort, "commit: an abort does not read the prop");
    check(H::DecideCommit(true, 2, true) == H::CommitStep::BadCommit, "commit: a byte past 1 is a bad commit");
    check(H::DecideCommit(true, 255, false) == H::CommitStep::BadCommit, "commit: a bad byte beats a dead prop");
    check(H::DecideCommit(true, 1, false) == H::CommitStep::TargetGone, "commit: a dead or replaced prop is gone");
    check(H::DecideCommit(true, 1, true) == H::CommitStep::Destroy, "commit: a matched live prop is destroyed");

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
