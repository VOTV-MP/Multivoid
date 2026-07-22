# Hands-on runbook -- container v125 (proto 125), take 2026-07-22

**Deployed:** `multivoid-0.9.0n-125.dll`, SHA-256 `05aff7799a707858`, MATCH x4 (HOST / CLIENT_1 /
CLIENT_2 / DEV) -- verified this session. (An earlier revision of this file cited
`f79eb2ce86cdc46e`; that predated the audit-fix commit `79534796` and was stale.)
**Protocol 125. RELAUNCH BOTH PEERS** -- the join gate is byte-EQUALITY on the (game target, build)
pair, so a still-running 124 peer is refused by design.
**HEAD:** `c6a212c7`. **Status going in:** AS-BUILT + smoke green both directions. **NOT verified.**

This take supersedes `handson_runbook_2026-07-22_container_v124.md`, whose instruction was "look, do
not take". **That is inverted here: taking is the whole point.**

---

## This take decides TWO INDEPENDENT questions. Keep the verdicts SEPARATE.

Different channels, different roots. One can be green while the other is red, and that is NOT a
contradiction -- it is two bugs, one fixed and one still open.

| | Q-PROP | Q-STACK |
|---|---|---|
| Channel | prop BIRTH broadcast (`PROP-DROP` / `SPAWN broadcast`) | container DECREMENT (`container_contents`) |
| Root | `prop_drop_intent.cpp:279` class whitelist -- **still open, no fix shipped** | client->host container lane -- **built in v125** |
| Steps | (a) + (b) | (c) |

Do not let "the stack works" overwrite "the prop is lost". Write both verdicts down.

---

## Steps -- run in THIS ORDER (the order IS the instrument)

### (a) POSITIVE CONTROL -- the HOST takes an item out of a container

Walk to any world container with contents. **As the HOST**, take one item out.

**A line of the prop-birth channel MUST appear in the host log:**

```
grep -E "spawn-seam adopted|MIRROR ambient spawn" <host>/multivoid.log
```

**CORRECTED 2026-07-22 after the take.** This file originally said to grep
`PROP-DROP|SPAWN broadcast`. Both strings are wrong for the HOST: `PROP-DROP` belongs to
`prop_drop_intent`, which is CLIENT-only, and `SPAWN broadcast` belongs to the `takeObj` POST
observer in `prop_container_extract.cpp`, which has never fired once (it is 0x45-invisible). Grepping
them on the host returns 0 for a perfectly healthy run -- the control read as failed when it had
actually passed under a different name (`host_spawn_watcher: spawn-seam adopted`, 5 lines in the
2026-07-22 take). A control that names the wrong channel is worse than no control: it manufactures a
void verdict out of a good run.

**If NO line appears, STOP -- the run is void** and steps (b)/(c) prove nothing about Q-PROP.

This step exists because of a real near-miss on 2026-07-22: the prop-birth channel printed ZERO
lines in an entire smoke run, so the silence at a client extract *looked* like proof the host was
never told, and it was not proof of anything. **The control and the test must ride the SAME channel**
-- a container-decrement line does NOT show the prop-birth channel is alive.

### (b) THE TEST -- the CLIENT takes an item out, with NO conflict

**The host must not touch that container during this step.** Single variable: no simultaneous grab,
no CAS conflict. As the CLIENT, take one item out. Then, as the HOST, look at the world where the
client is standing.

- **Q-PROP RED** = no new prop-birth line on the host, and the host cannot see the item in the world.
  This is what code-reading predicts: `prop_drop_intent.cpp:279` drops any birth that is neither a
  parked place nor a whitelisted class (reel / module / drive). A burger is neither.
- **Q-PROP GREEN** = the line appears and the host sees the item. Then the code-level prediction is
  wrong and some other lane carries it -- say so plainly; it kills the whole `:279` thread.

### (c) Q-STACK -- the v125 container decrement

Find a container holding **2** of something. **As the CLIENT**, take **1**. Then open the SAME
container **as the HOST**.

- **Q-STACK GREEN** = the host sees **1**.
- **Q-STACK RED** = the host still sees **2**.

This is the symptom reported on b124 ("3 burgers from an order of 2"). On b124 the client's container
write was dropped by an early `IsHost()` return **by construction**, so the host was *supposed* to see
2 -- that build's behaviour was not a mystery. v125 built the missing half; this step is its first
real test. It worked in the autonomous smoke (host logged `eid=4941 applied 6 records` right after
the client went 7->6), which is evidence the lane runs, not evidence it is right in play.

---

## What to read in the logs

Host log, `Game_0.9.0n_HOST/.../multivoid.log`:

- `container_contents: eid=N applied M records` -- the client's decrement crossed and was accepted.
- `container_contents: CONFLICT eid=N slot U -- ...` -- a CAS refusal. **Zero expected** in this take:
  (b) and (c) are deliberately conflict-free. If one appears, the run had a conflict you did not
  intend and both verdicts are confounded -- note it rather than reading through it.
- `PROP-DROP` / `SPAWN broadcast` -- the prop-birth channel: step (a) control, step (b) test.

Client log, `Game_0.9.0n_CLIENT_1/.../multivoid.log`:

- `container_contents: 0x45 verb callback ENTERED ... (role=CLIENT)` -- the extract edge is live.
- `container_contents: eid=N shipped M records` -- the client authored its slice.

---

## Known-open going in (do NOT report these as new findings)

- **`currVol` does not re-derive on extraction.** Measured in the smoke: records went 7->6 while
  `currVol` stayed 28579.0 on BOTH peers. The two peers agreeing is not evidence the re-derive works
  -- neither of them ran it.
- **Nested containers arrive EMPTY** by design (increment 2, not built).
- **The refusal path leaves the item with the refused client** -- reachable only when a CONFLICT
  fires, which this take deliberately avoids.
- **Slot 0** (the player's own personal container) is excluded fail-closed by BOUNDARY 1 and is read
  by no lane at all.
