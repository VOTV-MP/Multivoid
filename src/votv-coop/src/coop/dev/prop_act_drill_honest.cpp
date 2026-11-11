// coop/dev/prop_act_drill_honest.cpp -- the steps of the act drill's phase `honest`: one client takes and uses each
// family of shared prop, and the host judges that the take was its own commit and never a raw destroy. See
// coop/dev/prop_act_drill.h.

#include "coop/dev/prop_act_drill.h"

#include "coop/creatures/kerfur_entity.h"
#include "coop/dev/drill_props.h"
#include "coop/element/registry.h"
#include "coop/props/prop_act_host.h"
#include "coop/props/prop_act_request.h"
#include "coop/props/remote_prop.h"

#include "ue_wrap/actors/container_inventory.h"
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
#include <string>

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
    if (o < 0 || !c.locals) return sg::Verdict::Run;
    const std::wstring text = ue_wrap::ftext_utils::FTextToString(c.locals + o);
    UE_LOGI("%s game hint: '%ls'", kTag, text.c_str());
    if (text == kNoSpace) g_hintSeen = true;
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
    static R::InstanceOffset sUses{L"invRead_Uses"};
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
    if (!sponge || !E::CallMainPlayerAddObjectToFreeHotbarSlot(player, sponge, ue_wrap::engine::HotbarAdd{}, collected) || !collected)
        return "this client's own sponge could not be spawned and held";
    if (coop::prop_act_request::dev::PendingId() != 0) return "the hold of this client's own sponge opened a request";
    E::MainPlayerGrabState hand{};
    if (!E::ReadMainPlayerGrabState(player, hand) || !hand.mainHand) return "the hand holds no sponge";
    if (!WriteField<float>(sSpongeSoap, hand.mainHand, 0.f)) return "the sponge's soapAmount could not be written";
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

// The player's pockets: mainPlayer_C.inventoryComponent_player, and its volume fields as the game reads them.
void* PlayerInventoryComponent(void* player) {
    static R::InstanceOffset sComp{L"inventoryComponent_player"};
    const int32_t o = player ? sComp.Of(player) : -1;
    void* const comp = o < 0 ? nullptr : *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(player) + o);
    return comp && R::IsLive(comp) ? comp : nullptr;
}

// One own crowbar spawned and pocketed with no notice (its refusal is the step's, not the fill's); `taken` is the
// native return.
const char* PocketOwnCrowbar(void* player, bool* taken) {
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
    *taken = false;
    if (!own || !E::CallMainPlayerAddObjectToInventory(player, own, *taken)) return "an own spawn could not be spawned or pocketed";
    return nullptr;
}

