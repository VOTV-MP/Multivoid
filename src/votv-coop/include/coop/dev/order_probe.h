// coop/dev/order_probe.h -- [dev] the delivery-order census: who writes the save's
// order queue, on which peer, and what the drone delivers, read at the bodies through the script gate.
// Observation only. Each peer logs, tagged [ORDER-PROBE]:
//   ui_laptop makeAnOrder at entry: automatic, the order's item count, the queue's count, and the calling
//     Blueprint function (the laptop's order button, daynightCycle's func_newHour, trigger_eventer, or
//     none for a call of ours);
//   ui_laptop addOrderCart and removeOrderCart at exit: the queue's count;
//   the drone's sendShop (a no-op while the drone is active), checkOrders and compileOrder (a delivery)
//     at entry: the drone's active flag, the item count of the order it carries, the queue's count;
//   daynightCycle func_newHour at entry: the time it was given (the daily order runs from hour 6).
// A client's order for the run: order_selftest=1. Run with order_probe=1.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::order_probe {

bool IsEnabled();

// Keeps the session for the log's side and registers the watches once (the gate resolves their names on
// the game thread). A single bool read when off. Game thread.
void Install(coop::net::Session* session);

}  // namespace coop::dev::order_probe
