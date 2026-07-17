// coop/dev/add_points.cpp -- see coop/dev/add_points.h.

#include "coop/dev/add_points.h"

#include "coop/world/balance_sync.h"
#include "coop/dev/dev_gate.h"
#include "ue_wrap/core/log.h"

namespace coop::dev::add_points {

void GivePoints(int amount) {
    // Strict client lockout: on a client this would send a BalanceDelta request
    // that credits the SHARED host-canonical balance -- a real economy cheat in
    // someone else's game (coop::dev_gate).
    if (!coop::dev_gate::Allowed()) {
        UE_LOGW("add_points: REFUSED -- dev features are disabled while connected as a client");
        return;
    }
    // Route through the shared-balance feature so the credit lands on the HOST's
    // canonical balance and both peers stay mirrored: on the host (or solo) it applies
    // locally via AddPoints (the host poll then broadcasts the new total); on a client
    // it sends a BalanceDelta request to the host. CreditRouted is render-thread safe.
    coop::balance_sync::CreditRouted(amount);
}

}  // namespace coop::dev::add_points
