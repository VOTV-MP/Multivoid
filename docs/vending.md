# Vending machines

## Purpose

The snack and soda vending machines (`prop_vending_C`, keyed `snacksVendingMachine`): their stock, the coins put in, the buy,
and the refill, so every player sees the one machine's state and a buy by a client takes from the host's stock. The coffee
machines have their own intent and are on [devices.md](devices.md); props in general are on [props.md](props.md).

## How it works

A machine holds seven slots (`retarget_items`, `retarget_prices`, `retarget_amounts`), a `balance` (the coins put in) and `isEmpty`.
Its use (`actionOptionIndex`: 4 buys a slot, 7 takes the change) and a coin put in (`playerUsedOn`) run on the peer that used it,
and the item a buy mints is a prop that peer alone would hold (a client authors no prop spawn). The refill (`beginRestock`, started
by `checkStock` and a timer) waits until the machine's figure is out of the line of sight of the machine's own peer's camera. Each
peer's machine was therefore its own.

The machine is the host's (`coop/world/vending_sync`):

- **State, host to all.** Once a second the host compares each machine with what it last sent and, on a change, sends one
  `AmbientFixtureState` per slot with the family `kVendStock` (`id` a hash of the interactable key, `x` the slot's amount, `y` its
  price, `z` the balance, `pitch` the slot count, `yaw` isEmpty, pad byte 0 the slot, `birthKey` the item's name). The client stages
  the round and, once every slot is in, writes the amounts, prices, balance and isEmpty into its copy and runs `gennameboxes`.
- **A buy or the change, client to host.** The client's `actionOptionIndex` is refused on its copy and sent as `PropIgniteIntent`
  verb 10 (`key` the machine, pad byte 0 the action, pad byte 1 the slot). The host's `TakeIntent` runs the same call with its own
  local player, so the item is a host prop and reaches the client as one.
- **A coin, client to host.** The client's `playerUsedOn` is refused and sent as verb 11 (`key`, `elementId` the coin's id, read from
  the prop element tracker for a local prop and from the mirror reverse for a host-made one). The host puts that coin in.
- **The refill.** `beginRestock`, `checkStock` and `timerCheckStock` are refused on a client (rows of the watch table, with their
  own fingerprints). The host's refill runs by its own line-of-sight wait, which reads the host's camera 0 alone; the only thing a
  player sees of it is the figure's sound (`audio_figura`), which the client's copy plays on the edge of `isEmpty` going from false
  to true.

## Who owns what

| State | Owner | Shape |
|---|---|---|
| a machine's amounts, prices, balance, isEmpty | the host | polled 1 Hz, sent per slot; the client writes them |
| a buy, the change, a coin | the host | the client's call refused and sent as `PropIgniteIntent` verbs 10 and 11 |
| the refill | the host | refused on the client; the figure's sound replayed there |
| the item a buy mints | the host | a host prop, sent by the prop lane |

## Wire messages

| Kind | Direction | Carries |
|---|---|---|
| `AmbientFixtureState` (family `kVendStock`) | the host to all | one slot's amount and price, the balance, the slot count and isEmpty |
| `PropIgniteIntent` (verbs `kVerbVendUse` 10, `kVerbVendCoin` 11) | a client to the host | a buy or change (key, action, slot); a coin (key, element id) |

## Late join

The joiner is sent every machine's seven slots at the connect (`QueueConnectBroadcastForSlot`), so its copy takes the host's stock
before it can be used.

## Known limits

| Limit | Evidence |
|---|---|
| The refill's wait for the figure to be out of view reads the host's camera, so a client may see the figure while it restocks; the sound is the only presentation replayed | `[RD]` `beginRestock` reads camera 0; `[V]` the figure's sound played on the client when `isEmpty` rose |
| A coin that is no prop of the shared set (a client's own local prop, with no element id) is put in on the client's copy: it runs there, and the copy's balance differs from the host's until the host's machine next changes | `[V]` in the coin drill's first run the client's balance read 995 and the host's 990; the guard is `vending_sync::OnCoinPre` |
| At most 32 slots are read per machine (`kMaxSlots`); the game's machine holds 7 | `[RD]` `prop_vending_C` |

What was measured, by the `vendBase` and `vendLane` drills on two peers (`coop/dev/vending_drill`) `[V]`. Without the lane, the client bought slot 0
locally (amounts 19 and balance 990 on the client, the host's untouched at 20 and 0) and minted its own chips item. With it, the host wrote balance
1000; the client's buy was refused and run by the host (balance 1000 to 990) and the copy followed (amounts[0] 19, balance 990); the host's chips
item reached the client; the client's coin was refused and put in by the host (990 to 995) and the copy followed; the host emptied the machine and
`checkStock` restocked it at once (amounts[0] 20, isEmpty), and the client followed and played the figure's sound. No script error or fault was
logged on either peer.

## Code map

| Concept | Files |
|---|---|
| the machine's state, the intents, the refusals | `coop/world/vending_sync`; the routing in `coop/dispatch/event_dispatch_prop_drive`, `coop/dispatch/event_dispatch_intent`; the ticks in `coop/world/event_lanes`; the joiner's resend in `coop/session/connect_replay` |
| the drills | `coop/dev/vending_drill` (`vendBase`, `vendLane`) |
