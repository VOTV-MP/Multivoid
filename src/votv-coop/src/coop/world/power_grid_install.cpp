// coop/world/power_grid_install.cpp -- see coop/world/power_grid_install.h.

#include "coop/world/power_grid_install.h"

#include "coop/player/hand_item.h"
#include "coop/player/hand_spend.h"
#include "coop/player/players_registry.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/devices/generator.h"

namespace coop::power_grid::install {
namespace {

namespace GEN = ue_wrap::generator;
namespace R   = ue_wrap::reflection;

// The class a generator's insert takes, as the hand lane announces a hand item of it (hand_item compares without case).
constexpr const wchar_t* kUpgradeClass = L"prop_transformerUpgrade_C";

}  // namespace

bool PaidByHand(uint8_t sender, const coop::net::PowerGridPayload& p) {
    if (p.op != coop::net::kPowerGridOpUpgrade) return false;
    coop::hand_item::ApplyOwedNow(sender);
    if (!coop::hand_spend::Spend(sender, kUpgradeClass, 1)) return false;
    UE_LOGI("power_grid: slot %u's install at generator %u is paid by the upgrade its hand held", sender, p.index);
    return true;
}

void AnnounceHeldUpgrade(coop::net::Session& session, void* function, const uint8_t* locals) {
    static void* s_fn = nullptr;
    static int32_t s_off = -1;
    if (function != s_fn) {
        s_fn = function;
        s_off = R::FindParamOffset(function, L"holdObject");
    }
    if (s_off < 0 || !locals) return;
    void* held = *reinterpret_cast<void* const*>(locals + s_off);
    if (held && GEN::IsUpgrade(held) && coop::hand_item::IsLocalHandViewActor(held))
        coop::hand_item::AnnounceHeldNow(session, coop::players::Registry::Get().Local(), held);
}

}  // namespace coop::power_grid::install
