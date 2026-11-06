# Deployables

## Purpose

The tools that leave a persistent actor behind (the grappling hook and rope, the garden hose, the nail
gun, the wall builder), timed explosives, the fishing rod and the physgun, each reverse-engineered with
a design. The hook, the rope and the garden hose are built; the rest are not. A prop a hook drags is the props page's
driven-prop channel ([props.md](props.md)).

## How it works

Their shared shape: the owner keeps its previews, montages and ammo local; a commit becomes an
intent to the host, either the spawn of a persistent actor the host replays through the native
entry point or an action on an existing host-owned prop; the host mirrors the actor down; a late
joiner gets it from the save; a player-attached phase (climbing a hook, a cast) is an extension of
the player's own stream.

Five rules govern a mirror of a deployable, and the first four are the ones a first attempt at
the hook lane did not have in front of it:

- **A deployed actor descends from the save-participating base, so a mirror of one is written
  into the save unless it says otherwise.** The game's save walk asks every object implementing
  the save interface whether to ignore it, and that answer is a plain field on the base class.
  Set it on every mirror, in the deferred window before the spawn finishes. This bites hardest on
  the HOST, which holds mirrors of hooks its clients fired and owns the only save in the session.
- **Transitions are polled, not caught at a spawn hook.** The player's fire path is
  Blueprint-internal, and the player already holds a pointer
  to their own deployed hook. One field read is the whole discovery channel, and it is more
  precise than a class scan: a variant of the hook class is placed by the level and attaches
  itself on every peer, so a scan that adopts by class doubles it.
- **A mirror's Blueprint tick is cancelled, not merely disabled.** The deployed hook's tick reads
  the LOCAL player, so one frame of it on the wrong machine drags the viewer toward somebody
  else's hook. The class registers its tick while it spawns, so a disable can only land after;
  the interceptor on the dispatched body needs no window at all.
- **The actor's own save record is the handover payload.** It already carries both attach keys,
  both component names and the cable length, and the game's own load path resolves them in
  whatever world it lands in. Nothing about an anchor needs a format of ours.
- **The physics constraint exists on the host and nowhere else.** The deployed hook's tie is a
  PhysX constraint the game builds on whichever machine runs its attach verbs, and a constraint
  pulling a host-owned prop on a client is that client moving shared state the host never sees.
  So a client breaks every tie a hook builds on it, in the frame that built it (a post watch on `makeAttachments`, `[V]` on the rig: `hookdrag` on `72742a09`, every role),
  and the host builds a client's hook its real tie on the mirror it holds of that hook, against
  that client's puppet, from the bite the owner's state names. The client's own hook keeps its
  pull on the client's own player, which is a velocity write in the tick and not the constraint;
  its head still rides what it bit. Two players hooking one prop is then two host constraints on
  one host body, which is what single-player physics would do (`coop/items/hook_constraint`).

A garden hose (`coop/world/ground_hose_sync`) is laid on one peer: the coil used on a faucet spawns a
`groundHose_C` there, and until the coil finishes it the hose's tick lays a point wherever that peer's own
player walks. So only a finished hose crosses, as its own save record: its points and the keys of its
faucet and sprinkler. The host sends each of its finished hoses when it is new or changed and its key once
when it goes; a client lays the record on its copy, makes the copy when it has none, and ties it to its own
faucet and sprinkler of those keys, so the faucet's water, which the appliance lane already shares, runs
its sprinkler there too. A hose a client lays, re-lays, cuts from its sprinkler or takes up goes to the host
as a route or a removal; the host checks it (a faucet of its world, the points in the world and no more
than 512, the sender by the hose's end or, for a removal, within 8 m of it) and lays or takes up its own,
and its state answers. A client that changed a hose drops a host state older than its change until the
answer comes. The hose's `getData` keeps the key of a sprinkler cut since, so the lane's record names an
end that is not tied now as `None` `[V]` (`harness/autotest/autotest_ground_hose.cpp`, twice: a hose
from each peer laid by its record, the host's faucet running the sprinkler on both, the client's cut and
its taking up of the host's hose, each checked on the other).

## Who owns what

| State | Owner | Shape |
|---|---|---|
| a deployed hook, while its thrower still holds it | that peer | keyed by (slot, sequence); the others render a parked mirror |
| a deployed hook once both ends are anchored | the host | the thrower hands over the hook's own save record; it is a save actor from then on |
| a hook's tie, the physics constraint | the host | a client breaks every tie a hook builds on it; the host builds a client's tie on its mirror of that hook |
| a garden hose being laid | the peer whose player lays it | not sent until it is finished |
| a finished garden hose, its points and its two ends | the host | the hose's own save record; a client's change is a route the host checks |

## Wire messages

| Kind | Direction | Carries |
|---|---|---|
| `HookState` | the owner to all, relayed | a hook that still belongs to the player who fired it, keyed by (slot, sequence); create-or-update, re-sent on a slow keepalive |
| `HookDestroy` | the owner to all, or the host for a leaver | that hook is gone |
| `HookAnchorCommit` | a client to the host | the anchored hook's own save record, chunked |
| `HookAnchored` | the host to all | an anchored hook exists, as the same record |
| `GroundHoseState` | the host to all | a finished hose's own record, or its key when it went; chunked |
| `GroundHoseIntent` | a client to the host | a hose its player finished or changed, as its record, or took up; chunked |

## Late join

A hook its thrower still owns reaches a joiner on its keepalive. An anchored hook is a save actor
of the host's and arrives with the save, and the game's own load re-resolves both of its attach
keys in the joiner's world. A garden hose arrives with the save and again from the host at the ready edge,
which re-ties it in the joiner's world and carries what changed after the save was taken. The deployables that are
not built reach a joiner only through the save.

## Known limits

| Limit | Evidence |
|---|---|
| The deployables other than the hook, the rope and the garden hose (nail gun, wall builder, explosives, fishing rod, physgun) are not synced; a nail or a wall placed by one peer reaches the others only through the save at their next join | `[V]` no lane under `coop/props` catches them |
| A garden hose reaches the others only once it is finished: while one player lays it, the others see the faucet without it. A sprinkler a hose ties is made static by the coil on the laying peer only | `[V]` `coop/world/ground_hose_sync` sends no laying hose; `prop_sprinkler_C`'s `connectHose` calls `setPropProps`, which the lane does not replay |
| The drill lays hoses by their records and runs the cut by the hose's own `detachSprinkler`; a hose walked out with a coil, and a cut by grabbing the sprinkler, are not driven on the rig | `[?]` `harness/autotest/autotest_ground_hose.cpp` |
| A client's hook into the ATV ties the host's ATV against the client's puppet; whether the ATV lane's corrector carries that pull back to the client's copy is not measured | `[?]` `coop/items/hook_constraint` ties any keyed actor the bite resolves; `coop/interactables/atv_sync` corrects rather than parks |

## Code map

| Concept | Files |
|---|---|
| the owner's stream, the mirrors and the brain park | `coop/items/hook_sync` |
| the anchor handoff | `coop/items/hook_anchor` |
| the tie, host-only | `coop/items/hook_constraint` |
| the props a tie claims for the driven-prop channel | `coop/items/hook_prop_claim` |
| the garden hose lane | `coop/world/ground_hose_sync`; the hose, its faucet and sprinkler `ue_wrap/actors/ground_hose` |
