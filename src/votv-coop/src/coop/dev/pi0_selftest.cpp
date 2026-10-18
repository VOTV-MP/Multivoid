// coop/dev/pi0_selftest.cpp -- [dev] a cleared Player Element keeps its id. See the header.
#include "coop/dev/pi0_selftest.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/element/player.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "ue_wrap/core/log.h"

#include <cstdint>

namespace coop::dev::pi0_selftest {
namespace {

namespace PL = coop::players;

constexpr uint8_t kSpareSlot = PL::kMaxPeers - 1;

void Run() {
    PL::Registry& reg = PL::Registry::Get();
    if (reg.LocalPeerId() == kSpareSlot || reg.Puppet(kSpareSlot) || reg.GetPlayerElement(kSpareSlot)) {
        UE_LOGI("[PI-0-SELFTEST] ABANDONED slot in use");
        return;
    }
    // Never spawned: GetActor() is null, which is all Registry::IsPuppet reads of it.
    static coop::RemotePlayer s_puppet;

    reg.RegisterPuppet(kSpareSlot, &s_puppet);
    const coop::element::Player* first = reg.GetPlayerElement(kSpareSlot);
    if (!first) {
        UE_LOGI("[PI-0-SELFTEST] FAIL no Player Element after RegisterPuppet");
        reg.UnregisterPuppet(kSpareSlot);
        return;
    }
    const coop::element::ElementId oldId = first->GetId();
    reg.ClearPuppet(kSpareSlot);
    if (reg.Puppet(kSpareSlot) || first->Puppet()) {
        UE_LOGI("[PI-0-SELFTEST] FAIL clear left the puppet bound");
        reg.UnregisterPuppet(kSpareSlot);
        return;
    }
    reg.RegisterPuppet(kSpareSlot, &s_puppet);
    const coop::element::Player* second = reg.GetPlayerElement(kSpareSlot);
    const coop::element::ElementId newId = second ? second->GetId() : coop::element::kInvalidId;
    if (second && newId == oldId && second->Puppet() == &s_puppet)
        UE_LOGI("[PI-0-SELFTEST] PASS cleared element kept id=%u", static_cast<unsigned>(oldId));
    else
        UE_LOGI("[PI-0-SELFTEST] FAIL cleared element re-minted old=%u new=%u", static_cast<unsigned>(oldId),
                static_cast<unsigned>(newId));
    reg.UnregisterPuppet(kSpareSlot);
}

}  // namespace

void Tick(coop::net::Session* session) {
    static const bool s_on = coop::config::ResolveFlag(::coop::config_registry::rows::pi0_selftest);
    if (!s_on || !session || session->role() != coop::net::Role::Host) return;
    static bool s_done = false;
    if (s_done) return;
    PL::Registry& reg = PL::Registry::Get();
    if (reg.LocalPeerId() != PL::kPeerIdHost || !reg.GetPlayerElement(PL::kPeerIdHost)) return;
    s_done = true;
    Run();
}

}  // namespace coop::dev::pi0_selftest
