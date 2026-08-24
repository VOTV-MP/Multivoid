// coop/dev/add_points.cpp -- see coop/dev/add_points.h.

#include "coop/dev/add_points.h"

#include "coop/world/balance_sync.h"
#include "coop/dev/dev_gate.h"
#include "ue_wrap/core/log.h"

namespace coop::dev::add_points {

void GivePoints(int amount) {
    // Strict client lockout: crediting the SHARED host-canonical balance from a client
    // would be a real economy cheat in someone else's game (coop::dev_gate).
    if (!coop::dev_gate::Allowed()) {
        UE_LOGW("add_points: REFUSED -- dev features are disabled while connected as a client");
        return;
    }
    // Apply through the shared-balance feature so the credit lands on the HOST's canonical
    // balance and both peers stay mirrored: on the host (or solo) it applies locally via
    // AddPoints and the host poll then broadcasts the new total. A connected client is
    // refused there too -- since v135 there is no client->host economy write at all
    // (security A5). CreditLocal is render-thread safe.
    coop::balance_sync::CreditLocal(amount);
}

}  // namespace coop::dev::add_points
