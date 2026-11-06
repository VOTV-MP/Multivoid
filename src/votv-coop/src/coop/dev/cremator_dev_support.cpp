// coop/dev/cremator_dev_support.cpp -- see cremator_dev_support.h.

#include "cremator_dev_support.h"  // co-located private header (src tree, not include/)

#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/props/prop_snapshot.h"
#include "coop/save/join_window_baseline.h"

#include "ue_wrap/core/types.h"

namespace coop::dev::cremator_support {

bool SlotSettled(coop::net::Session& session, uint8_t slot) {
    coop::RemotePlayer* p = coop::players::Registry::Get().Puppet(slot);
    ue_wrap::FVector at{};
    return p && p->valid() && p->TryGetLocation(at) && session.IsSlotWorldReady(slot) &&
           coop::prop_snapshot::IsBracketClosed(slot) && !coop::join_window_baseline::IsLateWindowOpen(slot) &&
           session.SlotReliableIdle(slot);
}

}  // namespace coop::dev::cremator_support
