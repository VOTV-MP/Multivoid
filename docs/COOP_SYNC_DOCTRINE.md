# How a system gets synced

The method every lane that survived converged on, written so that anyone can follow it without
re-deriving it. [ARCHITECTURE.md](ARCHITECTURE.md) holds the authority model this applies;
[COOP_DISPATCH_VISIBILITY.md](COOP_DISPATCH_VISIBILITY.md) answers whether a hook fires;
[COOP_ENTITY_EXPRESSION_MAP.md](COOP_ENTITY_EXPRESSION_MAP.md) says how each entity family gets its
identity; [CODE_MAP.md](CODE_MAP.md) says where lanes live; [STATUS.md](STATUS.md) says how far
each one is.

## In one paragraph

Find the base the system rests on and build that first. Reverse-engineer the system until its
verbs, its writers and its state are counted facts, not guesses. Give every element exactly one
authority; a client never writes shared-world state, it authors an intent or takes an assigned
lease, and the arbiter performs or validates. Pick the seam per verb from the dispatch ladder,
because visibility is a property of the dispatch path, not of the function. Park the receiver's
brain restorably, and mirror values only into parked brains. Give every entity its identity at
birth and never mint a second row for one actor. Ship every lane with its mid-activity join
answer, its suppression on the producer side, and evidence from a real run. No crutches: a
filter, a skip or a suppression bolted where the symptom shows is a defect, not a fix.

## Step 0: foundation first

Before designing the sync of X, ask whether X reads a value whose own sync is absent, partial or
wrong. The test: would X need a hold-and-retry register to tolerate the base's divergence? If so,
stop, park X with its reverse-engineering and design kept and the dependency named, build the
base properly, and resume X on top. The laptop's power lane parked this way on the power chain.

## Step 1: reverse-engineer until the facts are counted

The tools are `tools/bp_cpp.py` (a whole Blueprint as readable pseudo-C++, with `--offsets` for
the bytecode-offset listing that citations use), `tools/bp_cfg.py` (the control-flow graph), the
reflection dumps for layouts, and a disassembler for native code, in that order of reach. The
census a design owes: every writer of the state (all of them, by search and by reading, not the
first hit), every verb (the player-facing entry points and their dispatch opcode), every reader
that matters across peers, the actor's birth and death seams, and which pieces persist in the
save. A claim is measured only when the instrument is named; a design doc tags every fact as
measured or inferred, and the review attacks the difference.

## Step 2: exactly one owner per element

Pick the first row that fits, from the table on [ARCHITECTURE.md](ARCHITECTURE.md): shared-world
progression belongs to the host and clients mirror; a discrete, persistent, shared-world change a
client initiates is an intent the host validates and performs; a continuously simulated element
one peer is interacting with belongs to that peer by assignment, never by assertion, with the host
validating and able to reassign, and the authority returning on release; a peer's own body, pose,
camera and voice belong to that peer, never gated on receive; presentation belongs to nobody and
is never sent back.

Three rules ride on the table. The suppressed side is always the client-side producer, never a
receive gate, because a receive gate turns a cheat fix into a loss defect. An intent names what,
never what it costs. If the arbiter cannot hook the trigger, ask whether the actor can point at an
artifact the arbiter can resolve, the shop-order shape, before declaring a lane blocked.

## Step 3: the seam, per verb

Read the dispatch map first, then pick the cheapest seam that actually fires:

1. The `ProcessEvent` interceptor, which can cancel: engine-originated calls, ticks, BeginPlay,
   input, delegates, timers, interface events. It fires only on that dispatch.
2. The native function seam (`ue_wrap/core/ufunction_hook`, after the call): calls into native
   functions on every route, because every route funnels through the function's thunk. It
   cannot cancel; when a native must be cancelled, detour the C++ function itself.
3. The bytecode seam (`ue_wrap/core/vm_dispatch`, observe only, at the call site): every
   Blueprint-internal virtual dispatch, including calls from inside a graph; it carries no
   arguments and cannot cancel, so a consumer pairs it with the native seam or a per-site
   reconcile for values.
4. Per-site reconcile, the last resort: let the verb run, snapshot and diff the observable state,
   and converge to the authority's answer. Also the fallback when a seam exists but the verb lives
   in the graph.

