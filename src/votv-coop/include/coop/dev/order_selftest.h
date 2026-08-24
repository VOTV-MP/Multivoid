// coop/dev/order_selftest.h -- DEV-ONLY. Make a CLIENT place a real laptop shop order, so the
// host-authoritative pricing path (security A34/A35, proto v136) can be exercised end to end by an
// autonomous smoke instead of by asking a human to go shopping.
//
// WHY A NEW INSTRUMENT, when `drone_probe_drive=1` already auto-places a client order: because that
// one cannot exercise this path. `[V]` `daynightCycle::"Make Default Order"` -- the builder it uses
// -- writes `name = NAME_None` into every item it produces (@2723, @2364). From v136 the row NAME is
// the shop identity and the only thing that travels, so an order built that way carries nothing to
// forward. An idle smoke plus a nameless order would have proved exactly what the ReliableKind
// checklist warns about: install markers green, the wire dead.
//
// WHAT IT DOES, on a CLIENT, once, after a settle delay: reproduces `ui_laptop`'s Button_order
// without the UI. It debits the local balance by the summed price (`[V]` @6122 Multiply(storePrice,
// -1) -> @6168 lib_C::addPoints -- and that debit is the thing the whole feature has to correct,
// so a drill that skipped it would not be testing the interesting half), then calls the native
// makeAnOrder with rows resolved from the peer's own `list_store`. `order_sync`'s watermark poll
// then sees orders.Num rise and forwards it exactly as it would a human's purchase.
//
// THE ROWS ARE CHOSEN, not arbitrary: `drive` (price 2), `cup` (1) and `burger` (90) -- 93 credits
// total, cheap enough not to disturb a save, and `cup` is one of the 101 rows whose `object` is the
// generic `prop_C` and whose real identity lives in `asProp`. So a GREEN run also exercises the
// wholesale-row copy that fixes the ~141 rows which used to mis-deliver (security A45).
//
// WHAT A RUN PROVES, and how to read it:
//   GREEN  -- host log: `order_sync: committed slot N order id=M (3 items) and charged 93 from the
//             shared balance (X -> X-93)`. That single line is the whole feature: the host priced it
//             from its own table, checked its own balance, confirmed the orders.Num +1 edge, and
//             charged. Client log: the balance mirror converging on X-93.
//   RED    -- run the same thing with `VOTVCOOP_STORE_CATALOG_BREAK=1` in the HOST's environment.
//             The catalog gate must reject the corrupted read, and the host must log
//             `REFUSING order ... the host's store catalog is unusable` with NO charge, while the
//             client logs `order id=M REFUSED by the host` and restores its cart. That is the drill
//             for the fail-closed branch, which otherwise can never fire in a healthy build
//             ([[lesson-an-instrument-blind-to-the-phenomenon-always-passes]]).
//
// ini-gated OFF (`[dev] order_selftest=1`); never ships enabled. It DOES mutate local state (a
// balance debit + an order), which is why it is a knob and not a passive readout -- but every effect
// is either what a human purchase does or what the feature under test is supposed to correct.
// (RULE 2 exempts probes/diagnostics/tools.)

#pragma once

namespace coop::dev::order_selftest {

// Fire the one-shot if enabled, on a CLIENT, once the session and world have settled. Game thread.
void Tick(bool connected, bool isHost);

}  // namespace coop::dev::order_selftest
