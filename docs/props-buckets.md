# Buckets and cement bags

A world bucket's water, a concrete bucket's concrete and a cement bag's units are the host's on every peer, and a client's verbs
on them are asked of the host. The identity, birth and drop of the props themselves are on [props.md](props.md).

## What the game does

A water bucket (`prop_bucket_C`, and the base's mop bucket `prop_bucket_mop_C`, a bucket with nothing of its own) holds
`height` of water up to `maxHeight`, with `soap` and a brewing `alcoholP` that turns `alcohol` on at 1. Its tick eases
`height_interp` toward `height` and redraws the water; tipped, it pours; under water, it fills. A held soap adds its amount
to `soap`, a sponge dipped takes 2 cm of water or one soap, a clean jar takes a part of a brewed bucket's water. A cement bag
on a bucket with water makes it (`lib.replaceProp`) a concrete bucket (`prop_concreteBucket_C`) of
round(`height` / `maxHeight` * 20) `units`; a trowel takes one unit, the last turns the bucket back into a water bucket, and 600 s
dry it. A bag gives or takes one unit of another bag. Before the lane, each peer ran all of it on its own copies.

## Who owns what

- The host owns a world water bucket's `height`, `soap`, `alcohol`, `alcoholP` and `alcReady`, a concrete bucket's `units`,
  `dry` and `dryTimer`, and a world bag's `units`. A held item is its holder's own and reaches the host in its drop's record.
- A client's water bucket copy runs its own tick, for the water's look. At the tick's entry the host's last row is written
  into it (`coop/props/bucket_pin`), so what the copy's own code changed since lasts at most one tick and the water drawn is
  the host's. A client's concrete bucket copy never runs its tick, and no peer's copy runs `deleteAlc`, which destroys the bread
  and sugar of a brew: the host's own brew destroys them and the destroy lane relays it.
- A copy is a peer's by one rule (`coop/props/mirror_gate`): on a client every world copy but the one in the local hand, on
  the host the copy shown in a client's hand.

## The rows

`BucketState` carries up to ten rows of 20 bytes: the element id, the type (plain, concrete, bag), flags (`alcohol`,
`alcReady`, `dry`), a count (soap or units), a level (the water), a progress (the brew or the drying clock) and a per-element
`seq`. The host reads an element after its own tick (a water or concrete bucket), after a bag's transfer, and when it applies a
family; it sends a row when a field moved past a row's worth (bools, counts and the water exactly, the brew by 0.01, the
drying clock by 5 s), at most ten rows a second for one element, carrying the latest values. A row for a copy not yet bound waits
for its bind and is applied at the bind, unless a newer row has been applied since; it and every other per-element record
end with the element, at the engine's announcement of its end `[?]`. A joiner is sent every element at its world-ready, each row
numbered with the element's next `seq` like any other, bar one whose drop record is still in flight. A client keeps a row only when every number is finite and in its class's range, and
refuses a message whose length, row count or padding is wrong.

## A client's verbs

Six families of `PropActRequest` (`coop/props/prop_act_bucket*`; the registry is `coop/props/prop_act_families`):

| Byte | Verb | The host's judge | The host's apply at the commit |
|---|---|---|---|
| 6 | a held soap on a bucket without alcohol | the held class, the amount 0 to 10 | `soap` plus the amount, the soap's use spent |
| 7 | a clean jar on a brewed bucket | the held jar, alcohol and more than a third of the water | the water down by `maxHeight` / 2.4, the jar's use spent |
| 8 | a held sponge dipped | water above 10 cm, the branch (soap or water) equals the host's | one soap, or 2 cm of water |
| 9 | a held cement bag on a bucket | `canConcrete`, the units the water makes equal the client's | a concrete bucket of that many units replaces the bucket |
| 10 | a held trowel in a concrete bucket | the concrete is not dry | `takeConcrete` on the host's copy |
| 11 | a unit between the held bag and a world bag | the world bag can give or take | the world bag's units, one more or less |

A client's watch lets the game refuse where the Blueprint would refuse or do nothing; otherwise it announces the hold, asks and
cancels. A grant reserves the target for the sender until its commit; the client replays its half (the game's own verb, or for
9, 10 and 11 a spend of the held item) and commits 0 or 1. At 1 the host reads the judge's condition again and writes the
recorded value; when its own player or tick changed the target in the round trip the apply is counted, not written, and the
asker's copy gets the host's row, sent to the asker alone. At 0 the asker's copy gets the host's row. A verb the client cannot
form from what it reads (the copy, the hold, the amount) is cancelled and said once, never run unasked on the shared copy. The
host keeps the held cement bag's units from the take (the destroy of the world copy it was taken from, claimed by the hold that
follows it); the mix and transfer judges refuse what the count cannot pay, and a new hold or the peer leaving empties the
count `[?]`. A trowel's take is the game's own rule. The host's own player's verb on a target a client has reserved is
cancelled for that round trip.

## A dropped item

A bucket, bag or trowel a client drops reaches the host in the drop's save record. A class gate refuses a record the sender's own
drop does not await, and one with a number past its class's range (the water above `maxHeight`, soap above 10000, concrete
above 20 units or above the units the bucket had when its sender took it, a bag above its cap, a trowel above 5). A refusal
is final for the record: the element's rows are not held back, and the host's row of the element goes to the sender, when the
host's copy has an element id `[?]`.

## Open

- The pour props a dip or a tip births (`prop_sponge_bucketpour_C`) come from every peer's own tick, the client's matched to
  the host's births `[?]`.
- A soap's and a sponge's own amounts are not in their drop records: the host's copies read the class defaults `[?]`.
- A watered plant from a held bucket and a trowel's wall repair belong to other lanes.
- Every behaviour above is `[?]` until the lane's drill has run on the rig.