A tier that could observe and cancel a script function on every route does not exist; until one
does, a Blueprint-internal verb is cancelled by per-site reconcile downstream, never by a receive
gate. Choose by census, not by habit: read the verb's dispatch opcode at every call site and write
the chosen seam and the reason into the design. If a hook "should fire", prove that it fires with
a probe before building on it.

## Step 4: park the receiver's brain

A value mirrored into an actor whose own Blueprint logic still ticks diverges or fights, because
both peers self-simulate. Every mirror therefore declares its parking, and the parking must be
restorable; a latch that cannot restore what it took is worse than one that refuses:

- Field latches: write the parked value, re-assert it if the game flips it back, restore it when
  authority arrives; only for types whose prior value can be restored.
- Scheduler kills: clear or gate the timer or tick that drives self-simulation, assert it dead on
  a slow watch, restore on ownership return.
- Physics receivers: a mirror is driven kinematic while it is remote-owned, re-latched if the game
  re-enables simulation, and given its velocity back on release. Never write a linear velocity
  into a body at rest; assigning a velocity wakes it.
- Never park by neutering the entity, deleting constraints or swapping in a fake actor class:
  keep the engine's entity and drive it, the parallel class hierarchy.

## Step 5: identity at birth, one row per actor

Identity is assigned at the birth seam, a spawn catch, a drop intent or a birth channel, never
minted passively by a census, which once produced thousands of zombie rows per join. One actor is
one row: adoption, never a second mint. A destroy-and-recreate transition, hold to drop to store
to equip, carries the identity across the actor gap. Keys are load-bearing, case included. If two
peers can create "the same" object independently, the design says which one is canonical and how
the loser dissolves, before it ships, not after the first duplicate.

## Step 6: the late-join row is part of the lane

A lane is not done until its mid-activity join answer exists in writing, one of snapshot, seed,
park, replay or unlatch, chosen, implemented and listed in the late-join column of
[STATUS.md](STATUS.md). "Don't join during X" is a crutch. The join order is fixed: identity,
save transfer, pre-world per-player state, world load, connect replay, per-lane seeds, the ready
gate ([join.md](join.md)); a new lane picks its slot in that order explicitly.

## Step 7: wire discipline

A new wire format or field bumps the build number in the same commit. A new reliable kind walks
the router checklist on [CODE_MAP.md](CODE_MAP.md): a kind that parses but does not route is a
silent black hole. Compatibility is byte-equality on the version pair per lobby, and the update
check informs, never gates. Receive boundaries are strict on format (refuse ill-formed input
whole) and permissive on motion and state (log, never block; display follows the sender, and trust
is a separate ledger).

## Step 8: evidence, or it did not happen

A build that compiles proves it compiles. The lane exists when the checks on
[../CONTRIBUTING.md](../CONTRIBUTING.md) pass: the hot-path check (no per-frame object-array
walk, no heavy work per dispatch or per tick), the file-size check, the deploy, the two-peer smoke
of thirty seconds or more, and a clean log diff. Every detector and gate
is shown red before it is trusted, because a gate that cannot fire passes forever. Differential
evidence beats absolute: baseline against change, with a negative control.

## Forbidden patterns

- A filter, skip or suppression added where the symptom appears instead of where the cause lives.
- A receive-side gate protecting shared state; the producer is the side that gets suppressed.
- Two implementations of one concept compiled together; a flag that re-enables retired behaviour.
- A mirror that "works" because the entity was replaced or lobotomised instead of parked.
- A per-frame full-array scan or any find-by-class on a hot path; a cached engine pointer without
  a world stamp.
- A bound or clamp applied symmetrically to the host: the host may cheat, and bounds are
  client-scoped.

## Worked references

- The intent shape, complete and minimal: `coop/items/order_sync` (the laptop shop order).
- Presser-authored device state with claim-free deltas: the desk lanes on [signals.md](signals.md).
- A host-authoritative world family with echo interception: `coop/world/weather_rain`.
- The assigned-syncer vehicle, its failures included: [vehicles.md](vehicles.md).
- Identity across destroy and create: `coop/props/prop_drop_intent`.
- The join spine: [join.md](join.md) and `coop/save/save_transfer`.
