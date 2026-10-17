// coop/dev/prop_act_drill_honest.cpp -- the steps of the act drill's phase `honest`: one client takes and uses each
// family of shared prop, and the host judges that the take was its own commit and never a raw destroy. See
// coop/dev/prop_act_drill.h.

#include "coop/dev/prop_act_drill.h"

#include "coop/dispatch/event_feed.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/props/prop_act_host.h"
#include "coop/props/prop_act_request.h"

#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"

#include <cstdint>
#include <cstring>
#include <string>

namespace coop::dev::prop_act_drill::detail {
namespace {

namespace E   = ue_wrap::engine;
namespace R   = ue_wrap::reflection;
namespace PAH = coop::prop_act_host;
using ue_wrap::FVector;

constexpr const wchar_t* kCrowbar = L"/Game/objects/prop_crowbar.prop_crowbar_C";
constexpr const wchar_t* kFood    = L"/Game/objects/prop_food.prop_food_C";
constexpr const wchar_t* kMre     = L"/Game/objects/prop_food_mre.prop_food_mre_C";
constexpr const wchar_t* kPills   = L"/Game/objects/prop_sleeppills.prop_sleeppills_C";
constexpr const wchar_t* kSoap    = L"/Game/objects/prop_food_soap.prop_food_soap_C";
constexpr const wchar_t* kPlush   = L"/Game/objects/prop_eriePlush.prop_eriePlush_C";
constexpr const wchar_t* kSponge  = L"/Game/objects/prop_sponge.prop_sponge_C";

template <class T>
bool WriteField(R::InstanceOffset& field, void* obj, T value) {
    const int32_t o = obj ? field.Of(obj) : -1;
    if (o < 0) return false;
    std::memcpy(static_cast<uint8_t*>(obj) + o, &value, sizeof(value));
    return true;
}

// The client's copy is on its last use, so an eat of it is a destroy the watch asks the host about.
const char* SetLastUse(void*, void* mirror) {
    static R::InstanceOffset sUses{L"uses"};
    return WriteField<int32_t>(sUses, mirror, 1) ? nullptr : "the copy's uses could not be written";
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

constexpr Row kRows[] = {
    {"pocket",       kCrowbar, L"prop_crowbar_C",     Verb::Pocket, Ends::Inventory, nullptr},
    {"hold crowbar", kCrowbar, L"prop_crowbar_C",     Verb::Hold,   Ends::Hand,      nullptr},
    {"hold food",    kFood,    L"prop_food_C",        Verb::Hold,   Ends::Hand,      nullptr},
    {"hold pills",   kPills,   L"prop_sleeppills_C",  Verb::Hold,   Ends::Hand,      nullptr},
    {"eat last use", kFood,    L"prop_food_C",        Verb::Use,    Ends::Gone,      &SetLastUse},
    {"mre",          kMre,     L"prop_food_mre_C",    Verb::Use,    Ends::Gone,      nullptr},
    {"soap",         kSoap,    L"prop_food_soap_C",   Verb::Soap,   Ends::Gone,      &SetUpSoap},
    {"pills",        kPills,   L"prop_sleeppills_C",  Verb::Use,    Ends::Gone,      nullptr},
    {"erie plush",   kPlush,   L"prop_eriePlush_C",   Verb::Use,    Ends::Gone,      &SetLastUse},
};

// The host's side of every step here: the client's take was its own commit (one destroy commit, the eid dead) and
// no raw destroy of the client's came through the seam.
Judged Judge(const Row&, coop::element::ElementId eid, const Baseline& base, std::string* why) {
    const uint32_t commits = PAH::CommitsOf(kClientSlot, PAH::internal::CommitStep::Destroy) - base.destroys;
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
    const uint32_t raw = coop::event_feed::DrainedFrom(kClientSlot, coop::net::ReliableKind::PropDestroy) - base.rawDestroys;
    if (raw != 0) {
        *why = std::to_string(raw) + " raw PropDestroy from the client";
        return Judged::Fail;
    }
    *why = "one destroy commit, the eid dead, no raw PropDestroy from the client";
    return Judged::Pass;
}

}  // namespace

const Phase& Honest() {
    static const Phase p{"honest", kRows, sizeof(kRows) / sizeof(kRows[0]), &Judge};
    return p;
}

}  // namespace coop::dev::prop_act_drill::detail
