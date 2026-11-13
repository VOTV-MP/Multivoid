# Deployables

## Purpose

The tools that leave a persistent actor behind (the grappling hook and rope, the nail gun, the wall
builder), timed explosives, the fishing rod and the physgun, each reverse-engineered with a design.
The hook, the rope, the nail gun's and a hand's nails and the wall builder's walls are built; the rest
are not. A prop a hook drags is the props page's
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

A nail and a custom wall (`coop/world/build_sync`) are save actors the game rebuilds from their own records
(`ue_wrap/actors/build_piece`). A nail gun's projectile, or a nail pressed by hand, leaves a `nail_C` (or one of its five
kinds) that ties the actor it went into to the one behind it, or to the world, with a physics constraint; its record
carries both ends' keys and the pins. A wall builder places a `customWall_unfinished_C`; bricks of its material finish it
into a `customWall_C`, or a `customWall_wetConcrete_C` that dries into one. The host sends each piece's record when it is
new or changed and its key once when it goes. A client makes the host's piece on its copy, or applies the record to its
piece of that key, a nail tied to that peer's actors of the record's keys. A client's copy runs nothing of its own: a nail
drops no item when it goes and no force breaks its tie (so a pin, `nail_pin_C`, which destroys itself as it snaps, goes
on a client only as the host's), an unfinished wall counts no bricks, wet concrete does not dry, and a wall's health and
maximum are scaled by 10,000, so the cracks read the host's ratio and no hit there breaks it. The one tie a client's
force breaks is a host nail in the board its player holds: that tie is breakable there as the game has it, and a pull
that snaps it goes to the host as a snap (`BuildIntent` op 4). The host's copy of a held board follows the
holder kinematically and never strains its own tie, so the host takes the client's word: when that client's hold drives
one of the nail's ends on the host and the nail is within 8 m, the host snaps its nail as force does (a pin goes and
drops its item), and its state answers either way, so a refused snap is tied again. A piece a client's player
makes keeps its key and goes to the host, which checks the class, the world and the reach (30 m) and makes its own under
that key; one the host does not answer in 10 s was refused and goes. A nail or unfinished wall that player takes away
goes to the host as a removal within 8 m, and the host destroys its own, dropping what the game drops; a piece lost on a
client otherwise is made again from its record. A wall's health on a client's copy moves only by what that peer does to
it -- its player's hit or repair, or a prop it threw -- so the client watches it every frame and sends the difference from
the host's last value to the host (within 30 m), showing it on its copy until the host answers. The host applies it as it
arrives, a loss through its wall's own `Damage`, which breaks and drops there, a gain as `fix` adds it, up to the maximum,
and sends the wall's state at once; a refused change is answered with the state as it was. A host nail whose tie is
broken outright on a client's copy is tied again from the host's record once 1.5 s have passed without the host's own snap
`[V]` (`harness/autotest/autotest_build.cpp`, each run with a rejoin: nails fired by each peer through two boards, the
held tie and a tie broken outright on the client's copy, a host pin whose board the client shoves and then grabs and
drags until the pin snaps on both, each peer's removal
with its one dropped nail item on both, an unfinished wall, its finish, a wet wall drying, a client's hit and repair shown
on its copy within 110 ms and on the host's wall, a log thrown by each peer counted once, the host's break, a client's
wall builder).

A poster on a wall rides the same lane. A poster item's put spawns a `poster_c` (the treehouse's and the custom poster's
classes too), a save actor whose record carries its picture (`type`, and `filename` for a custom one); the poster's own
action 7 takes it down into the taker's hand and destroys it, and the item is a prop the prop lanes carry. Each ran on the
one machine, so a poster put up or taken down on one peer stayed as it was on the others. The host now sends each
poster as a piece, a client's put-up goes to it as a new piece and a client's take-down as a removal within 8 m, and a
record applied to a poster already up repaints it through the poster's own `setPoster` (`coop/world/build_sync`) `[V]`
(`coop/dev/poster_probe`: before, the host's new poster never reached the client and a poster the client took down stayed
on the host's wall; after, the host's new poster was made on the client with its picture, the client's take-down took the
host's down, and the client's new poster was made on the host with its picture, both peers ending with the same six
posters). A custom poster's image is a file in each player's own posters folder, so another peer shows its own image by
that name, or none.

