// coop/props/prop_act_bucket_dip.cpp -- the dip family (8) of a water bucket; see coop/props/prop_act_bucket.h.
//
// A held sponge's left click traces to a bucket and calls its dipped(sponge, isDipped&) (prop_sponge.cpp:371-415). Above 10 cm of
// water: with soap in the bucket the soap drops by one (the sponge takes it), else the water drops by 2 cm and the bucket splashes
// (prop_bucket.cpp:899-940). The client's PRE on dipped cancels the call, sets its out-param false so the sponge's tail does not run,
// and asks the host with the branch the client's copy took. A grant replays the sponge's own left click under the latch: its trace,
// its nested dipped (let through, and noted as the replay's) and its tail (the power, the sound, the montage). The commit is 1 when
// that nested dipped ran. The host's judge reads its own bucket for the branch and the water, records the branch, and the commit
// applies the BP's target lines.

#include "coop/props/prop_act_bucket.h"

#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/facts/function_row.h"

namespace coop::prop_act_bucket {
namespace {

UE_FACT_FN(kRowDipped, "prop_act_bucket", L"prop_bucket_C", L"dipped", Declared);
constexpr int kTagDipped = 0x50424430;   // "PBD0"
constexpr const char* kDead = "a client's sponge dipped in a water bucket runs on its own copy only, unasked, and the copies diverge";

constexpr float kDipMinHeight = 10.f;    // the BP's dipped does nothing at or below this
constexpr float kDipWater = 2.f;         // the water a dip takes

// Set by the PRE when the nested dipped of the replay runs; the commit word.
bool g_dipRan = false;

R::ParamOffset g_outOff{L"isDipped"};

// The call's out-param: the sponge's tail runs on true, so a cancelled dip sets it false.
void SetDipped(const sg::Call& c, bool v) {
    const int32_t off = g_outOff.Of(c.function);
    uint8_t* const out = off >= 0 ? sg::OutParamPtr(c, off) : nullptr;
    if (out) *out = v ? 1 : 0;
}

// ---- the client's half ----

bool RunDip(void*, void* local, const PF::Verb& verb) {
    uint8_t branch = 0;
    std::wstring name;
    void* sponge = nullptr;
    // The hold may have changed in the round trip: the replay is the stroke of the sponge the player holds now.
    if (!VerbByte(verb, branch) || branch > 1 || !ue_wrap::miner::ReadHold(local, name, sponge) || !sponge ||
        !IsClass(sponge, L"prop_sponge_C"))
        return false;
    g_dipRan = false;
    return B::CallHandUseLmb(sponge, local);
}

uint8_t DipCommit(void*, const PF::Verb&) { return g_dipRan ? 1 : 0; }

// ---- the host's half ----

std::optional<Tally> Judge(N::Session& session, uint8_t slot, const N::PropActRequestPayload& req, void* bucket) {
    if (!InRoster(slot)) return Tally::FitFamilyArg;
    g_judged[slot] = Judged{};
    if (JudgeSkippedByRed(slot, "dip")) return std::nullopt;
    uint8_t branch = 0;
    B::Plain p;
    if (B::KindOf(bucket) != B::Kind::Plain || !VerbByte(req.verb, branch) || branch > 1) return Tally::FitFamilyArg;
    if (!B::ReadPlain(bucket, p)) return Tally::FitFamilyUnread;
    std::wstring cls;
    if (!HandHolds(session, slot, L"prop_sponge_C", cls)) return Tally::FitFamilyHand;
    if (!(p.height > kDipMinHeight) || (p.soap > 0) != (branch == 1)) return Tally::FitFamilyRule;
    if (!BudgetOf(N::PropActFamily::BucketDip, slot).Take(kDipBudget, N::NowMs())) return Tally::FitFamilyBudget;
    g_judged[slot] = Judged{true, req.family, branch, cls};
    return std::nullopt;
}

// A dip's commit of 1: the judge's condition read again on the host's copy, then the BP's target lines of the recorded branch.
std::optional<Tally> ApplyDip(uint8_t slot, void* bucket) {
    if (!InRoster(slot) || !g_judged[slot].valid || g_judged[slot].family != static_cast<uint8_t>(N::PropActFamily::BucketDip))
        return Tally::FitFamilyArg;
    const bool soapBranch = g_judged[slot].value == 1;
    if (ApplySkippedByRed(slot, "dip")) return std::nullopt;
    B::Plain p;
    if (!B::ReadPlain(bucket, p)) return FailedApply(slot, bucket, "dip", Tally::FitFamilyUnread);
    if (!(p.height > kDipMinHeight) || (p.soap > 0) != soapBranch) return FailedApply(slot, bucket, "dip", Tally::FitFamilyRule);
    if (soapBranch) {
        --p.soap;
        B::WritePlain(bucket, p.height, p.soap, p.alcoholP, p.alcohol, p.alcReady);
        B::CallUpd(bucket);
    } else {
        p.height -= kDipWater;
        B::WritePlain(bucket, p.height, p.soap, p.alcoholP, p.alcohol, p.alcReady);
        B::CallDipEffect(bucket);
    }
    coop::bucket_sync::Note(coop::bucket_sync::Count::FamilyApplies);
    coop::bucket_sync::MarkDirty(bucket);
    UE_LOGI("prop_act: slot %u's dip took %s of a water bucket (water %.1f, soap %d)", static_cast<unsigned>(slot),
            soapBranch ? "a soap" : "2 cm", p.height, static_cast<int>(p.soap));
    return std::nullopt;
}

// ---- the registry's entry ----

const PF::WatchDef kDipWatch[] = {{kRowDipped, kTagDipped, &PreDipped, "dip", kDead}};
const PF::Family kDip = {
    static_cast<uint8_t>(N::PropActFamily::BucketDip), "bucket dip", EL::ElementType::Prop, PF::Apply::AtCommit, kDipWatch, 1,
    &RunDip, &DipCommit, &Judge, nullptr, &ApplyDip, &NoteAborted, nullptr, nullptr, nullptr};
const PF::Registrar kRegisterDip(kDip);

}  // namespace

sg::Verdict PreDipped(const sg::Call& c) {
    static R::ParamOffset sSponge{L"sponge"};
    N::Session* const s = RQ::internal::LinkedSession();
    if (!s) return sg::Verdict::Run;
    void* const bucket = c.object;
    if (s->role() == N::Role::Host) return RQ::internal::HostPre(bucket);
    if (RQ::internal::ReplayingTarget(bucket)) {   // the replay's own sponge stroke reached its bucket
        g_dipRan = true;
        return sg::Verdict::Run;
    }
    if (RQ::internal::ReplayInProgress()) {   // a replay never acts unasked on a second target: the sponge's tail does not run
        SetDipped(c, false);
        return sg::Verdict::Cancel;
    }
    EL::ElementId eid = EL::kInvalidId;
    if (B::KindOf(bucket) != B::Kind::Plain || !PF::SharedTarget(bucket, EL::ElementType::Prop, eid))
        return sg::Verdict::Run;
    B::Plain p;
    if (!B::ReadPlain(bucket, p)) {
        SetDipped(c, false);
        return CancelUnread("a bucket copy's members cannot be read");
    }
    if (!(p.height > kDipMinHeight)) return sg::Verdict::Run;   // the game's own refusal: nothing to dip into
    void* local = nullptr;
    void* held = nullptr;
    std::wstring name;
    if (const char* why = ReadClientHold(local, name, held)) {
        SetDipped(c, false);
        return CancelUnread(why);
    }
    if (held != ParamObject(c, sSponge)) return sg::Verdict::Run;
    PF::Verb v;
    PutVerbByte(v, p.soap > 0 ? 1 : 0);
    SetDipped(c, false);
    return AskFamily(*s, local, held, bucket, eid, N::PropActFamily::BucketDip, &v);
}

}  // namespace coop::prop_act_bucket
