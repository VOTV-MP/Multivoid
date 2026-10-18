// coop/dev/drill_hand.cpp -- see coop/dev/drill_hand.h.

#include "coop/dev/drill_hand.h"

#include "coop/player/players_registry.h"

#include "ue_wrap/actors/inventory.h"
#include "ue_wrap/engine/engine_mainplayer.h"

namespace coop::dev::drill_hand {

bool TakeIntoHandNow(const wchar_t* item, const wchar_t* cls) {
    void* p = coop::players::Registry::Get().Local();
    return p && ue_wrap::inventory::WriteHeldItem(item, cls) && ue_wrap::engine::CallMainPlayerUpdateHold(p);
}

}  // namespace coop::dev::drill_hand
