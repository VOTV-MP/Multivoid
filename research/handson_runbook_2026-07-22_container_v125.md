# Hands-on runbook -- container extraction (R11b), v125

**Build:** `multivoid-0.9.0n-125.dll`, SHA-256 `f79eb2ce86cdc46e...`, MATCH on all 4 installs, 0 strays.
**Proto:** **125** (was 124) -- **RELAUNCH BOTH PEERS.** A v124 peer cannot join; the join gate refuses on
byte-equality of the version pair, by design.
**HEAD:** `163fc974`. **Status going in:** AS-BUILT + smoke green both directions. **NOT verified.**

This take supersedes `handson_runbook_2026-07-22_container_v124.md`, whose instruction was "look, do not
take". **That is now inverted: taking is the whole point.**

---

## What changed since your last take

v124 gave the client the RECEIVE half -- it could finally SEE container contents. It could not tell
anyone what it did to them: the callback returned at `IsHost()` and every client extraction was dropped.
That is the dupe you hit (you took one of two burgers, the host still saw two, three came out of an order
of two).

v125 makes the lane bidirectional. Whichever peer's verb fired authors the container slice; the host
checks it against what it last published, applies it, and passes it to the other peers without echoing it
back to whoever did it.

Also in this build: the container's volume re-derive had **never once run** on any peer (it was resolved
from the wrong class and silently skipped). That is the `686` vs `0.0` you photographed.

---

## The scenario -- your own, verbatim

1. **Order something** that arrives in the drone sack (burgers again is ideal -- it is the measured case).
2. Wait for the delivery. **CLIENT: open the sack.** You should see the items (this is v124's half; it
   was verified last take, so a failure here is a REGRESSION and worth stopping on).
3. **CLIENT: take ONE item.**
4. **HOST: open the same sack.**
   - **PASS** = the host sees the remaining count, i.e. **one fewer**. Two delivered, client took one,
     host sees one.
   - **FAIL** = the host still sees both. That is the dupe unchanged.
5. **HOST: take the rest.** **CLIENT: re-open the sack.** It should be empty, and no item should have
   reappeared in your hands or the world.

If you have a third peer up, the interesting extra: with CLIENT_1 and CLIENT_2 both watching, have
CLIENT_1 take an item and confirm CLIENT_2 sees the container shrink too (that is the relay path; the
author itself must NOT get its own state echoed back).

**Optional, and genuinely useful:** try to have both peers grab the SAME item at the same instant. No
rollback is built on purpose -- one of you will see the container snap back. What matters is whether that
ever happens at all in real play; the log counts it.

---

## What to read in the logs afterwards

Both logs are `Game_0.9.0n_{HOST,CLIENT_1}/WindowsNoEditor/VotV/Binaries/Win64/multivoid.log`.

**Must be present on BOTH peers** (if missing, the lane is inert and nothing else matters):
```
container_contents: 0x45 verb callback ENTERED for the first time on this peer (role=...)
```

**The client's take crossing the wire:**
```
CLIENT  container_contents: eid=<N> shipped <n> records (<b> B) [client-authored]
HOST    container_contents: eid=<N> applied <n> records
```

**The volume fix** (this is the 686-vs-0.0 line):
```
container_contents: re-derive verbs resolved (updateVolumesAndMass=<ptr> on prop_container_C, ...)
```
A `WARN ... re-derive verb MISSING` instead means the fix did not resolve on your build -- report it.

**Conflicts** (expected: zero, unless you deliberately raced):
```
container_contents: CONFLICT eid=<N> slot <S> -- <which condition failed> ... Total refused: <count>
```
That count is the evidence that decides whether a rollback ever gets built. A handful over a long session
= leave it alone. Constant = the design owes a rollback.

---

## Known-open, so you do not report them as new

- **The player's own inventory container** (slot 0) is deliberately untouched -- excluded fail-closed.
  Moving an item from the container into your own inventory and then into your HAND crosses a second,
  separate store; only the container end is synced by this build.
- **Putting an item INTO a world container** (`putObjectIn_overlap`) is UNMEASURED. If it misbehaves,
  that is a known gap, not a v125 regression.
- **Nested containers** (a bag inside a crate) arrive EMPTY by design, not broken -- the transitive walk
  is increment 2.
- Simultaneous grab of the same item: window narrowed, class alive.
- Unrelated and still open from take 4: the drone console (a client pressing E cannot send the drone
  back), the sack not dangling on the client, dumb-kerfur active/off not mirroring.
