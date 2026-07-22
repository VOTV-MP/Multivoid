# Hands-on runbook -- R11 container contents (v124), 2026-07-22

> **OUTCOME (same day): R11 is VERIFIED GREEN.** This runbook is kept as the point-in-time record
> of the take. Two takes came back RED first; the cause was NOT the design but
> `vm_dispatch`'s `g_allResolved` permanent latch, which left this lane's verbs unresolved and
> its callback silently dead (fixed `3027aeed`). After the fix the client saw the delivered
> burgers and both peers' logs matched: `shipped 2/1/0 records` <-> `applied 2/1/0 records`.
> The proof table below is updated to what the take actually established.
> Still open and NOT closed by this take: **R11b** (a client's extraction has no upstream lane --
> the dupe was reproduced live, 3 burgers from an order of 2).

**Deployed:** `multivoid-0.9.0n-124.dll`, md5 `A07DC35FC6104417`, on all 4 installs
(HOST / CLIENT_1 / CLIENT_2 / DEV).
**Protocol:** 124 (was 123). **RELAUNCH BOTH PEERS** -- the join gate is byte-EQUALITY on the
(game target, build) pair, so a still-running 123 peer is refused, by design.
**HEAD:** `7305d12e`. Commits in this take: `135edb21` (codec extraction), `bfde48cb` (the lane),
`7305d12e` (the R11b baseline measurement).

---

## What this take is actually deciding

**ONE question: does the client SEE the container's contents?** R11's take-4 symptom was "the drone
delivery bag is empty on the client, full on the host (~686 volume vs 0.0)". The verdict is pure
RECEIVE. Nothing else in this take is a verdict.

## LOOK, DO NOT TAKE

**Do not extract anything from a world container as the CLIENT during this take.**

Not superstition -- a measured open lane. There is no upstream (client -> host) lane for a client's
container mutation: `takeObj` runs locally on every peer and the host is never told. Extracting as the
client will, on the next host-side container mutation, make the item reappear -- and that is **R11b**, a
separate PRE-EXISTING root (measured against the v123 baseline; see
`research/findings/computers-devices/votv-take4-hands-on-bugs-2026-07-21.md` -> R11b). It predates this
build. Doing it during this take would confound the R11 verdict with an unrelated known bug.

Taking things out as the HOST is fine.

---

## Steps

1. Relaunch BOTH peers on the new DLL (proto 124). Fresh-ish save per the testing rule.
2. Client joins. Wait for the join to settle (the world-ready line in the client log).
3. **Baseline check, before any delivery:** walk to ANY world container that already has contents (a
   garbage bin, a backpack, the mailbox) and open it as the CLIENT. It should show contents, not 0.0.
   This exercises the CONNECT-SEED path -- the one path already proven cross-peer in the smoke.
4. **The R11 case:** order something from the laptop, let the drone deliver, then open the delivery
   container **as the client**. It should show the ordered items and a non-zero volume.
5. Open the same container as the HOST and compare the item list + the volume number.

## The verdict

- **R11 GREEN** = step 4 shows the delivered items on the client, and step 5 shows the same list and a
  matching volume on the host.
- **R11 RED** = the client still shows 0.0 / empty after the delivery.
- **PARTIAL** = step 3 works (connect seed) but step 4 does not (the 0x45 edge). That is the specific
  half the smoke could NOT prove -- see below -- and it is the most likely failure mode.

## What is already proven, and what this take is the FIRST test of

| Layer | Status going in |
|---|---|
| read GObjStack -> serialize -> wire -> deserialize -> apply | **PROVEN cross-peer** in the LAN smoke: host shipped 284 containers / 241 non-empty / 1718 records; client applied 241 MATCH, 0 MISMATCH, 0 MISSING (largest 43 records) |
| the connect-seed path (join) | **PROVEN** (same run); 292 parks all resolved by retry, 0 expired |
| the 0x45 `addObject` EDGE (a live delivery) | **PROVEN 2026-07-22** -- host `shipped 2 records` at 17:29:23, client `applied 2 records` same second. Also proven for REMOVE: `shipped 1` / `shipped 0` -> `applied 1` / `applied 0` |
| `updateVolumesAndMass` re-deriving `currVol` | **PARTIAL** -- the host's own `currVol` tracks (2991.4 -> 1495.7 -> 0.0, census). The CLIENT's mirrored `currVol` was measured STALE at the last sample (`contents=0 vol=1495.7`), so the re-derive on the apply path is NOT confirmed. Own follow-up |
| a NESTED container (a "case" inside the delivery) | **STILL NOT PROVEN** -- the take ordered burgers, no nested container was involved. By design it arrives EMPTY, not broken (its own contents are increment 2). If the take-4 order included a case, expect the case to be present but empty on both peers |

## What to read in the log if it is RED

Client log, `Game_0.9.0n_CLIENT_1/.../multivoid-*.log`:

- `container_contents: eid=N applied M records` -- the apply fired. If M is right but the UI shows 0.0,
  the bug is in the re-derive (`updateVolumesAndMass`), not the lane.
- `container_contents: eid=N not resolvable yet -- parked (TTL 30s)` followed by no later `applied` for
  that eid, and eventually `expired` -- the container's element never bound. Identity problem, not a
  contents problem.
- `container_contents: eid=N does not resolve to a container -- refusing` -- the eid resolved to the
  wrong actor.
- Nothing at all for the delivery container after the drone arrives -> **the 0x45 edge never fired**.
  Check the host log for `[vm_dispatch] registered verb addObject` and whether the host emitted
  `container_contents: eid=N shipped ...` at delivery time.

Host log: `container_contents: eid=N shipped M records (B bytes)` at the moment of delivery is the proof
the edge fired. Its absence with a correct client-side connect seed pins the failure to the edge.

## Not in scope for this take

- R11b (client extraction) -- see above; its own investigation, arbiter territory.
- The INSERT direction (a client putting something INTO a world container) -- **unmeasured**, no claim.
- Increment 2: the transitive nested-container walk.
