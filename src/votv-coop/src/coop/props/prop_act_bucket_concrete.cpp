// coop/props/prop_act_bucket_concrete.cpp -- the mix (9) and trowel (10) families; see coop/props/prop_act_bucket.h.
//
// A held cement bag's left click on a water bucket (prop_cementBag.cpp:20-122) takes need = round(height / maxHeight * 20) units
// from the bag, replaces the bucket with a concrete bucket of `need` units (lib.replaceProp makes it; the game's verb run on a client
// would make a client-local one), and destroys the old bucket and, when empty, the bag. The client's replay is the held bag's side
// alone, written through ue_wrap::bucket: its units, the game's hint, its destroy; the bucket's side is the host's apply. A held
// trowel's left click on a concrete bucket (prop_wallfixer.cpp:215-237) takes one unit (takeConcrete, whose last unit turns the
// bucket back into a water bucket) and loads the trowel with 5; the replay loads the held trowel, the apply takes the unit.
// The bag's left click also reaches a world bag: that is the transfer family's (coop/props/prop_act_bucket_bag.cpp).

#include "coop/props/prop_act_bucket.h"

#include "coop/player/hand_spend.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/engine/engine_pawn.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/votv_lib.h"

#include <cmath>
#include <string>

namespace coop::prop_act_bucket {
namespace {

namespace E = ue_wrap::engine;

UE_FACT_FN(kRowBagLmb,   "prop_act_bucket", L"prop_cementBag_C",  L"playerHandUse_LMB", Declared);
UE_FACT_FN(kRowFixerLmb, "prop_act_bucket", L"prop_wallfixer_C",  L"playerHandUse_LMB", Declared);
constexpr int kTagBagLmb = 0x50424C30, kTagFixerLmb = 0x50424C31;   // "PBL0", "PBL1"
constexpr const char* kDeadMix =
    "a client's cement bag on a water bucket makes a concrete bucket on its own copy only, unasked, and the copies diverge";
constexpr const char* kDeadTrowel =
    "a client's trowel in a concrete bucket takes concrete on its own copy only, unasked, and the copies diverge";
constexpr int32_t kTrowelLoad = 5;   // the concrete a trowel carries after it takes a unit

// The units of concrete a bucket's water makes, as the BP rounds them (UKismetMathLibrary::Round).
int32_t MixNeed(const B::Plain& p) {
    return p.maxHeight > 0.f ? static_cast<int32_t>(std::floor(p.height / p.maxHeight * 20.f + 0.5f)) : 0;
}

// ---- the client's half ----

// The held bag's side of the mix, as the BP's lines do: its units, the hint, the bag gone at the last.
bool RunMix(void*, void* local, const PF::Verb& verb) {
    int32_t need = 0;
    std::wstring name;
    void* held = nullptr;
    if (!VerbInt(verb, need) || need < 1 || need > kMixNeedMax || !ue_wrap::miner::ReadHold(local, name, held) || !held ||
        B::KindOf(held) != B::Kind::Bag)
        return false;
    int32_t units = 0, maxUnits = 0;
    if (!B::ReadBag(held, units, maxUnits) || units - need < 0) return false;
    units -= need;
    B::WriteBagUnits(held, units);
    ue_wrap::votv_lib::AddHint(std::to_wstring(units) + L" units left", 0, local);
    if (units <= 0) E::DestroyActor(held);
    return true;
}

uint8_t SpentCommit(void*, const PF::Verb&) { return 1; }

// The held trowel's load.
bool RunTrowel(void*, void* local, const PF::Verb& verb) {
    std::wstring name;
    void* held = nullptr;
    if (!VerbZeroFrom(verb, 0) || !ue_wrap::miner::ReadHold(local, name, held) || !held || B::KindOf(held) != B::Kind::Trowel)
        return false;
    return B::WriteTrowel(held, kTrowelLoad) && B::CallUpdConcrete(held);
}

// ---- the host's half ----

std::optional<Tally> JudgeMix(N::Session& session, uint8_t slot, const N::PropActRequestPayload& req, void* bucket) {
    if (!InRoster(slot)) return Tally::FitFamilyArg;
    g_judged[slot] = Judged{};
    if (JudgeSkippedByRed(slot, "mix")) return std::nullopt;
    int32_t need = 0;
    B::Plain p;
    if (B::KindOf(bucket) != B::Kind::Plain || !VerbInt(req.verb, need) || need < 1 || need > kMixNeedMax) return Tally::FitFamilyArg;
    if (!B::ReadPlain(bucket, p)) return Tally::FitFamilyUnread;
    std::wstring cls;
    if (!HandHolds(session, slot, L"prop_cementBag_C", cls)) return Tally::FitFamilyHand;
    if (!p.canConcrete || MixNeed(p) != need) return Tally::FitFamilyRule;
    if (coop::bucket_hold::BagUnits(slot) < need) return Tally::FitFamilyHand;   // the held bag cannot pay it
    if (!BudgetOf(N::PropActFamily::BucketMix, slot).Take(kMixBudget, N::NowMs())) return Tally::FitFamilyBudget;
    g_judged[slot] = Judged{true, req.family, need, cls};
    return std::nullopt;
}

// A mix's commit of 1: the water read again on the host's copy, the held bag's ledger spent (the client's replay spent the bag), then
// a concrete bucket of the recorded units in place of the bucket.
std::optional<Tally> ApplyMix(uint8_t slot, void* bucket) {
    if (!InRoster(slot) || !g_judged[slot].valid || g_judged[slot].family != static_cast<uint8_t>(N::PropActFamily::BucketMix))
        return Tally::FitFamilyArg;
    const int32_t need = g_judged[slot].value;
    if (ApplySkippedByRed(slot, "mix")) return std::nullopt;
    B::Plain p;
    if (!B::ReadPlain(bucket, p)) return FailedApply(slot, bucket, "mix", Tally::FitFamilyUnread);
    const int32_t units = coop::bucket_hold::BagUnits(slot);
    if (units < need) return FailedApply(slot, bucket, "mix", Tally::FitFamilyHand);
    coop::bucket_hold::SetBagUnits(slot, units - need);
    if (!p.canConcrete || MixNeed(p) != need) return FailedApply(slot, bucket, "mix", Tally::FitFamilyRule);
    void* const made = B::ReplaceProp(bucket, L"concreteBucket", bucket);
    if (!made || B::KindOf(made) != B::Kind::Concrete) return FailedApply(slot, bucket, "mix", Tally::FitFamilyUnread);
    B::WriteConcrete(made, need, 0.f, false);
    B::CallUpdStage(made);
    E::DestroyActor(bucket);
    coop::bucket_sync::Note(coop::bucket_sync::Count::FamilyApplies);
    coop::bucket_sync::MarkDirty(made);   // 0 while the new bucket has no element id: its own tick tells it
    UE_LOGI("prop_act: slot %u mixed %d units of cement into a water bucket -- a concrete bucket stands in its place",
            static_cast<unsigned>(slot), static_cast<int>(need));
    return std::nullopt;
}

std::optional<Tally> JudgeTrowel(N::Session& session, uint8_t slot, const N::PropActRequestPayload& req, void* bucket) {
    if (!InRoster(slot)) return Tally::FitFamilyArg;
    g_judged[slot] = Judged{};
    if (JudgeSkippedByRed(slot, "trowel")) return std::nullopt;
    B::Concrete cb;
    if (B::KindOf(bucket) != B::Kind::Concrete || !VerbZeroFrom(req.verb, 0)) return Tally::FitFamilyArg;
    if (!B::ReadConcrete(bucket, cb)) return Tally::FitFamilyUnread;
    std::wstring cls;
    if (!HandHolds(session, slot, L"prop_wallfixer_C", cls)) return Tally::FitFamilyHand;
    if (cb.dry) return Tally::FitFamilyRule;
    if (!BudgetOf(N::PropActFamily::BucketTrowel, slot).Take(kTrowelBudget, N::NowMs())) return Tally::FitFamilyBudget;
    g_judged[slot] = Judged{true, req.family, 0, cls};
    return std::nullopt;
}

// A trowel's commit of 1 (the client's replay loaded its own tool): takeConcrete on the host's copy
// (its last unit turns the bucket into a water bucket). A dry bucket takes nothing: the out-param says so, and that is a failed apply.
std::optional<Tally> ApplyTrowel(uint8_t slot, void* bucket) {
    if (!InRoster(slot) || !g_judged[slot].valid || g_judged[slot].family != static_cast<uint8_t>(N::PropActFamily::BucketTrowel))
        return Tally::FitFamilyArg;
    if (ApplySkippedByRed(slot, "trowel")) return std::nullopt;
    B::Concrete cb;
    if (!B::ReadConcrete(bucket, cb)) return FailedApply(slot, bucket, "trowel", Tally::FitFamilyUnread);
    if (cb.dry) return FailedApply(slot, bucket, "trowel", Tally::FitFamilyRule);
    ue_wrap::CachedObjRef ref;
    ref.Set(bucket);
    bool dry = false;
    if (!B::CallTakeConcrete(bucket, dry) || dry) return FailedApply(slot, bucket, "trowel", Tally::FitFamilyRule);
    coop::bucket_sync::Note(coop::bucket_sync::Count::FamilyApplies);
    if (ref.Alive()) coop::bucket_sync::MarkDirty(bucket);   // the last unit replaced the bucket: the water bucket's own tick tells it
    UE_LOGI("prop_act: slot %u's trowel took a unit out of a concrete bucket (%d left)", static_cast<unsigned>(slot),
            static_cast<int>(cb.units) - 1);
    return std::nullopt;
}

// ---- the registry's entries ----

const PF::WatchDef kMixWatch[] = {{kRowBagLmb, kTagBagLmb, &PreBagLmb, "mix or transfer", kDeadMix}};
const PF::WatchDef kTrowelWatch[] = {{kRowFixerLmb, kTagFixerLmb, &PreFixerLmb, "trowel", kDeadTrowel}};
const PF::Family kMix = {
    static_cast<uint8_t>(N::PropActFamily::BucketMix), "bucket mix", EL::ElementType::Prop, PF::Apply::AtCommit, kMixWatch, 1,
    &RunMix, &SpentCommit, &JudgeMix, nullptr, &ApplyMix, &NoteAborted, nullptr, nullptr, nullptr};
const PF::Family kTrowel = {
    static_cast<uint8_t>(N::PropActFamily::BucketTrowel), "bucket trowel", EL::ElementType::Prop, PF::Apply::AtCommit, kTrowelWatch, 1,
    &RunTrowel, &SpentCommit, &JudgeTrowel, nullptr, &ApplyTrowel, &NoteAborted, nullptr, nullptr, nullptr};
const PF::Registrar kRegisterMix(kMix);
const PF::Registrar kRegisterTrowel(kTrowel);

}  // namespace

// The held bag's left click: on a water bucket the mix (9), on a world bag a transfer (11); anything else is the game's.
sg::Verdict PreBagLmb(const sg::Call& c) {
    static R::ParamOffset sPlayer{L"player"};
    N::Session* const s = RQ::internal::LinkedSession();
    if (!s) return sg::Verdict::Run;
    void* const player = ParamObject(c, sPlayer);
    if (s->role() == N::Role::Host) {
        void* const hit = player ? E::ReadMainPlayerHitActor(player) : nullptr;
        return B::KindOf(hit) == B::Kind::None ? sg::Verdict::Run : RQ::internal::HostPre(hit);
    }
    if (RQ::internal::ReplayInProgress()) return sg::Verdict::Cancel;   // a replay never acts unasked on a second target
    if (!player || !coop::players::Registry::Get().IsLocal(player) || B::KindOf(c.object) != B::Kind::Bag)
        return sg::Verdict::Run;
    void* const hit = E::ReadMainPlayerHitActor(player);
    const B::Kind hitKind = B::KindOf(hit);
    if (hitKind == B::Kind::Bag) return AskTransfer(*s, c.object, hit, 0);
    EL::ElementId eid = EL::kInvalidId;
    B::Plain p;
    if (hitKind != B::Kind::Plain || !PF::SharedTarget(hit, EL::ElementType::Prop, eid)) return sg::Verdict::Run;
    if (!B::ReadPlain(hit, p)) return CancelUnread("a bucket copy's members cannot be read");
    int32_t units = 0, maxUnits = 0;
    if (!B::ReadBag(c.object, units, maxUnits)) return CancelUnread("the held bag's units cannot be read");
    const int32_t need = MixNeed(p);
    // The game's own refusals (it shows its hint): no concrete in the bucket, no water, or not enough cement.
    if (!p.canConcrete || need <= 0 || units - need < 0) return sg::Verdict::Run;
    if (need > kMixNeedMax) return CancelUnread("the water makes more concrete than a bucket holds");
    void* local = nullptr;
    void* held = nullptr;
    std::wstring name;
    if (const char* why = ReadClientHold(local, name, held)) return CancelUnread(why);
    if (held != c.object) return sg::Verdict::Run;
    PF::Verb v;
    PutVerbInt(v, need);
    return AskFamily(*s, local, held, hit, eid, N::PropActFamily::BucketMix, &v);
}

// The held trowel's left click on a concrete bucket.
sg::Verdict PreFixerLmb(const sg::Call& c) {
    static R::ParamOffset sPlayer{L"player"};
    N::Session* const s = RQ::internal::LinkedSession();
    if (!s) return sg::Verdict::Run;
    void* const player = ParamObject(c, sPlayer);
    if (s->role() == N::Role::Host) {
        void* const hit = player ? E::ReadMainPlayerHitActor(player) : nullptr;
        return B::KindOf(hit) == B::Kind::Concrete ? RQ::internal::HostPre(hit) : sg::Verdict::Run;
    }
    if (RQ::internal::ReplayInProgress()) return sg::Verdict::Cancel;
    if (!player || !coop::players::Registry::Get().IsLocal(player) || B::KindOf(c.object) != B::Kind::Trowel)
        return sg::Verdict::Run;
    void* const hit = E::ReadMainPlayerHitActor(player);
    EL::ElementId eid = EL::kInvalidId;
    if (B::KindOf(hit) != B::Kind::Concrete || !PF::SharedTarget(hit, EL::ElementType::Prop, eid)) return sg::Verdict::Run;
    int32_t load = 0;
    B::Concrete cb;
    if (!B::ReadTrowel(c.object, load) || !B::ReadConcrete(hit, cb)) return CancelUnread("a trowel or concrete bucket cannot be read");
    if (load > 0 || cb.dry) return sg::Verdict::Run;   // the game's own hints: the tool still has concrete, the concrete is dry
    void* local = nullptr;
    void* held = nullptr;
    std::wstring name;
    if (const char* why = ReadClientHold(local, name, held)) return CancelUnread(why);
    if (held != c.object) return sg::Verdict::Run;
    return AskFamily(*s, local, held, hit, eid, N::PropActFamily::BucketTrowel, nullptr);
}

}  // namespace coop::prop_act_bucket
