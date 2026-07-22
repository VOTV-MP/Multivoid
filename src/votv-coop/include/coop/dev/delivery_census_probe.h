// coop/dev/delivery_census_probe.h -- see the .cpp. Diagnostic only; ini-gated OFF by default
// ([dev] delivery_census=1). Settles gate O-1 of
// research/findings/inventory-items/votv-order-delivery-pipeline-RE-2026-07-22.md by COUNTING
// the delivery-path actors (FindObjectsByClass) and comparing POINTERS -- never by looking a
// container up by key, since the broken key lookup is the thing under investigation.
//
// Run it across TWO CONSECUTIVE deliveries: whether the receiving container's actor ptr / eid /
// propInventory.Index change between them is the churn answer, and that decides whether stable
// element-key addressing is applicable to the R11 lane at all.

#pragma once

#include <cstdint>

namespace coop::dev::delivery_census {

// Game thread. Self-gated on the ini key and on its own 0.5 Hz sample interval; logs only when
// the census CHANGES (plus the first sample).
void Tick(bool isHost);

}  // namespace coop::dev::delivery_census
