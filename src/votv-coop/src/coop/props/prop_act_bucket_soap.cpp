// coop/props/prop_act_bucket_soap.cpp -- the soap (6) and jar (7) families of a water bucket; see coop/props/prop_act_bucket.h.
//
// A client's soap used on a bucket without alcohol adds its amount to the bucket's soap (prop_bucket.cpp:524-538): the bucket's
// `soap += amount`, then upd and dipEffect, and the soap is destroyed by the holder. A clean jar used on a brewed bucket with more
// than a third of its water (:481-517) takes maxHeight/2.4 of the water and gives the holder a brewed jar. Both ride one watch, the
// bucket's playerUsedOn, told apart by the held item. Each spends the hold on the host through hand_spend, which bounds the asks by
// the hand lane's use count, so neither takes a bucket of its own.

#include "coop/props/prop_act_bucket.h"

#include "coop/player/hand_spend.h"

#include "ue_wrap/actors/prop_use.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/facts/function_row.h"

#include <algorithm>

namespace coop::prop_act_bucket {

namespace PU = ue_wrap::prop_use;

namespace {

UE_FACT_FN(kRowUsedOn, "prop_act_bucket", L"prop_bucket_C", L"playerUsedOn", Declared);
constexpr int kTagUsedOn = 0x50425530;   // "PBU0"
constexpr const char* kDead =
    "a client's soap or clean jar on a water bucket runs on its own copy only, unasked, and the copies diverge";

// The held item the replay consumed, kept from the replay to its commit word: the hand is the player's own, so a destroyed item is
// the evidence the game's verb ran.
ue_wrap::CachedObjRef g_replayHeld;

// ---- the client's half ----

bool RunSoapOrJar(void* bucket, void* local, bool jar, int32_t amount) {
    std::wstring name;
    void* held = nullptr;
    // The hold may have changed in the round trip: the replay uses the player's hold of now, so only the asked one runs.
    if (!ue_wrap::miner::ReadHold(local, name, held) || !held) return false;
    if (jar ? !IsJarName(name) : !IsClass(held, L"prop_food_soap_C")) return false;
    int32_t have = 0;
    if (!jar && (!PU::ReadSoapAmount(held, have) || have != amount)) return false;
    g_replayHeld.Set(held);
    return PU::CallPlayerUsedOn(bucket, local, nullptr, ue_wrap::FVector{}, nullptr, held, R::FName{});
}

bool RunSoap(void* bucket, void* local, const PF::Verb& verb) {
    int32_t amount = 0;
    return VerbInt(verb, amount) && RunSoapOrJar(bucket, local, false, amount);
}

bool RunJar(void* bucket, void* local, const PF::Verb& verb) {
    return VerbZeroFrom(verb, 0) && RunSoapOrJar(bucket, local, true, 0);
}

// 1 when the game's verb destroyed the held item.
uint8_t ItemSpentCommit(void*, const PF::Verb&) { return g_replayHeld.Alive() ? 0 : 1; }

// ---- the host's half ----

std::optional<Tally> Judge(N::Session& session, uint8_t slot, const N::PropActRequestPayload& req, void* bucket) {
    if (!InRoster(slot)) return Tally::FitFamilyArg;
    g_judged[slot] = Judged{};
    const bool jar = req.family == static_cast<uint8_t>(N::PropActFamily::BucketJar);
    if (JudgeSkippedByRed(slot, jar ? "jar" : "soap")) return std::nullopt;
    B::Plain p;
    if (B::KindOf(bucket) != B::Kind::Plain) return Tally::FitFamilyArg;
    if (!B::ReadPlain(bucket, p)) return Tally::FitFamilyUnread;
    int32_t amount = 0;
    if (jar ? !VerbZeroFrom(req.verb, 0) : (!VerbInt(req.verb, amount) || amount < 0 || amount > kSoapAmountMax))
        return Tally::FitFamilyArg;
    // The hand evidence: the hold the sender announced ahead of this ask, the owed state applied first. A jar is told by its hold
    // name, a soap by its class; the class is what hand_spend counts the use against.
    coop::hand_item::ApplyOwedNow(session, slot);
    const std::wstring cls = coop::hand_item::HeldClass(slot);
    if (cls.empty() || (jar ? !IsJarName(coop::hand_item::HeldItem(slot)) : !coop::hand_item::HeldClassIsA(slot, L"prop_food_soap_C")))
        return Tally::FitFamilyHand;
    if (jar ? !(p.alcohol && p.height > p.maxHeight / 3.f) : p.alcohol) return Tally::FitFamilyRule;
    g_judged[slot] = Judged{true, req.family, amount, cls};
    return std::nullopt;
}

// A soap's commit of 1: the judge's condition read again on the host's copy, the hold spent, the amount added.
std::optional<Tally> ApplySoap(uint8_t slot, void* bucket) {
    if (!InRoster(slot) || !g_judged[slot].valid || g_judged[slot].family != static_cast<uint8_t>(N::PropActFamily::BucketSoap))
        return Tally::FitFamilyArg;
    const Judged j = g_judged[slot];
    if (ApplySkippedByRed(slot, "soap")) return std::nullopt;
    B::Plain p;
    if (!B::ReadPlain(bucket, p)) return FailedApply(slot, bucket, "soap", Tally::FitFamilyUnread);
    if (p.alcohol) return FailedApply(slot, bucket, "soap", Tally::FitFamilyRule);
    if (!coop::hand_spend::Spend(slot, j.cls.c_str(), 1)) return FailedApply(slot, bucket, "soap", Tally::FitFamilyHand);
    p.soap += j.value;
    B::WritePlain(bucket, p.height, p.soap, p.alcoholP, p.alcohol, p.alcReady);
    B::CallUpd(bucket);
    B::CallDipEffect(bucket);
    coop::bucket_sync::Note(coop::bucket_sync::Count::FamilyApplies);
    coop::bucket_sync::MarkDirty(bucket);
    UE_LOGI("prop_act: slot %u's soap of %d added to a water bucket (soap now %d)", static_cast<unsigned>(slot),
            static_cast<int>(j.value), static_cast<int>(p.soap));
    return std::nullopt;
}

// A jar's commit of 1: the water taken, as the BP's target-side lines do (the brewed jar is the holder's own).
std::optional<Tally> ApplyJar(uint8_t slot, void* bucket) {
    if (!InRoster(slot) || !g_judged[slot].valid || g_judged[slot].family != static_cast<uint8_t>(N::PropActFamily::BucketJar))
        return Tally::FitFamilyArg;
    const Judged j = g_judged[slot];
    if (ApplySkippedByRed(slot, "jar")) return std::nullopt;
    B::Plain p;
    if (!B::ReadPlain(bucket, p)) return FailedApply(slot, bucket, "jar", Tally::FitFamilyUnread);
    if (!(p.alcohol && p.height > p.maxHeight / 3.f)) return FailedApply(slot, bucket, "jar", Tally::FitFamilyRule);
    if (!coop::hand_spend::Spend(slot, j.cls.c_str(), 1)) return FailedApply(slot, bucket, "jar", Tally::FitFamilyHand);
    p.height = (std::max)(p.height - p.maxHeight / 2.4f, 0.f);
    B::WritePlain(bucket, p.height, p.soap, p.alcoholP, p.alcohol, p.alcReady);
    coop::bucket_sync::Note(coop::bucket_sync::Count::FamilyApplies);
    coop::bucket_sync::MarkDirty(bucket);
    UE_LOGI("prop_act: slot %u filled a jar from a brewed water bucket (water now %.1f)", static_cast<unsigned>(slot), p.height);
    return std::nullopt;
}

// ---- the registry's entries ----

const PF::WatchDef kUsedOnWatch[] = {{kRowUsedOn, kTagUsedOn, &PreUsedOn, "soap or jar on a bucket", kDead}};

// The two entries share one record per slot with the other four, so the slot and session events are carried by the soap's entry alone.
const PF::Family kSoap = {
    static_cast<uint8_t>(N::PropActFamily::BucketSoap), "bucket soap", EL::ElementType::Prop, PF::Apply::AtCommit, kUsedOnWatch, 1,
    &RunSoap, &ItemSpentCommit, &Judge, nullptr, &ApplySoap, &NoteAborted, &OnRelease, &OnPeerGone, &OnDisconnect};
const PF::Family kJar = {
    static_cast<uint8_t>(N::PropActFamily::BucketJar), "bucket jar", EL::ElementType::Prop, PF::Apply::AtCommit, nullptr, 0,
    &RunJar, &ItemSpentCommit, &Judge, nullptr, &ApplyJar, &NoteAborted, nullptr, nullptr, nullptr};
const PF::Registrar kRegisterSoap(kSoap);
const PF::Registrar kRegisterJar(kJar);

}  // namespace

// The soap's and the jar's watch, and the others' shared hooks.
sg::Verdict PreUsedOn(const sg::Call& c) {
    static R::ParamOffset sHold{L"holdObject"}, sPlayer{L"player"};
    N::Session* const s = RQ::internal::LinkedSession();
    if (!s) return sg::Verdict::Run;
    void* const bucket = c.object;
    if (s->role() == N::Role::Host) return RQ::internal::HostPre(bucket);
    if (RQ::internal::ReplayingTarget(bucket)) return sg::Verdict::Run;
    if (RQ::internal::ReplayInProgress()) return sg::Verdict::Cancel;   // a replay never acts unasked on a second target
    EL::ElementId eid = EL::kInvalidId;
    if (B::KindOf(bucket) != B::Kind::Plain || !PF::SharedTarget(bucket, EL::ElementType::Prop, eid))
        return sg::Verdict::Run;
    void* const player = ParamObject(c, sPlayer);
    if (!player || !coop::players::Registry::Get().IsLocal(player) || !ParamObject(c, sHold)) return sg::Verdict::Run;
    B::Plain p;
    if (!B::ReadPlain(bucket, p)) return CancelUnread("a bucket copy's members cannot be read");
    void* local = nullptr;
    void* held = nullptr;
    std::wstring name;
    if (const char* why = ReadClientHold(local, name, held)) return CancelUnread(why);
    if (p.alcohol) {
        // A brewed bucket takes only a clean jar with water enough; the game's own hint answers anything else.
        if (!IsJarName(name) || !(p.height > p.maxHeight / 3.f)) return sg::Verdict::Run;
        return AskFamily(*s, local, held, bucket, eid, N::PropActFamily::BucketJar, nullptr);
    }
    if (!IsClass(held, L"prop_food_soap_C")) return sg::Verdict::Run;   // the BP does nothing for another item
    int32_t amount = 0;
    if (!PU::ReadSoapAmount(held, amount) || amount < 0 || amount > kSoapAmountMax)
        return CancelUnread("the held soap's amount cannot be read");
    PF::Verb v;
    PutVerbInt(v, amount);
    return AskFamily(*s, local, held, bucket, eid, N::PropActFamily::BucketSoap, &v);
}

void OnRelease(uint8_t slot) {
    if (InRoster(slot)) g_judged[slot] = Judged{};
}

void OnPeerGone(uint8_t slot) {
    if (!InRoster(slot)) return;
    g_judged[slot] = Judged{};
    for (auto& family : g_budget) family[slot].Reset();
}

void OnDisconnect() {
    for (const char*& said : g_unreadSaid) said = nullptr;
    coop::bucket_hold::OnDisconnect();
    for (Judged& j : g_judged) j = Judged{};
    for (auto& family : g_budget)
        for (N::IntentBucket& b : family) b.Reset();
    g_replayHeld.Reset();
}

}  // namespace coop::prop_act_bucket
