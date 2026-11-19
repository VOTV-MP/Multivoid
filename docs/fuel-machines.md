# Fuel machines

## Purpose

The machines a gas can fuels and a button starts, each with its fuel and its on/off in its own record and a tick that
burns the fuel, so each ran on every copy for itself; the host's copy is now the machine, and a client's pour and press
are asked of it. The woodchipper is in [devices.md](devices.md); the base's own generator (`generator_C`, no fuel) is the
power grid's, there too.

## The portable generator

`prop_generatorP_C` keeps its fuel and `isActive` in its record; a can fills it (`gascanFuel`, or one lying in its fuel box),
action 8 runs `tryActivate` and `activateOutlets` powers its two sockets, and its tick burns fuel. Each copy ran its own, so a
client's pour or start never reached the host (Trello #50) `[V]`. Now (`coop/interactables/generator_sync`) a client's copy is
refused its tick and the fuel box; its pour and press run on its copy and go to the host (`GeneratorIntent`), which checks the
key and class, a living sender in reach and, for a pour, the can in its hand, then adds the fuel or runs `tryActivate`. The
host reads its generators twice a second and sends one that changed, a running one once a second, a refused intent's state
to its sender and every one to a joiner (`GeneratorState`); a client writes it, with `activateOutlets` on an on/off change.
`[V]` `coop/dev/genp_probe` (`VOTVCOOP_GENP_PROBE=client`): the client's pour of 50 and its start ran on the host, whose burn the
client's copy then followed; before it the host's copy stayed empty and off.

A gas can is any class of the can's line (`prop_gascan_C` and its big, full, mini and funny cans, which the game's own
fuelling casts take), in the hotbar hand or carried by the grab, as a big can is: the generator's, the woodchipper's and
the ATV's judges ask both (`hand_item::HeldClassIsA`, `remote_prop::CarriesClassA`) `[V]` (`VOTVCOOP_GENP_GRAB=
prop_gascan_big_full_C`: a client's pour from a carried big can was refused as "no gas can in the hand"; now the host
added its 100 and the generator ran on both).
