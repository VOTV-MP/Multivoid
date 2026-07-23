# Hands-on runbook -- the TWO remaining container measurements (proto 125), take 2026-07-23

**Deployed:** `multivoid-0.9.0n-125.dll`, SHA-256 `05aff7799a707858`, MATCH x4 (HOST / CLIENT_1 /
CLIENT_2 / DEV). **Protocol 125. RELAUNCH BOTH PEERS** -- the join gate is byte-EQUALITY on the
(game target, build) pair, so a stale 124 peer is refused by design.
**HEAD going in:** `8357d138` (docs only since the DLL was built; the bytes are unchanged).
**Launch:** `mp_host_game.bat` on one box, `mp_client_connect.bat` on the other (named-window
launchers). Fresh save, current version -- never an old slot.

## What this take is FOR

The 2026-07-22 container take (`handson_runbook_2026-07-22_container_v125.md`) proved the SEQUENTIAL
half: Q-STACK green when the peers edit in turn. It **deliberately avoided a conflict** and it did
**not** run the Q-PROP discriminator. Those are the two measurements the profile
(`docs/COOP_SYNC_PROFILES.md` container facets) still flags as unmeasured. This take does exactly
those two, and nothing else. **They are INDEPENDENT** -- either can be green while the other is red;
write the two verdicts down separately.

| | M1 -- CONFLICT>0 | M2 -- Q-PROP discriminator |
|---|---|---|
| Question | does the compare-and-swap actually RUN and protect under a concurrent edit? | is the lost extracted item dropped AT `:279`, or never enqueued at all? |
| Channel | host `container_contents: CONFLICT` | client `[PROP-DROP] CLIENT authored` |
| Moves | container facet #3 (the CAS) `UNKNOWN/log` -> a measured verdict | container facet #4 root `[RD]` -> `[V]`; gates the `:279` firing-set probe |

---

## M1 -- PRODUCE a conflict (the concurrent CAS)

The 2026-07-22 take had `CONFLICT=0` on both peers, so the compare-and-swap **never executed once** --
the run says nothing about the case where the overwrite risk actually lives. This step is built to
make it fire. A CONFLICT counter that stays 0 is not "the CAS held"; it is "the CAS never ran"
(same logic as a positive control: a CAS is only tested by a non-zero conflict).

### Mechanics (so the choreography is not guessing)

- `g_localChangeMs[eid]` is stamped the instant THIS peer's own take/put verb mutates a container
  (`container_contents_sync.cpp:693`).
- The host re-publishes a new hash only on its next broadcast Tick, updating `g_publishedHash[eid]`
  (`:372`).
- `HostAcceptsClientWrite` (`:553`) refuses a client write -- and bumps the counter (`:573`) -- on
  either of two branches, both of which are a genuine concurrent conflict:
  - **HOST-side change in flight** (the ideal): the client's base still matched the last published
    hash, but the host touched the same container within the 1500 ms window (`kConflictWindowMs`) and
    had not yet re-published. Narrow window (~one broadcast cycle).
  - **STALE BASE**: the client authored from a state the host has already moved past (base != host
    published, or base == 0). Easier to hit, equally valid as "the CAS ran and refused".

### Steps

1. Both peers walk to the **same** world container and open it so it fully syncs (client base ==
   host published). Close it.
2. Both peers stand at that container. On a shared count "3-2-1-take", **both take from the SAME
   slot at the same instant**. Bias toward the ideal branch by having the HOST take a hair FIRST.
3. If no CONFLICT line appears (see grep below), repeat step 2 -- each attempt is cheap. Try ~5
   times before concluding the timing is too loose.

### Read (HOST log)

```
grep "container_contents: CONFLICT" "<host>/multivoid.log"
```

Record, VERBATIM, the branch phrase and the running count from the line:

```
container_contents: CONFLICT eid=N slot U -- <branch> (author base=.., host published=..).
Write REFUSED; re-publishing host truth to the author. Total refused this session: K
```

- `<branch>` = `a HOST-side change is in flight within the conflict window` (ideal) **or**
  `the author edited a state the host has not published (STALE BASE)`. Both count as CAS-RAN.
  Note which -- they mean different things and the first take of this CAS misread one as the other.

### The convergence check (do this the moment a CONFLICT fires)

A refusal is only half the story. After it, the host re-publishes its truth to the author. Open the
SAME container on **both** peers and compare:

- **CONVERGED (correct)** = both peers show the HOST's post-conflict contents; the refused client's
  edit was overwritten by host truth, no phantom.
- **DIVERGED / DUP (open sub-case)** = the refused client keeps the item it tried to move, or the
  counts disagree. This is the known-open "the CAS refusal path leaves the item with the refused
  client" (`container_contents_sync.cpp:598-607`). If you see it, that is the finding -- record the
  exact counts on each peer.

### M1 verdicts to write

- **CAS-RAN** (`CONFLICT>0`, branch noted) vs **CAS-NEVER-RAN** (stayed 0 after ~5 tries -> timing
  too loose, not a pass).
- **CONVERGED** vs **DIVERGED/DUP** (with the per-peer counts).

---

## M2 -- the Q-PROP discriminator

Q-PROP was RED on 2026-07-22 (a client-extracted item never reached the host world) but its root is
only `[RD]`: the log cannot separate "dropped at `prop_drop_intent.cpp:279`" from "never enqueued at
all" -- both are silence. This ~30 s step separates them.

### Steps

1. As the **CLIENT**, pick up a **pre-existing** ordinary world prop that both peers can see -- NOT
   one just extracted from a container (that is the case under test and proves nothing), and NOT a
   reel / module / drive class (those pass via the `freshBirth` whitelist and leave the branch
   undifferentiated).
2. Place it back down.
3. Record the prop's class in the report regardless.

### Read

Client log -- the authoring edge:

```
grep "\[PROP-DROP\] CLIENT authored" "<client>/multivoid.log"
```

Host log -- the positive echo (the host actually spawned the client-placed prop):

```
grep "\[PROP-DROP\] HOST spawned client-placed prop" "<host>/multivoid.log"
```

### M2 verdicts to write

- **`CLIENT authored drop intent` appears** -> the authoring path downstream of the gate works, so
  the burger's loss IS admission at `:279`. Root moves `[RD]` -> `[V]`; the `:279` firing-set probe
  (gate:me) may start. Confirm the host echo line also appears.
- **No `CLIENT authored` line** -> the root is UPSTREAM of `:279`, in the enqueue. The firing-set
  probe must NOT start -- it would probe a line that is not on the path. This is a bigger finding.
- **`CLIENT authored FRESH-BIRTH intent`** (not `drop intent`) -> you picked a whitelisted class;
  redo with an ordinary prop. The path is alive but the branch is not isolated.

---

## Log locations

- Host: `Game_0.9.0n_HOST/.../multivoid.log`
- Client: `Game_0.9.0n_CLIENT_1/.../multivoid.log`

`multivoid.log` is the current run; `multivoid-prev.log` is the previous. Grep the current one.

## What to send back

Six lines, no interpretation needed on your end -- just the raw verdicts:

1. M1 branch phrase (verbatim) + `Total refused` count, or "CONFLICT stayed 0 after N tries".
2. M1 convergence: CONVERGED, or the two per-peer counts if DIVERGED/DUP.
3. M2 client line: the full `[PROP-DROP] CLIENT authored ...` line, or "absent".
4. M2 host echo: present / absent.
5. M2 prop class.
6. Anything unexpected in either log (a CONFLICT during M2, a crash, a stall).