## Who owns what

| State | Owner | Shape |
|---|---|---|
| a deployed hook, while its thrower still holds it | that peer | keyed by (slot, sequence); the others render a parked mirror |
| a deployed hook once both ends are anchored | the host | the thrower hands over the hook's own save record; it is a save actor from then on |
| a hook's tie, the physics constraint | the host | a client breaks every tie a hook builds on it; the host builds a client's tie on its mirror of that hook |
| a nail, a custom wall in each stage, and a poster on a wall | the host | the piece's own save record; a client's new piece or removal is an intent the host checks |

## Wire messages

| Kind | Direction | Carries |
|---|---|---|
| `HookState` | the owner to all, relayed | a hook that still belongs to the player who fired it, keyed by (slot, sequence); create-or-update, re-sent on a slow keepalive |
| `HookDestroy` | the owner to all, or the host for a leaver | that hook is gone |
| `HookAnchorCommit` | a client to the host | the anchored hook's own save record, chunked |
| `HookAnchored` | the host to all | an anchored hook exists, as the same record |
| `BuildState` | the host to all | a nail's, a custom wall's or a poster's own record, or its key when it went; chunked |
| `BuildIntent` | a client to the host | a piece its player made, as its record, or took away, a wall it hit or repaired, or a nail its pull snapped; chunked |

## Late join

A hook its thrower still owns reaches a joiner on its keepalive. An anchored hook is a save actor
of the host's and arrives with the save, and the game's own load re-resolves both of its attach
keys in the joiner's world. Nails and walls arrive with the save and again at the ready edge, keyed, so the
joiner applies the host's record to the piece its save made; a piece of the joiner's save the host does not name
within 20 s goes. The deployables that are not built reach a joiner only through the save.

## Known limits

| Limit | Evidence |
|---|---|
| The timed explosives, the fishing rod and the physgun are not synced | `[V]` no lane carries them |
| A client's hit or repair shows on its own copy at once; the host and the other clients see it one network trip later, and a host's own change reaches the clients on the next poll, within half a second | `[RD]` `coop/world/build_sync` |
| A client's pull snaps a host nail by its own physics, and the host takes its word within the hold and reach checks; a snap the host refuses was not run, and the drill grabs through the player's physics handle as the game's grab does rather than by a press | `[?]` `harness/autotest/autotest_build.cpp` step R |
| A brick a client drops into an unfinished wall counts when the host's copy of the brick touches the host's wall; that path was not run, nor was a real wall builder's or nail gun's press | `[?]` the drill places walls and fires projectiles as the tools' own scripts do |
| The host captures every piece's record twice a second; the cost with hundreds of nails is not measured | `[?]` `coop/world/build_sync` |
| A client's hook into the ATV ties the host's ATV against the client's puppet; whether the ATV lane's corrector carries that pull back to the client's copy is not measured | `[?]` `coop/items/hook_constraint` ties any keyed actor the bite resolves; `coop/interactables/atv_sync` corrects rather than parks |

## Code map

| Concept | Files |
|---|---|
| the owner's stream, the mirrors and the brain park | `coop/items/hook_sync` |
| the anchor handoff | `coop/items/hook_anchor` |
| the tie, host-only | `coop/items/hook_constraint` |
| the props a tie claims for the driven-prop channel | `coop/items/hook_prop_claim` |
| nails, custom walls and posters | `coop/world/build_sync`; the pieces `ue_wrap/actors/build_piece`; `coop/dev/poster_probe` |
