// harness/autotest/autotest_prop_props.cpp -- TEST-ONLY: the toolgun's prop-properties tool across two peers, first increment.
//
// Two premises are measured before any arm is built. I1a: the host presses tool_propProps_C on a keyed prop it laid on the
// floor, with the tool's isFrozen set the way the spawn menu's assignParameters sets it (the tool reads is_static,
// is_frozen, is_active and is_sleeping from spawnmenu->toolParameters into its four fields at the toolgun's click, and
// init is the whole press), and logs the prop's flags before and after. I1b: the client takes a toolgun into its hand
// the way a player's pick does and the host logs whether hand_item::HeldClassIs says so for the client's slot.
// The verdict is INVALID by design: the drill only measures so far, and the arms come next. Gated by
// VOTVCOOP_RUN_PROP_PROPS; the peers' folder is VOTVCOOP_CAN_DIR.

#include "harness/autotest.h"
#include "disc_drills.h"   // co-located private header (src tree, not include/)
#include "harness/autotest/peer_signal.h"

#include "coop/dev/drill_hand.h"
#include "coop/dev/drill_props.h"
#include "coop/element/registry.h"
#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"
#include "coop/session/rig_verdict.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/desk/dish_writers.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/hit_result.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace harness::autotest {
namespace {

namespace DP = coop::dev::drill_props;
namespace DW = ue_wrap::dish_writers;
namespace E  = ue_wrap::engine;
namespace PS = peer_signal;
namespace R  = ue_wrap::reflection;
namespace RV = coop::rig_verdict;
using disc_drill::OnGameThread;

constexpr const char* kTag = "prop-props";
constexpr const wchar_t* kSackPath = L"/Game/objects/prop_pointSack.prop_pointSack_C";
constexpr const wchar_t* kToolPath = L"/Game/objects/tool_propProps.tool_propProps_C";
constexpr const wchar_t* kToolgunPath = L"/Game/objects/prop_toolgun.prop_toolgun_C";
constexpr const wchar_t* kToolgunItem = L"toolgun";
constexpr const wchar_t* kToolgunClass = L"prop_toolgun_C";
constexpr float kSackAheadCm = 150.f;
constexpr float kFloorDownCm = 400.f;
constexpr float kMinColumnCm = 5.f;
constexpr float kRiseCm = 40.f;
constexpr int kJoinSec = 480;
constexpr int kSignalSec = 240;
constexpr int kEidSec = 30;
constexpr int kHandSec = 30;

struct Flags {
    int isStatic = -1, frozen = -1, sleep = -1;
};

Flags ReadFlags(void* prop) {
    Flags f;
    if (!prop) return f;
    f.isStatic = ue_wrap::prop::IsStatic(prop) ? 1 : 0;
    f.frozen = ue_wrap::prop::IsFrozen(prop) ? 1 : 0;
    f.sleep = ue_wrap::prop::IsSleeping(prop) ? 1 : 0;
    return f;
}

bool WriteToolBool(void* tool, const wchar_t* field, bool value) {
    const int32_t off = R::FindPropertyOffset(R::ClassOf(tool), field);
    if (off < 0) return false;
    const bool v = value;
    std::memcpy(static_cast<uint8_t*>(tool) + off, &v, sizeof(v));
    return true;
}

// The sack laid on the floor ahead of the host: a plain keyed prop of no special class. Game thread.
void* LaySack(void* me) {
    void* cls = ue_wrap::asset_load::LoadObjectByPath(kSackPath);
    ue_wrap::FVector at{};
    if (!cls || !E::TryGetActorLocation(me, at)) return nullptr;
    const ue_wrap::FVector fwd = E::GetActorForwardVector(me);
    const float len = std::sqrt(fwd.X * fwd.X + fwd.Y * fwd.Y);
    if (len < 0.1f) return nullptr;
    void* sack = nullptr;
    const DP::Drop d = DP::DropOnFloor(me, cls, nullptr, at.X + fwd.X / len * kSackAheadCm,
                                       at.Y + fwd.Y / len * kSackAheadCm, at.Z, kFloorDownCm, kMinColumnCm, kRiseCm,
                                       &sack);
    return d == DP::Drop::Spawned ? sack : nullptr;
}

// One press of the tool on `target` with isFrozen set, as the toolgun's click runs it: a fresh tool whose four fields hold
// the values assignParameters would leave in them, the toolgun's outHit on the target, and init(toolgun). The tool
// destroys itself in its body; one left standing is destroyed here. Game thread.
bool Press(void* me, void* target, const char** why) {
    void* toolCls = ue_wrap::object_index::ClassByName(L"tool_propProps_C");
    ue_wrap::FVector at{};
    if (!toolCls || !E::TryGetActorLocation(me, at)) {
        *why = "the tool's class or the player's location is missing";
        return false;
    }
    const ue_wrap::FVector up{at.X, at.Y, at.Z + 60.f};
    void* toolgun = DW::SpawnToolgun(up);
    void* tool = DW::SpawnTool(toolCls, up);
    ue_wrap::FVector tgt{};
    if (!toolgun || !tool || !E::TryGetActorLocation(target, tgt)) {
        *why = "the toolgun, the tool or the target's location is missing";
        return false;
    }
    const bool wrote = WriteToolBool(tool, L"isStatic", false) && WriteToolBool(tool, L"isFrozen", true) &&
                       WriteToolBool(tool, L"isActive", false) && WriteToolBool(tool, L"isSleeping", false);
    const bool ran = wrote &&
                     ue_wrap::hit_result::WriteField(toolgun, L"outHit", target, ue_wrap::prop::GetStaticMesh(target), tgt) &&
                     DW::CallToolInit(tool, toolgun);
    if (!ran) *why = wrote ? "outHit could not be written or init did not run" : "the tool's fields did not resolve";
    if (tool && R::IsLive(tool)) E::DestroyActor(tool);
    if (toolgun && R::IsLive(toolgun)) E::DestroyActor(toolgun);
    return ran;
}

void RunHost() {
    if (!disc_drill::HostWorldUp()) return;
    PS::Mark("host_ready");
    if (!PS::Await("client_ready", kJoinSec)) return;
    ue_wrap::asset_load::LoadObjectByPath(kToolgunPath);
    ue_wrap::asset_load::LoadObjectByPath(kToolPath);

    // I1a.
    void* sack = nullptr;
    bool laid = false;
    PS::WaitFor("a sack laid on the floor ahead of the host", 60, [&] {
        OnGameThread([&] {
            void* me = coop::players::Registry::Get().Local();
            sack = me ? LaySack(me) : nullptr;
        });
        laid = sack != nullptr;
        return laid;
    });
    if (!laid) return;
    PS::WaitFor("the sack's element id", kEidSec, [&] {
        bool known = false;
        OnGameThread([&] { known = coop::element::Registry::Get().EidForActor(sack) != coop::element::kInvalidId; });
        return known;
    });
    Flags before, after;
    const char* why = "";
    bool ran = false;
    OnGameThread([&] {
        void* me = coop::players::Registry::Get().Local();
        before = ReadFlags(sack);
        ran = me && Press(me, sack, &why);
        after = ReadFlags(sack);
    });
    if (!ran) {
        RV::Say(kTag, RV::Outcome::Invalid, "the press did not run: %s", why);
        return;
    }
    UE_LOGI("prop-props: I1a flags before %d,%d,%d after %d,%d,%d (static,frozen,sleep)", before.isStatic, before.frozen,
            before.sleep, after.isStatic, after.frozen, after.sleep);
    PS::Mark("i1a_done");

    // I1b.
    if (!PS::Await("toolgun_in_hand", kSignalSec)) return;
    int held = 0;
    PS::WaitFor("the client's toolgun in hand here", kHandSec, [&] {
        OnGameThread([&] { held = coop::hand_item::HeldClassIs(1, kToolgunClass) ? 1 : 0; });
        return held == 1;
    }, false);
    UE_LOGI("prop-props: I1b HeldClassIs(slot 1, prop_toolgun_C)=%d", held);
    RV::Say(kTag, RV::Outcome::Invalid, "the premises are measured; the arms are not built yet");
    PS::Mark("host_done");
    UE_LOGI("prop-props: DONE on host");
}

void RunClient() {
    if (!disc_drill::HostPuppetUp()) return;
    PS::Mark("client_ready");
    if (!PS::Await("i1a_done", kSignalSec)) return;
    OnGameThread([] { ue_wrap::asset_load::LoadObjectByPath(kToolgunPath); });
    OnGameThread([] { coop::dev::drill_hand::TakeIntoHandNow(kToolgunItem, kToolgunClass); });
    const bool inHand = PS::WaitFor("the toolgun in this hand", kHandSec, [] {
        bool in = false;
        OnGameThread([&] {
            void* h = coop::hand_item::LocalHandActor();
            in = h && R::ClassNameOf(h) == kToolgunClass;
        });
        return in;
    });
    if (!inHand) return;
    UE_LOGI("prop-props: CLIENT the toolgun is in this hand");
    PS::Mark("toolgun_in_hand");
    PS::Await("host_done", 60);
    UE_LOGI("prop-props: DONE on client");
}

}  // namespace

DWORD WINAPI PropPropsThread(LPVOID /*arg*/) {
    if (!PS::Begin(kTag)) return 0;
    if (IsClientRole()) RunClient();
    else RunHost();
    return 0;
}

}  // namespace harness::autotest
