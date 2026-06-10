// coop/dev/drone_probe.h -- dev-only RE probe for the delivery drone (Adrone_C).
//
// Gated on ini `drone_probe=1`. Resolves the unknowns the static RE couldn't (BP
// bytecode isn't in the EXE -- see research/findings/votv-delivery-drone-RE-and-
// coop-sync-design-2026-06-03.md, the 10 [PROBE]s). Answers, from ONE real
// delivery (call the drone console -> watch it fly in -> drop -> leave):
//   [#1] is mainGamemode.drone a persistent placed singleton? (presence + ptr stability)
//   [#2] is AdroneConsole::player_use ProcessEvent-dispatched (hookable)? (observer fires)
//   [#3] does dropSack's box fire the Aprop_C Init observer? (orderbox/giftbox Init fires)
//   [#4] flyingType int -> {delivery,pickup,sell} value (state dump during the cycle)
//   [#5] which of triggerFly/beginFly/dropSack/soundAlarm/checkOrders/compileOrder are
//        ProcessEvent vs BP-internal? (observer fires vs silence)
//   [#6] order-commit path (saveSlot.orders.Num / droneOrder.items.Num dump)
//   [#8] auto-drive schedule (daynight sendDriveBox / Make Default Order observer fires)
//   [#9] gift injector on good task (createNewTask / Make Default Order fires)
//   [#10] does the mirror drone register in mainGamemode.radarObjects? (membership check)
// A CLIENT-side run additionally answers [#7] (is the laptop shop reachable) + [#10].
//
// NONE of this ships (RULE 3 -- dev probe). All reads are reflection (FindPropertyOffset
// by field name -> robust to offset drift) + ProcessEvent observers. Game thread only.

#pragma once

namespace coop::dev::drone_probe {

// Install the ProcessEvent observers (once, ini-gated, retried until the drone BP
// class loads). Safe to call every net-pump tick -- self-latches.
void Install();

// Per-tick state poll: mainGamemode.drone presence + Active/hasOrder/hasSack/flyingType/
// pickedUp transitions + saveSlot order/economy dump (throttled) + radarObjects membership.
// No-op unless `drone_probe=1`. Game thread only.
//
// `connected`/`isHost` come from the session (net_pump). When ALSO `drone_probe_drive=1`, the
// probe DRIVES the game's own delivery path once, after a settle delay, so it answers its
// questions autonomously (no hands-on): HOST flies a default delivery (Make Default Order ->
// laptop.makeAnOrder(order,true)); CLIENT commits a shop order (laptop.addOrderCart). RULE 1 --
// run the probe ourselves, don't offload the hands-on.
void Tick(bool connected, bool isHost);

}  // namespace coop::dev::drone_probe
