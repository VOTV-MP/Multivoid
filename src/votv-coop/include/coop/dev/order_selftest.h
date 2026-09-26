// coop/dev/order_selftest.h -- DEV-ONLY. The order lanes end to end, driven by an autonomous smoke
// instead of by asking a human to go shopping. Two legs, each once a session:
//   CLIENT -- once its world is up and its join over, it reproduces the laptop's order button without
//             the UI: a real order through the game's own shop, rows resolved from the peer's own
//             `list_store`, then the local debit by the price that order came back with -- the debit
//             the feature has to correct, so a drill skipping it tests only the easy half. order_sync's
//             gate on makeAnOrder forwards it as it would a human's purchase.
//   HOST   -- once a client's world is ready (a tick after, so its queue's reset went first), it makes
//             the day cycle's own daily order, "Make Default Order" then makeAnOrder with automatic
//             set, as the game does at six: a world event's order, whose items carry no row. The
//             queue mirror has to carry them by class; the client's DONE line says whether it did.
//
// The client's rows are chosen: `drive`, `cup` and `burger`, cheap enough not to disturb a save; `cup`
// is one of the rows whose `object` is the generic `prop_C`, its identity in `asProp`, so a green run
// exercises the wholesale-row copy those rows need.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::order_selftest {

// Reading a run:
//   GREEN -- the host logs one line committing the client's order and charging it against the shared
//            balance (priced from its own table, its own balance checked, the orders.Num edge
//            confirmed), the client's balance mirror converges on the same figure, and the client's
//            "[order_selftest] client DONE ... PASS" says the host's daily order reached its queue by
//            class with every item built. Read the charge off the client's own line: the price comes
//            from the live shop.
//   RED   -- run it again with VOTVCOOP_STORE_CATALOG_BREAK=1 in the HOST's environment: the catalog
//            gate must reject the corrupted read, the host refusing the order and charging nothing, the
//            client logging the refusal and restoring its cart -- the fail-closed branch, which a
//            healthy build never fires on its own.

// ini-gated OFF (`[dev] order_selftest=1`); never ships enabled. It DOES mutate state -- a balance
// debit and two orders -- but every effect is what a human purchase or the day cycle does, or what the
// feature under test is supposed to correct.

// The legs, once enabled; a single bool read when off. Game thread.
void Tick(coop::net::Session* session);

// The per-session half: a rejoin's peers run their legs again.
void OnDisconnect();

}  // namespace coop::dev::order_selftest