// The inventory is filled the way a player fills it: one own crowbar pocketed per tick until the game's own volume rule
// refuses ("Not enough space": totalVolume + Get Volume(name) over maxVolume). On 0.10 build b284 that rule works only
// once the game's volume tables have arrived (the row waits for VolumesReady): before, Get Volume answers 0 for every name
// and no number of pockets fills the inventory. After, a crowbar is 2986 of the player's 50000.
const char* FillStep(void* player, bool* full) {
    bool taken = false;
    if (const char* why = PocketOwnCrowbar(player, &taken)) return why;
    *full = !taken;
    if (*full) LogPlayerVolumes(player, "when the pocket was refused");
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

// The wastebasket is loaded as the game loads it: an item dropped into its Box settles, and the bin's own tick freezes
// it, attaches it and lists it in itemsInside, after which checkPickup turns the bin's own canHold and canCollect off
// (so its canBeCollectedOrPickedUp answers refused). The bin must first have come to rest (its place unchanged across
// two polls) and stand upright; then a small item (the pills) is dropped above it and awaited to rest in turn; the step
// is done once the bin's data reads canCollect false, and a miss names the cause.
const char* LoadBin(void* bin, uint64_t sinceMs, bool* done) {
    if (sinceMs == 0) g_bin = BinLoad{};
    FVector binAt{};
    if (!E::TryGetActorLocation(bin, binAt)) return BinFail(bin, "the bin's place is unread");
    if (!g_bin.dropped) {
        const bool rest = g_bin.haveBinAt && Unmoved(g_bin.binAt, binAt);
        g_bin.binAt = binAt;
        g_bin.haveBinAt = true;
        if (!rest) return sinceMs > kBinLoadBoundMs ? BinFail(bin, "the bin never came to rest") : nullptr;
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
            return sinceMs - g_bin.phaseMs > kBinLoadBoundMs ? BinFail(bin, "the item never came to rest") : nullptr;
        g_bin.rested = true;
        g_bin.phaseMs = sinceMs;
        const float dx = at.X - binAt.X, dy = at.Y - binAt.Y, dz = at.Z - binAt.Z;
        if (dx * dx + dy * dy + dz * dz > kBinInsideCm * kBinInsideCm) return BinFail(bin, "the item did not land in the bin");
    }
    return sinceMs - g_bin.phaseMs > kBinTakeBoundMs ? BinFail(bin, "the bin never took the item") : nullptr;
}

// A frozen heavy prop, on the host's copy, which is the one the host's judge reads (frozen is not synced): the game's
// own setter, with active off. canBeCollectedOrPickedUp then refuses the hold twice over, as heavy and as frozen; and the
// freeze leaves the body not simulating, which the host's hold judge reads first (fit_physics, ahead of fit_take_refused in
// the order of coop/props/prop_act_host.cpp FirstRefusal), so that is the tally of the refusal the row asks for.
const char* FreezeHost(void* actor, uint64_t, bool* done) {
    if (!ue_wrap::prop::CallSetPropProps(actor, false, true, false, false)) return "setPropProps could not freeze the host's copy";
    *done = true;
    return nullptr;
}

constexpr Row kRows[] = {
    {"pocket",       kCrowbar, L"prop_crowbar_C",     Verb::Pocket, Ends::Inventory, nullptr},
    // The bin is second: it is dropped and must stand, and the rows walk the puppet on along a line whose far end slopes,
    // where a bin spawned there came to rest on its side (a placement the row cannot judge), so it goes early on the flat.
    {"loaded bin",   kBin,     L"prop_openContainer_wastebasket_C", Verb::Pocket, Ends::Refused, nullptr, Target::Host, true,
     false, nullptr, nullptr, false, PAH::Tally::FitTakeRefused, &LoadBin},
    {"hold crowbar", kCrowbar, L"prop_crowbar_C",     Verb::Hold,   Ends::Hand,      nullptr},
    {"hold food",    kFood,    L"prop_food_C",        Verb::Hold,   Ends::Hand,      nullptr},
    {"hold pills",   kPills,   L"prop_sleeppills_C",  Verb::Hold,   Ends::Hand,      nullptr},
    {"eat last use", kFood,    L"prop_food_C",        Verb::Use,    Ends::Gone,      &SetLastUse},
    {"mre",          kMre,     L"prop_food_mre_C",    Verb::Use,    Ends::Gone,      nullptr},
    {"soap",         kSoap,    L"prop_food_soap_C",   Verb::Soap,   Ends::Gone,      &SetUpSoap},
    {"pills",        kPills,   L"prop_sleeppills_C",  Verb::Use,    Ends::Gone,      nullptr},
    // The plush offers no option, so its last use is asked and refused. Then the classes the judge reads by their own
    // data: a food subclass, a prop the client drags, and last a frozen heavy prop, whose hold the
    // host refuses (fit_physics: the freeze stops the body, and that term is judged before canBeCollectedOrPickedUp's
    // fit_take_refused) and leaves in the world.
    {"erie plush",   kPlush,   L"prop_eriePlush_C",   Verb::Use,    Ends::Refused,   &SetLastUse, Target::Host, true, false,
     nullptr, nullptr, false, PAH::Tally::FitDenied},
    {"food subclass", kMreAB,  L"prop_food_mre_AB_C", Verb::Use,    Ends::Gone,      &SetLastUse},
    {"dragged pocket", kCrowbar, L"prop_crowbar_C",   Verb::Pocket, Ends::Inventory, nullptr, Target::Host, true, false,
     nullptr, nullptr, true},
    {"frozen heavy hold", kHeater, L"prop_heater_C",  Verb::Hold,   Ends::Refused,   nullptr, Target::Host, true, false,
     nullptr, nullptr, false, PAH::Tally::FitPhysics, &FreezeHost},
    // The calls that are the client's own (no request), a use that also destroys a second prop inside the latch, and last
    // the full inventory (FillStep: on b284 Get Volume answers 0, so the fill writes the volume over the limit), which
    // stays full for whatever would follow it.
    {"own spawn",       kCrowbar,  L"prop_crowbar_C",     Verb::Pocket, Ends::Inventory, nullptr, Target::Client, false, false, &MakeOwnCrowbar},
    {"native subclass", kShrimp,   L"prop_food_shrimp_C", Verb::Use,    Ends::NativeGone, &SetShrimpLastEat, Target::Host, false},
    {"non-last use",    kFood,     L"prop_food_C",        Verb::Use,    Ends::Unchecked, &SetTwoUses, Target::Host, false, true},
    {"trash native",    kChipPile, L"actorChipPile_C",    Verb::Hold,   Ends::Unchecked, nullptr, Target::Host, false},
    // The kerfur pocket is a bail: its volume (Get Volume 105905) is over the player's whole inventory (maxVolume 50000), so
    // additemadvanced refuses it with "Not enough space" and additemasobject leaves the actor alone (capture-0.10.0-b284.txt,
    // inventorycomponent_c.additemadvanced / additemasobject). That holds only once the game's volume tables have arrived,
    // about 2.5 minutes after a world loads (before, Get Volume answers 0 and the kerfur is taken, which an earlier version
    // of this row mistook for 0.10's rule): the row waits for them. The host's judge grants it (its bound is the largest
    // container any player can wear, 250000), so the client asks, is granted, and its own game bails: the abort path.
    {"kerfur pocket",   nullptr,   L"prop_kerfurOmega_C", Verb::Pocket, Ends::Stays,     nullptr, Target::HostKerfur, true, false,
     nullptr, nullptr, false, PAH::Tally::kCount, nullptr, true},
    {"kerfur hold",     nullptr,   L"prop_kerfurOmega_C", Verb::Hold,   Ends::Taken,     nullptr, Target::HostKerfur},
    {"second destroy",  kCrowbar,  L"prop_crowbar_C",     Verb::Pocket, Ends::Inventory, nullptr, Target::HostTwo},
    {"full",            kCrowbar,  L"prop_crowbar_C",     Verb::Pocket, Ends::Stays,     nullptr, Target::Host, true, false, nullptr, &FillStep},
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

const char* SetLastUse(void*, void* mirror) {
    static R::InstanceOffset sUses{L"invRead_Uses"};
    return WriteField<int32_t>(sUses, mirror, 1) ? nullptr : "the copy's uses could not be written";
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

bool VolumesReady() {
    float v = 0.f;
    bool known = false;
    return ue_wrap::prop_gather::VolumeOfName(L"crowbar", v, known) && known && v > 0.f;
}

void LogPlayerVolumes(void* player, const char* when) {
    void* const comp = PlayerInventoryComponent(player);
    float total = -1.f, max = -1.f;
    const bool ok = comp && ue_wrap::container_inventory::CurrentVolume(comp, total) && ue_wrap::container_inventory::MaxVolume(comp, max);
    UE_LOGI("%s player inventory %s: totalVolume=%.1f maxVolume=%.1f (read=%d)", kTag, when, total, max, ok ? 1 : 0);
}

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

const Phase& Honest() {
    static const Phase p{"honest", kRows, sizeof(kRows) / sizeof(kRows[0]), &Judge};
    return p;
}

}  // namespace coop::dev::prop_act_drill::detail
