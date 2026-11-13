// coop/dev/prop_act_drill_honest.cpp -- the steps of the act drill's phase `honest`: one client takes and uses each
// family of shared prop, and the host judges that the take was its own commit and never a raw destroy. See
// coop/dev/prop_act_drill.h.

#include "coop/dev/prop_act_drill.h"

#include "coop/creatures/kerfur_entity.h"
#include "coop/dev/drill_props.h"
#include "coop/element/registry.h"
#include "coop/props/prop_act_host.h"
#include "coop/props/prop_act_request.h"
#include "coop/props/host_spawn_watcher.h"
#include "coop/props/prop_food_sync.h"
#include "coop/props/remote_prop.h"

#include "ue_wrap/actors/food.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/prop_flags.h"
#include "ue_wrap/actors/prop_gather.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/field_io.h"
#include "ue_wrap/core/ftext_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/facts/function_row.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>
#include <unordered_set>
#include <vector>

namespace coop::dev::prop_act_drill::detail {
namespace {

namespace E   = ue_wrap::engine;
namespace R   = ue_wrap::reflection;
namespace PAH = coop::prop_act_host;
namespace PRP = coop::remote_prop;
namespace EL  = coop::element;
namespace sg  = ue_wrap::script_gate;
namespace DP  = coop::dev::drill_props;
using ue_wrap::FVector;

constexpr const wchar_t* kMre     = L"/Game/objects/prop_food_mre.prop_food_mre_C";
constexpr const wchar_t* kKit     = L"/Game/objects/prop_medkit_pack.prop_medkit_pack_C";
constexpr const wchar_t* kPills   = L"/Game/objects/prop_sleeppills.prop_sleeppills_C";
constexpr const wchar_t* kSoap    = L"/Game/objects/prop_food_soap.prop_food_soap_C";
constexpr const wchar_t* kPlush   = L"/Game/objects/prop_eriePlush.prop_eriePlush_C";
constexpr const wchar_t* kSponge  = L"/Game/objects/prop_sponge.prop_sponge_C";
constexpr const wchar_t* kShrimp  = L"/Game/objects/prop_food_shrimp.prop_food_shrimp_C";
constexpr const wchar_t* kMreAB   = L"/Game/objects/prop_food_mre_AB.prop_food_mre_AB_C";
constexpr const wchar_t* kBin     = L"/Game/objects/prop_openContainer_wastebasket.prop_openContainer_wastebasket_C";
constexpr const wchar_t* kHeater  = L"/Game/objects/prop_heater.prop_heater_C";
constexpr const wchar_t* kChipPile = L"/Game/objects/actorChipPile.actorChipPile_C";
constexpr const wchar_t* kKerfurProp = L"prop_kerfurOmega_C";   // dropKerfurProp's prop class
constexpr const wchar_t* kNoSpace = L"Not enough space";        // the hint a bailed pocket shows
constexpr float kOwnLiftCm = 60.f;                              // a client's own spawn, above its player's origin
constexpr float kBinDropCm = 80.f;                              // the item dropped into the bin, above the bin's location
constexpr float kBinUprightZ = 0.5f;                            // a bin whose up vector's Z is above this stands
constexpr float kBinInsideCm = 60.f;                            // a rest point within this of the bin's origin is in the bin
constexpr uint64_t kBinLoadBoundMs = 20000;                     // HOST: the bin's rest, then the item's rest, each from its start
constexpr uint64_t kBinTakeBoundMs = 10000;                     // HOST: the item's rest in the bin to canCollect off

// Every Blueprint addHint, read to tell the full inventory's refusal from any other hint.
UE_FACT_FN(kRowHint, "dev.prop_act_drill", nullptr, L"addHint", AnyClass);
constexpr int kTagHint = 0x41434448;   // "ACDH"
bool g_hintSeen = false;
bool g_hintWatch = false;

sg::Verdict OnHintPre(const sg::Call& c) {
    static R::ParamOffset sText{L"InText"};
    const int32_t o = sText.Of(c.function);
    if (o >= 0 && c.locals && ue_wrap::ftext_utils::FTextToString(c.locals + o) == kNoSpace) g_hintSeen = true;
    return sg::Verdict::Run;
}

struct FindCtx {
    EL::ElementId kerfurId;
    void* found;
};

template <class T>
bool WriteField(R::InstanceOffset& field, void* obj, T value) {
    const int32_t o = obj ? field.Of(obj) : -1;
    if (o < 0) return false;
    std::memcpy(static_cast<uint8_t*>(obj) + o, &value, sizeof(value));
    return true;
}

// The client's copy has a use to spare, so an eat of it destroys nothing and runs natively.
const char* SetTwoUses(void*, void* mirror) {
    static R::InstanceOffset sUses{L"uses"};
    return WriteField<int32_t>(sUses, mirror, 2) ? nullptr : "the copy's uses could not be written";
}

// The soap's last use on a dry sponge in the hand: this client spawns its own sponge and holds it natively (no request,
// since a client's own spawn is its own), writes the hand's sponge dry and the soap copy to one use.
const char* SetUpSoap(void* player, void* mirror) {
    static R::InstanceOffset sSpongeSoap{L"soapAmount"};
    static R::InstanceOffset sSoapAmount{L"soapAmount"};
    void* const cls = ue_wrap::asset_load::LoadObjectByPath(kSponge);
    FVector at{};
    if (!cls || !E::TryGetActorLocation(player, at)) return "the sponge class did not load or the player's place is unread";
    void* const sponge = E::SpawnActor(cls, {at.X, at.Y, at.Z + 60.f});
    bool collected = false;
    if (!sponge || !E::CallMainPlayerHoldObject(player, sponge, collected) || !collected)
        return "this client's own sponge could not be spawned and held";
    if (coop::prop_act_request::dev::PendingId() != 0) return "the hold of this client's own sponge opened a request";
    E::MainPlayerGrabState hand{};
    if (!E::ReadMainPlayerGrabState(player, hand) || !hand.holdingActor) return "the hand holds no sponge";
    if (!WriteField<float>(sSpongeSoap, hand.holdingActor, 0.f)) return "the sponge's soapAmount could not be written";
    return WriteField<int32_t>(sSoapAmount, mirror, 1) ? nullptr : "the soap copy's soapAmount could not be written";
}

// The client's own crowbar, spawned above its player: keyed (its init mints the key) and without a row, which the gate
// reads as the client's own. The cell is read back first, else the row proves nothing.
const char* MakeOwnCrowbar(void* player, void** out) {
    void* const cls = ue_wrap::asset_load::LoadObjectByPath(kCrowbar);
    FVector at{};
    if (!cls || !E::TryGetActorLocation(player, at)) return "the class did not load or the player's place is unread";
    void* const own = E::SpawnActor(cls, {at.X, at.Y, at.Z + kOwnLiftCm});
    if (!own) return "this client's own prop did not spawn";
    if (!DP::HasKey(own)) return "cell not reached: the spawn has no key";
    if (EL::Registry::Get().EidForActor(own) != EL::kInvalidId) return "cell not reached: the spawn already has a row";
    *out = own;
    return nullptr;
}

// One own crowbar pocketed with no notice (its refusal is the step's, not the fill's); false from the native call ends it.
const char* FillStep(void* player, bool* full) {
    // The class is loaded once and held: a load per tick is a reflected call the fill need not make.
    static ue_wrap::CachedObjRef sCrowbar;
    void* cls = sCrowbar.Get();
    if (!cls) {
        cls = ue_wrap::asset_load::LoadObjectByPath(kCrowbar);
        sCrowbar.Set(cls);
    }
    FVector at{};
    if (!cls || !E::TryGetActorLocation(player, at)) return "the class did not load or the player's place is unread";
    void* const own = E::SpawnActor(cls, {at.X, at.Y, at.Z + kOwnLiftCm});
    bool taken = false;
    if (!own || !E::CallPutObjectInventory2(player, own, true, taken)) return "an own spawn could not be spawned or pocketed";
    *full = !taken;
    return nullptr;
}

// The shrimp eats by its own counter: its eat takes one `shrimps` and destroys the actor at 0, and never touches
// `uses`. One `shrimps` makes the first eat consume the copy; `uses` is set as well, so the food predicate asks of the
// red arm's AnyClass food row.
const char* SetShrimpLastEat(void* player, void* mirror) {
    static R::InstanceOffset sShrimps{L"shrimps"};
    if (!WriteField<int32_t>(sShrimps, mirror, 1)) return "the shrimp copy's shrimps could not be written";
    return SetLastUse(player, mirror);
}

// LoadBin's progress, cleared by its first call (the row's step runs one bin at a time).
struct BinLoad {
    ue_wrap::CachedObjRef item;
    FVector binAt{}, itemAt{};
    bool haveBinAt = false, haveItemAt = false, dropped = false, rested = false;
    uint64_t phaseMs = 0;   // the sinceMs at which the current wait began
};
BinLoad g_bin;

bool Unmoved(const FVector& a, const FVector& b) { return a.X == b.X && a.Y == b.Y && a.Z == b.Z; }

constexpr float kBinUnread = -9.f;   // a value the bin's diagnostic could not read: outside every up-vector Z

// The bin's itemsInside count (the TArray's Num, read by the member's name), -1 when the member is not found.
int32_t ItemsInside(void* bin) {
    static R::InstanceOffset sItems{L"itemsInside"};
    const int32_t o = bin ? sItems.Of(bin) : -1;
    if (o < 0) return -1;
    return reinterpret_cast<const ue_wrap::field_io::TArrayView*>(static_cast<const uint8_t*>(bin) + o)->num;
}

// One line naming where the prep stood, written before each of its FAILs: the bin's place and up-Z, the item's rest
// point and its distance to the bin's centre, the bin's itemsInside count and its canCollect.
const char* BinFail(void* bin, const char* why) {
    FVector binAt{}, itemAt{};
    const bool haveBin = bin && E::TryGetActorLocation(bin, binAt);
    void* const item = g_bin.item.Alive() ? g_bin.item.Get() : nullptr;
    const bool haveItem = item && E::TryGetActorLocation(item, itemAt);
    float dist = kBinUnread;
    if (haveBin && haveItem) {
        const float dx = itemAt.X - binAt.X, dy = itemAt.Y - binAt.Y, dz = itemAt.Z - binAt.Z;
        dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    }
    const FVector up = bin ? E::GetActorUpVector(bin) : FVector{};
    const float upZ = (up.X == 0.f && up.Y == 0.f && up.Z == 0.f) ? kBinUnread : up.Z;
    ue_wrap::prop_gather::PropDataFlags flags{};
    const int collect = bin && ue_wrap::prop_gather::ReadPropData(bin, flags) ? (flags.canCollect ? 1 : 0) : -1;
    UE_LOGW("%s host bin prep: %s -- bin (%.0f, %.0f, %.0f) up-Z %.2f, item (%.0f, %.0f, %.0f) %.0f uu from the bin's "
            "centre, itemsInside %d, canCollect %d",
            kTag, why, binAt.X, binAt.Y, binAt.Z, upZ, itemAt.X, itemAt.Y, itemAt.Z, dist, ItemsInside(bin), collect);
    return why;
}

// The prep's own bounds: the physics settle or the bin's take had no event by its bound, so the run measured nothing. The
// diagnostic line is the same; the returned text carries the prefix the core reads as INVALID, never as a FAIL.
const char* BinInvalid(void* bin, const char* why) {
    BinFail(bin, why);
    static std::string text;
    text = std::string("INVALID: ") + why;
    return text.c_str();
}

// The wastebasket is loaded as the game loads it: an item dropped into its Box settles, and the bin's own tick freezes it
// and attaches it, which turns the bin's canCollect off (checkPickup). The bin must first have come to rest (its place
// unchanged across two polls) and stand upright; then a small item (the pills) is dropped above it and awaited to rest in
// turn; the step is done once the bin's data reads canCollect false, and a miss names the cause.
const char* LoadBin(void* bin, uint64_t sinceMs, bool* done) {
    if (sinceMs == 0) g_bin = BinLoad{};
    FVector binAt{};
    if (!E::TryGetActorLocation(bin, binAt)) return BinFail(bin, "the bin's place is unread");
    if (!g_bin.dropped) {
        const bool rest = g_bin.haveBinAt && Unmoved(g_bin.binAt, binAt);
        g_bin.binAt = binAt;
        g_bin.haveBinAt = true;
        if (!rest) return sinceMs > kBinLoadBoundMs ? BinInvalid(bin, "the bin never came to rest") : nullptr;
        const FVector up = E::GetActorUpVector(bin);
        if (up.X == 0.f && up.Y == 0.f && up.Z == 0.f) return BinFail(bin, "the bin's up vector is unread");
        if (up.Z <= kBinUprightZ) return BinFail(bin, "the bin came to rest on its side");
        void* const cls = ue_wrap::asset_load::LoadObjectByPath(kPills);
        if (!cls) return BinFail(bin, "the pills class did not load");
        void* const item = E::SpawnActor(cls, {binAt.X, binAt.Y, binAt.Z + kBinDropCm});
        if (!item) return BinFail(bin, "the item for the bin did not spawn");
        g_bin.item.Set(item);
        g_bin.dropped = true;
        g_bin.phaseMs = sinceMs;
        return nullptr;
    }
    ue_wrap::prop_gather::PropDataFlags flags{};
    if (!ue_wrap::prop_gather::ReadPropData(bin, flags)) return BinFail(bin, "the bin's propData could not be read");
    if (!flags.canCollect) {
        *done = true;
        return nullptr;
    }
    FVector at{};
    void* const item = g_bin.item.Alive() ? g_bin.item.Get() : nullptr;
    if (!item || !E::TryGetActorLocation(item, at))
        return BinFail(bin, "the item dropped for the bin is gone or its place is unread");
    if (!g_bin.rested) {
        const bool rest = g_bin.haveItemAt && Unmoved(g_bin.itemAt, at);
        g_bin.itemAt = at;
        g_bin.haveItemAt = true;
        if (!rest)
            return sinceMs - g_bin.phaseMs > kBinLoadBoundMs ? BinInvalid(bin, "the item never came to rest") : nullptr;
        g_bin.rested = true;
        g_bin.phaseMs = sinceMs;
        const float dx = at.X - binAt.X, dy = at.Y - binAt.Y, dz = at.Z - binAt.Z;
        if (dx * dx + dy * dy + dz * dz > kBinInsideCm * kBinInsideCm) return BinFail(bin, "the item did not land in the bin");
    }
    return sinceMs - g_bin.phaseMs > kBinTakeBoundMs ? BinInvalid(bin, "the bin never took the item") : nullptr;
}

// A frozen heavy prop, on the host's copy and on the client's (frozen is not synced, and the client's own hold of a
// heavy prop would answer "too heavy"): the game's own setter, with active off.
const char* FreezeHost(void* actor, uint64_t, bool* done) {
    if (!ue_wrap::prop::CallSetPropProps(actor, false, true, false, false)) return "setPropProps could not freeze the host's copy";
    *done = true;
    return nullptr;
}

const char* FreezeCopy(void*, void* mirror) {
    return ue_wrap::prop::CallSetPropProps(mirror, false, true, false, false) ? nullptr : "setPropProps could not freeze this client's copy";
}

// An MRE poisons one content with the chance in its own `poisonChance`, a value its purchase sets and a spawned one has
// at 0. The host's copy is given 1, so the open poisons a content and the client's judge has a carrier to find.
const char* PoisonMre(void* actor, uint64_t, bool* done) {
    const int32_t off = ue_wrap::reflection::FindPropertyOffset(ue_wrap::reflection::ClassOf(actor), L"poisonChance");
    if (off < 0) return "the MRE declares no poisonChance";
    const float always = 1.f;
    std::memcpy(static_cast<uint8_t*>(actor) + off, &always, sizeof(always));
    *done = true;
    return nullptr;
}

constexpr Row kRows[] = {
    {"pocket",       kCrowbar, L"prop_crowbar_C",     Verb::Pocket, Ends::Inventory, nullptr},
    {"hold crowbar", kCrowbar, L"prop_crowbar_C",     Verb::Hold,   Ends::Hand,      nullptr},
    {"hold food",    kFood,    L"prop_food_C",        Verb::Hold,   Ends::Hand,      nullptr},
    {"hold pills",   kPills,   L"prop_sleeppills_C",  Verb::Hold,   Ends::Hand,      nullptr},
    {"eat last use", kFood,    L"prop_food_C",        Verb::Use,    Ends::Gone,      &SetLastUse},
    {"mre",          kMre,     L"prop_food_mre_C",    Verb::Use,    Ends::Unpacked,  nullptr, Target::Host, true, false,
     nullptr, nullptr, false, PAH::Tally::kCount, &PoisonMre, nullptr, true},
    {"soap",         kSoap,    L"prop_food_soap_C",   Verb::Soap,   Ends::Gone,      &SetUpSoap},
    {"pills",        kPills,   L"prop_sleeppills_C",  Verb::Use,    Ends::Gone,      nullptr},
    // The plush offers no option, so its last use is asked and refused. Then the classes the judge reads by their own
    // data: a food subclass, a loaded bin, a prop the client drags, and last a frozen heavy prop (its hold leaves a
    // heater in the client's hand, and the drag must start from an empty one).
    {"erie plush",   kPlush,   L"prop_eriePlush_C",   Verb::Use,    Ends::Refused,   &SetLastUse, Target::Host, true, false,
     nullptr, nullptr, false, PAH::Tally::FitDenied},
    {"food subclass", kMreAB,  L"prop_food_mre_AB_C", Verb::Use,    Ends::Gone,      &SetLastUse},
    {"loaded bin",   kBin,     L"prop_openContainer_wastebasket_C", Verb::Pocket, Ends::Refused, nullptr, Target::Host, true,
     false, nullptr, nullptr, false, PAH::Tally::FitCanCollect, &LoadBin},
    {"dragged pocket", kCrowbar, L"prop_crowbar_C",   Verb::Pocket, Ends::Inventory, nullptr, Target::Host, true, false,
     nullptr, nullptr, true},
    {"frozen heavy hold", kHeater, L"prop_heater_C",  Verb::Hold,   Ends::Taken,     &FreezeCopy, Target::Host, true, false,
     nullptr, nullptr, false, PAH::Tally::kCount, &FreezeHost},
    // The calls that are the client's own (no request), the pocket and the hold of a kerfur's prop (the pocket stays, the
    // hold is taken), a use that also destroys a second prop inside the latch, and the full inventory, which stays
    // full for whatever would follow it.
    {"own spawn",       kCrowbar,  L"prop_crowbar_C",     Verb::Pocket, Ends::Inventory, nullptr, Target::Client, false, false, &MakeOwnCrowbar},
    {"native subclass", kShrimp,   L"prop_food_shrimp_C", Verb::Use,    Ends::NativeGone, &SetShrimpLastEat, Target::Host, false},
    {"non-last use",    kFood,     L"prop_food_C",        Verb::Use,    Ends::Unchecked, &SetTwoUses, Target::Host, false, true},
    {"trash native",    kChipPile, L"actorChipPile_C",    Verb::Hold,   Ends::Unchecked, nullptr, Target::Host, false},
    {"kerfur pocket",   nullptr,   L"prop_kerfurOmega_C", Verb::Pocket, Ends::Stays,     nullptr, Target::HostKerfur},
    {"kerfur hold",     nullptr,   L"prop_kerfurOmega_C", Verb::Hold,   Ends::Taken,     nullptr, Target::HostKerfur},
    {"second destroy",  kCrowbar,  L"prop_crowbar_C",     Verb::Pocket, Ends::Inventory, nullptr, Target::HostTwo},
    {"full",            kCrowbar,  L"prop_crowbar_C",     Verb::Pocket, Ends::Stays,     nullptr, Target::Host, true, false, nullptr, &FillStep},
    // A first aid kit's `spawn` rolls the props of its `drop` list, and whether a kit spawned bare holds any is not measured
    // (the host's grant line says it): the step ends INVALID when it rolled nothing, which stops the phase, so it is the
    // last row and masks none. Otherwise it passes when each of the host's births has its mirror here. An unpack destroys
    // the client's copy and takes no inventory slot, so the full inventory the row before it leaves does not touch it.
    {"first aid kit", kKit,    L"prop_medkit_pack_C", Verb::Use,    Ends::Unpacked,  nullptr},
};

uint32_t Since(uint32_t now, uint32_t then) { return now - then; }

// A call that runs natively: the host answered nothing and took no commit.
Judged JudgeNative(const Row& row, coop::element::ElementId eid, const Baseline& base, std::string* why) {
    const uint32_t answered = Since(PAH::AnsweredTotal(kClientSlot), base.answered);
    const uint32_t commits = Since(PAH::CommitsOf(kClientSlot, PAH::internal::CommitStep::Destroy), base.destroys) +
                             Since(PAH::CommitsOf(kClientSlot, PAH::internal::CommitStep::Abort), base.aborts);
    if (answered != 0 || commits != 0) {
        *why = std::to_string(answered) + " replies and " + std::to_string(commits) + " commits for a native call";
        return Judged::Fail;
    }
    if (row.hostKeeps && !coop::element::LivePropActor(eid)) {
        *why = "the host's prop is gone after a use that destroys nothing";
        return Judged::Fail;
    }
    *why = row.hostKeeps ? "the answered total unchanged, no commit, the eid alive" : "the answered total unchanged, no commit";
    return Judged::Pass;
}

// The take that bailed on the client (a full inventory, or a volume its container cannot hold): one abort, no destroy,
// the prop alive, and a kerfur item's record still bound to it.
Judged JudgeAborted(const Row& row, coop::element::ElementId eid, const Baseline& base, std::string* why) {
    const uint32_t aborts = Since(PAH::CommitsOf(kClientSlot, PAH::internal::CommitStep::Abort), base.aborts);
    if (aborts == 0) {
        *why = "no abort commit from the client yet";
        return Judged::Wait;
    }
    const uint32_t destroys = Since(PAH::CommitsOf(kClientSlot, PAH::internal::CommitStep::Destroy), base.destroys);
    const uint32_t raw = PRP::dev::WireDestroysNaming(eid);
    if (aborts != 1 || destroys != 0 || raw != 0) {
        *why = std::to_string(aborts) + " aborts, " + std::to_string(destroys) + " destroy commits, " + std::to_string(raw) +
               " raw PropDestroy naming the target";
        return Judged::Fail;
    }
    if (!coop::element::LivePropActor(eid)) {
        *why = "the eid is gone on the host after an abort";
        return Judged::Fail;
    }
    if (row.target == Target::HostKerfur && coop::kerfur_entity::GetKerfurIdForEid(eid) == coop::element::kInvalidId) {
        *why = "the kerfur record no longer names the eid after an abort";
        return Judged::Fail;
    }
    *why = row.target == Target::HostKerfur ? "one abort commit, no destroy, the eid alive, the kerfur record still bound"
                                            : "one abort commit, no destroy, the eid alive";
    return Judged::Pass;
}

// The host's judge refused the act: one WrongFamily answer, by the row's own term, and the target stands.
Judged JudgeRefused(const Row& row, coop::element::ElementId eid, const Baseline& base, std::string* why) {
    const uint32_t refused = Since(PAH::AnsweredFor(kClientSlot, coop::net::PropActReason::WrongFamily), base.wrongFamily);
    if (refused == 0) {
        *why = "no WrongFamily answer for the client yet";
        return Judged::Wait;
    }
    const uint32_t byTerm = Since(PAH::TallyOf(row.fitTally), base.fit);
    if (refused != 1 || byTerm != 1) {
        *why = std::to_string(refused) + " WrongFamily answers and " + std::to_string(byTerm) + " " +
               PAH::TallyName(row.fitTally) + " refusals, 1 of each expected";
        return Judged::Fail;
    }
    if (!coop::element::LivePropActor(eid)) {
        *why = "the eid is gone on the host after a refusal";
        return Judged::Fail;
    }
    *why = std::string("one WrongFamily answer by ") + PAH::TallyName(row.fitTally) + ", the eid alive";
    return Judged::Pass;
}

// The client's take was its own commit (one destroy commit, the eid dead) and no raw PropDestroy of the client's names
// the target; a HostTwo row has exactly one that names the second prop, the destroy that leaves the latch.
Judged JudgeTaken(const Row& row, coop::element::ElementId eid, const Baseline& base, std::string* why) {
    const uint32_t commits = Since(PAH::CommitsOf(kClientSlot, PAH::internal::CommitStep::Destroy), base.destroys);
    if (commits == 0) {
        *why = "no destroy commit from the client yet";
        return Judged::Wait;
    }
    if (commits != 1) {
        *why = std::to_string(commits) + " destroy commits for one take";
        return Judged::Fail;
    }
    if (coop::element::LivePropActor(eid)) {
        *why = "the eid is still live on the host after the commit";
        return Judged::Fail;
    }
    if (row.target == Target::HostKerfur) {
        if (!base.kerfurBound) {
            *why = "the prop's eid named no kerfur record when the step began";
            return Judged::Fail;
        }
        if (coop::kerfur_entity::GetKerfurIdForEid(eid) != coop::element::kInvalidId) {
            *why = "the kerfur record still names the eid after the commit";
            return Judged::Fail;
        }
    }
    // An MRE's or a first aid kit's contents are rolled on the host's copy before its destroy, and no other take rolls
    // anything.
    const bool unpacks = std::wcscmp(row.leaf, L"prop_food_mre_C") == 0 || std::wcscmp(row.leaf, L"prop_medkit_pack_C") == 0;
    const uint32_t wantOpens = unpacks ? 1 : 0;
    const uint32_t opens = Since(PAH::UnpacksRolled(), base.unpacksRolled);
    if (opens != wantOpens) {
        *why = std::to_string(opens) + " unpacks on the host, " + std::to_string(wantOpens) + " expected";
        return Judged::Fail;
    }
    const uint32_t wantRaw = row.target == Target::HostTwo ? 1 : 0;
    const uint32_t raw = PRP::dev::WireDestroysNaming(eid);
    if (raw != 0) {
        *why = std::to_string(raw) + " raw PropDestroy naming the target, 0 expected";
        return Judged::Fail;
    }
    if (wantRaw) {
        const uint32_t second = PRP::dev::WireDestroysNaming(base.eid2);
        if (second < wantRaw) {
            *why = "the second prop's destroy has not come through the seam: " + std::to_string(second) +
                   " raw PropDestroy naming the second prop";
            return Judged::Wait;
        }
        if (second != wantRaw) {
            *why = std::to_string(second) + " raw PropDestroy naming the second prop, " + std::to_string(wantRaw) + " expected";
            return Judged::Fail;
        }
    }
    if (wantOpens) {
        *why = "one destroy commit, the eid dead, the contents rolled on the host, no raw PropDestroy from the client";
        return Judged::Pass;
    }
    *why = wantRaw ? "one destroy commit, the eid dead, the second prop's destroy through the seam"
                   : row.target == Target::HostKerfur
                         ? "one destroy commit, the eid dead, the kerfur record released, no raw PropDestroy"
                         : "one destroy commit, the eid dead, no raw PropDestroy from the client";
    return Judged::Pass;
}

// The host's side of every step here.
Judged Judge(const Row& row, coop::element::ElementId eid, const Baseline& base, std::string* why) {
    if (!row.asks) return JudgeNative(row, eid, base, why);
    if (row.ends == Ends::Refused) return JudgeRefused(row, eid, base, why);
    if (row.ends == Ends::Stays) return JudgeAborted(row, eid, base, why);
    return JudgeTaken(row, eid, base, why);
}

}  // namespace

// A food at ripeness 0 or below ragdolls its eater unless the eater is strong (prop_food.cpp, eat: `ripeness <= 0` and not
// `isStrong` call ragdollMode), and a ragdolled puppet sinks, so the next step finds no floor to spawn on. The copy is
// made fresh (the class default, 7200) as it is made a last use, and its ripeness is said, so a ragdoll after the eat
// names its cause.
constexpr float kFreshRipeness = 7200.f;

const char* SetLastUse(void*, void* mirror) {
    static R::InstanceOffset sUses{L"uses"};
    if (!WriteField<int32_t>(sUses, mirror, 1)) return "the copy's uses could not be written";
    ue_wrap::food::Condition c;
    if (ue_wrap::food::Read(mirror, c)) {
        UE_LOGI("%s c1 eaten food: ripeness %.0f before, made fresh (%.0f)", kTag, c.ripeness, kFreshRipeness);
        c.ripeness = kFreshRipeness;
        if (!ue_wrap::food::Write(mirror, c)) return "the copy's ripeness could not be written";
    }
    return nullptr;
}

bool HintWatchLive() {
    if (!g_hintWatch) g_hintWatch = sg::WatchName(kRowHint, kTagHint, &OnHintPre, nullptr);
    if (!g_hintWatch) return false;
    sg::ResolvePendingNames();
    return sg::NameWatchLive(kRowHint, kTagHint);
}

void HintWatchOff() {
    if (!g_hintWatch) return;
    sg::UnwatchName(kRowHint, kTagHint, &OnHintPre, nullptr);
    g_hintWatch = false;
}

void ClearHint() { g_hintSeen = false; }
bool HintSeen() { return g_hintSeen; }

void* FindKerfurProp(EL::ElementId kerfurId) {
    void* const cls = ue_wrap::object_index::ClassByName(kKerfurProp);
    if (!cls) return nullptr;
    FindCtx ctx{kerfurId, nullptr};
    ue_wrap::object_index::ForEachInstance(
        cls,
        [](void* p, void* obj, int32_t) {
            auto* c = static_cast<FindCtx*>(p);
            const EL::ElementId eid = EL::Registry::Get().EidForActor(obj);
            if (!c->found && eid != EL::kInvalidId && coop::kerfur_entity::GetKerfurIdForEid(eid) == c->kerfurId)
                c->found = obj;
        },
        &ctx);
    return ctx.found;
}

// ---- the unpack steps' host events (an MRE, a first aid kit) -------------------------------------------------------

namespace {
bool g_contentsSaid = false;
}

std::string ContentsHead(const Row& r) { return std::string(kTag) + " host " + r.name + " contents "; }

void UnpackHostReady() {
    g_contentsSaid = false;
    coop::host_spawn_watcher::dev::ArmSpawnRecord(true);   // from before the client acts: a content's spawn may go out inside the roll
}

// HOST, each poll while it waits for the client on an unpack step. The host's own commit counter is the no-change point of
// the open-off red: the commit has been processed (a destroy commit counted, the eid dead), so an unpack that did not
// happen will not happen. The contents are the set of actors the unpack's roll made, fixed when its spawn returned:
// prop_act_host brackets the call with host_spawn_watcher's birth capture (the Finish post-hook records every spawn
// finished on the game thread inside it), so they are named exactly, never counted from a world scan or a queue. A food
// row's contents are the foods of that set (the roll's other spawns, its pouring props, are no content); any other row's
// are its prop births, of any class. Once every content has its eid and, a keyed food, its FoodState on the wire, the host
// says how many there are, how many it poisoned and their eids (n=0 when the roll made none): a food's line follows its
// FoodState on the Default lane (session_lanes.h LaneForKind, FIFO), so a client that reads it has taken them. A content
// that never registers leaves the line unsaid, and the client's bound says INVALID.
const char* UnpackHostWatch(const Row& row, EL::ElementId eid, const Baseline& base) {
    if (Since(PAH::CommitsOf(kClientSlot, PAH::internal::CommitStep::Destroy), base.destroys) == 0 || EL::LivePropActor(eid))
        return nullptr;
    if (Since(PAH::UnpacksRolled(), base.unpacksRolled) == 0)
        return "the host processed the commit with no unpack: 0 unpacks on the host, 1 expected";
    if (g_contentsSaid) return nullptr;
    size_t fresh = 0, poisoned = 0;
    std::string eids;
    bool ready = true;
    for (const ue_wrap::CachedObjRef& born : coop::host_spawn_watcher::dev::CapturedBirths()) {
        void* const actor = born.Get();
        if (!actor) continue;
        const bool food = ue_wrap::food::IsFood(actor);
        if (row.foodContents ? !food : !ue_wrap::prop::IsDescendantOfProp(actor)) continue;
        // A kerfur's prop has the KerfurConvert as its wire signal and no PropSpawn (prop_lifecycle.cpp, the conversion
        // successor): it is no content a client should expect a mirror of.
        if (coop::kerfur_entity::IsKerfurPropClass(R::ClassOf(actor))) continue;
        ++fresh;
        // The watcher's drain is what sends a birth's spawn (or the init seam did, inside the roll): a birth that has left
        // its pending queue holding an eid and no send will never be sent (the drain skips a tracked birth, and the census
        // expresses only a prop with no eid), so the verdict is known and said now.
        const bool pending = coop::host_spawn_watcher::dev::StillPending(actor);
        const EL::ElementId id = EL::Registry::Get().EidForActor(actor);
        if (id == EL::kInvalidId) {
            ready = false;   // a straggler with no eid yet may still be minted and sent by the periodic census
            continue;
        }
        // The eid is minted before its PropSpawn goes out (the census walk mints with no send): the line waits for the send,
        // which is what makes a client's missing mirror a loss and no wait, and a send the session refused is the host's own fail.
        if (coop::host_spawn_watcher::dev::SpawnRefused(id)) {
            static std::string text;
            text = "the host's PropSpawn for content eid " + std::to_string(static_cast<unsigned>(id)) +
                   " was refused by the session";
            return text.c_str();
        }
        if (!coop::host_spawn_watcher::dev::SpawnSent(id)) {
            if (!pending) {
                static std::string unsent;
                unsent = "content eid " + std::to_string(static_cast<unsigned>(id)) +
                         " was never sent: it left the watcher's queue with no PropSpawn (tracked before its drain with no send of its own)";
                return unsent.c_str();
            }
            ready = false;
            continue;
        }
        eids += (eids.empty() ? "" : ",") + std::to_string(static_cast<unsigned>(id));
        if (!food) continue;
        // A content with no Key is counted by its eid and is never sent a FoodState (the lane sends keyed foods only).
        const std::wstring key = ue_wrap::prop::GetInteractableKeyString(actor);
        if (!key.empty() && key != L"None" && !coop::props::prop_food_sync::dev::StateSentFor(key)) ready = false;
        ue_wrap::food::Poison poison;
        if (ue_wrap::food::ReadPoison(actor, poison) && poison.delay > 0.f && poison.strength > 0.f) ++poisoned;
    }
    if (!ready) return nullptr;
    g_contentsSaid = true;
    coop::host_spawn_watcher::dev::ArmSpawnRecord(false);
    UE_LOGI("%s host %s: contents counted, n=%zu eids %s", kTag, row.name, fresh, eids.empty() ? "none" : eids.c_str());
    Say(ContentsHead(row) + "n=" + std::to_string(fresh) + " poisoned=" + std::to_string(poisoned) + " eids=" + eids);
    return nullptr;
}

const Phase& Honest() {
    static const Phase p{"honest", kRows, sizeof(kRows) / sizeof(kRows[0]), &Judge};
    return p;
}

}  // namespace coop::dev::prop_act_drill::detail
