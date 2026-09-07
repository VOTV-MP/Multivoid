// coop/dev/order_selftest.h -- DEV-ONLY. Make a CLIENT place a real laptop shop order, so the
// host-authoritative pricing path is exercised end to end by an autonomous smoke instead of by
// asking a human to go shopping. The other auto-order knob cannot reach it: the builder that one
// uses writes NAME_None into every item it produces, and the row NAME is the shop identity and
// the only thing that travels, so an order built that way carries nothing to forward.
//
// On a CLIENT, once, after a settle delay, it reproduces the laptop's order button without the
// UI: place a real order through the game's own shop, rows resolved from the peer's own
// `list_store`, then debit the local balance by the price that order came back with -- the debit
// the whole feature has to correct, so a drill skipping it tests only the easy half. order_sync's
// watermark poll then sees orders.Num rise and forwards it as it would a human's purchase.
//
// The rows are chosen: `drive`, `cup` and `burger`, cheap enough not to disturb a save; `cup` is
// one of the rows whose `object` is the generic `prop_C`, its identity in `asProp`, so a green
// run exercises the wholesale-row copy those rows need.

#pragma once

namespace coop::dev::order_selftest {

// Reading a run:
//   GREEN -- the host logs one line committing the order and charging it against the shared
//            balance: it priced the order from its own table, checked its own balance,
//            confirmed the orders.Num edge, and charged, and the client's balance mirror
//            converges on the same figure. Read the charge off the client's own log line
//            rather than expecting a fixed number -- the price comes from the live shop.
//   RED   -- run it again with VOTVCOOP_STORE_CATALOG_BREAK=1 in the HOST's environment. The
//            catalog gate must reject the corrupted read: the host refuses the order and charges
//            nothing, the client logs the refusal and restores its cart. That is the drill for the
//            fail-closed branch, which a healthy build can never fire on its own.

// ini-gated OFF (`[dev] order_selftest=1`); never ships enabled. It DOES mutate local state -- a
// balance debit and an order -- which is why it is a knob and not a passive readout; but every
// effect is either what a human purchase does or what the feature under test is supposed to
// correct. (RULE 2 exempts probes and diagnostics.)

// Fire the one-shot if enabled, on a CLIENT, once the session and world have settled. Game thread.
void Tick(bool connected, bool isHost);

}  // namespace coop::dev::order_selftest
