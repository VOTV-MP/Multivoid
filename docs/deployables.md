# Deployables

## Purpose

The tools that leave a persistent actor behind (the grappling hook and rope, the nail gun, the wall
builder), timed explosives, the fishing rod and the physgun, each reverse-engineered with a design.
The hook and the rope are built; the rest are not. A prop a hook drags is the props page's
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
  So a client breaks every tie a hook builds on it, at the native seam the build funnels through,
  and the host builds a client's hook its real tie on the mirror it holds of that hook, against
  that client's puppet, from the bite the owner's state names. The client's own hook keeps its
  pull on the client's own player, which is a velocity write in the tick and not the constraint;
  its head still rides what it bit. Two players hooking one prop is then two host constraints on
  one host body, which is what single-player physics would do (`coop/items/hook_constraint`).

## Who owns what

| State | Owner | Shape |
|---|---|---|
| a deployed hook, while its thrower still holds it | that peer | keyed by (slot, sequence); the others render a parked mirror |
| a deployed hook once both ends are anchored | the host | the thrower hands over the hook's own save record; it is a save actor from then on |
| a hook's tie, the physics constraint | the host | a client breaks every tie a hook builds on it; the host builds a client's tie on its mirror of that hook |

## Wire messages

| Kind | Direction | Carries |
|---|---|---|
| `HookState` | the owner to all, relayed | a hook that still belongs to the player who fired it, keyed by (slot, sequence); create-or-update, re-sent on a slow keepalive |
| `HookDestroy` | the owner to all, or the host for a leaver | that hook is gone |
| `HookAnchorCommit` | a client to the host | the anchored hook's own save record, chunked |
| `HookAnchored` | the host to all | an anchored hook exists, as the same record |

## Late join

A hook its thrower still owns reaches a joiner on its keepalive. An anchored hook is a save actor
of the host's and arrives with the save, and the game's own load re-resolves both of its attach
keys in the joiner's world. The deployables that are not built reach a joiner only through the save.

## Known limits

| Limit | Evidence |
|---|---|
| The deployables other than the hook and the rope (nail gun, wall builder, explosives, fishing rod, physgun) are not synced; a nail or a wall placed by one peer reaches the others only through the save at their next join | `[V]` no lane under `coop/props` catches them |
| A client's hook into the ATV ties the host's ATV against the client's puppet; whether the ATV lane's corrector carries that pull back to the client's copy is not measured | `[?]` `coop/items/hook_constraint` ties any keyed actor the bite resolves; `coop/interactables/atv_sync` corrects rather than parks |

## Code map

| Concept | Files |
|---|---|
| the owner's stream, the mirrors and the brain park | `coop/items/hook_sync` |
| the anchor handoff | `coop/items/hook_anchor` |
| the tie, host-only | `coop/items/hook_constraint` |
| the props a tie claims for the driven-prop channel | `coop/items/hook_prop_claim` |
