// coop/props/mirror_gate.cpp -- see coop/props/mirror_gate.h.

#include "coop/props/mirror_gate.h"

#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"
#include "coop/world/spawn_authority.h"

#include <cstdint>

namespace coop::mirror_gate {
namespace {

// The copy shown in another slot's hand: hand_item spawns it and nothing else owns its numbers.
bool IsRemoteHandMirror(void* actor) {
    for (std::uint8_t slot = 0; slot < coop::players::kMaxPeers; ++slot)
        if (coop::hand_item::MirrorActorForSlot(slot) == actor) return true;
    return false;
}

}  // namespace

bool IsPeerCopy(void* actor) {
    if (coop::spawn_authority::IsActiveClientSession()) return !coop::hand_item::IsLocalHandViewActor(actor);
    return IsRemoteHandMirror(actor);
}

}  // namespace coop::mirror_gate
