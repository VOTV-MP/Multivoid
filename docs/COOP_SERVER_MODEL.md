# The server model — what "our server" is, and how embedded equals dedicated

**Status: DESIGN, 2026-07-20. Not built.** Produced by a `/qf` pass (2 critic rounds) plus four user
decisions. Every measurement below was taken this session and is cited; nothing is carried framing.

**What this doc owns:** what the server IS, why the embedded and the dedicated server are the same
thing, which world state the server can hold, and what it costs to get there.
**What it does NOT own:** who may write which element — that is `COOP_SYNCER_MODEL.md` (authority).
Read that one for the per-element rules; read this one for the deployment and state model.

---

## 1. The two precedents, and why we need a half from each

| | Who simulates the world | Is there an embedded server? |
|---|---|---|
| **Minecraft / Paper** | The server. World gen, mobs, redstone are server code. | Yes — same code, in-process; the local client talks to it over the normal packet path. |
| **MTA** | The clients. The server never runs GTA. | Yes — but as a **child process**, see §2. |
| **Us** | Clients (forced: VOTV's world logic is cooked UE4 blueprints we cannot rewrite) | To be built — MTA's shape. |

Minecraft's answer to *"what does the server do"* (simulate everything) is unavailable to us: it would
mean reimplementing VOTV, which `ROADMAP.md:109-111` calls a decade-class trap. MTA's answer is
available and is the one we take.

**Do not confuse "the server owns state" with "the server simulates".** `[V]`
`reference/mtasa-blue/Server/mods/deathmatch/logic/CElement.h:265` — the MTA server element holds
`CVector m_vecPosition`, and `:103` exposes `GetPosition()`. The server owns full element state and
never runs the game. Owning state is the norm of the precedent, not a step toward reimplementation.

---

## 2. Deployment — one binary, spawned or launched (MEASURED)

`[V]` MTA's "host from in-game" is **not an embedded library**. The client spawns the real dedicated
server executable as a child process — `Client/mods/deathmatch/logic/CServer.cpp:126-131`:

```
"MTA Server.exe" --child-process --config "<file>"
```

with `CreateJobObjectW` + `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` (`:56-60`) so the server dies with the
game even on a crash, stdio pipes for console output, and an anonymous ready-event handed to the
child via stdin.

**Consequence for us, and it is the whole point:** there is no "embedded build" to keep honest.
Identity between embedded and dedicated is **physical, not disciplinary** — it is literally the same
binary. The arbiter cannot cheat by reading the engine, because it is not in the engine's process.

This **supersedes** the build-invariant proposal in `COOP_SYNCER_MODEL.md` **§1c** ("Make it a build
invariant, not a discipline" — a subtree forbidden from including `ue_wrap/`). That invariant existed to stop the embedded mode from silently reading
the engine; a separate process makes the cheat impossible rather than detectable. Keep the subtree
rule only as belt-and-braces if it is free.

**Also consequent:** the host's game becomes an ordinary client of the arbiter and loses its
privileged in-process authority path. That is the structural change; everything else follows.

**Cross-platform from day one.** MTA's child-launch is Windows-only (job objects, `CreatePipe`). Ours
must sit behind a launch abstraction or the Linux build hits it for no reason.

---

## 3. User decisions (2026-07-20) — these are settled, do not re-derive

1. **An empty server FREEZES.** Nothing ages while nobody is connected; the first joiner resumes
   exactly where it stopped. No offline-progression formula is needed.
2. **The arbiter holds its own state**, rather than acting as custodian of an opaque save blob.
3. **Simulation happens where a player is** — zero players is the degenerate case of that one rule,
   not a separate mode. Modelled on Minecraft's per-chunk freeze (no player in the chunk, no
   simulation there; furnaces stop smelting).
4. **The arbiter is a child process from day one** (§2), so nothing is "extracted later".

### The correction decision 3 needed (user, same session)

"Simulation follows the player" does **not** hold as stated for BP latent machines: they are bound to
the **process**, not to a region. Stop the process and everything stops; leave it running and
everything runs regardless of where any player is. VOTV is not chunked and its blueprints were not
written to be paused.

So the split returns on a different axis than region-occupancy:

- **player-anchored** rollers — at zero players they have no anchor and nothing to do. `[V]`
  `COOP_RNG_AUTHORITY.md:92` `pineconeSpawner` is PLAYER-CAMERA-anchored (dump-measured).
- **globally latent machines** — driven by the process, region-blind. `[V]` `:94` `ticker_wispSpawner`
  is ABSOLUTE-map-coords-anchored (dump-measured, +-60-70k X/Y, Z=80k); `[V]` `:365`
  `prop_food_mushroom_C` shows **4462 steady TickInterval re-arms** (food-spoilage self-simulation).

### Why the freeze survives that correction anyway

The globally latent class splits again, and both halves are freeze-compatible **independently**:

- **Accumulators** (`concreteBucket dryTimer += DeltaSeconds` — `COOP_WORLD_PROP_DIVERGENCE.md:30`)
  are linear in elapsed time, therefore recomputable. They do not need to run; they need to be
  *recomputed* at unfreeze. That is a write, not a resume.
- **Gated random rolls** (spawners, `gate[0.02]`) simply do not fire. An unobserved ambient spawn is
  not state anyone returns to inspect.

**Therefore no Wine simulation carrier is required for a zero-player dedicated server**, and a Linux
VPS runs the arbiter natively. This retires the reasoning in `ROADMAP.md:62-66` — see §8.

---

## 4. THE RULE

> **The arbiter holds values and anchors. The engine holds only what has a world-dependent rate.**

An accumulator is never streamed; it is **anchored**. Store the start stamp once, let every peer
compute the value. This is `CClock`'s shape — `[V]`
`reference/mtasa-blue/Server/mods/deathmatch/logic/CClock.cpp` is 58 LOC and `CClock::Get` is pure
formula, `(GetTickCount64_() - m_ullMidnightTime) / m_ulMinuteDuration`. **There is no tick.**

What anchoring buys, all of it for free:

- no stream at all, versus periodic or on-change pushes;
- divergence is impossible — one formula, one anchor;
- **late join is solved** (principle 8): the joiner gets one stamp and is instantly correct, with no
  snapshot cadence and no catch-up;
- **freeze works**: store elapsed at pause, re-anchor on resume.

**The condition, and it is the thing to measure:** anchoring is only valid if the accumulator's RATE
is constant. If drying is slower in rain or faster indoors, the formula needs those inputs too, and
if they are engine facts the element goes back to a syncer. See
`[[lesson-converges-for-free-needs-complete-input-readset]]`.

---

## 5. Worked example — the cement bucket

Chosen because one object contains both classes of state.
`[V]` `COOP_WORLD_PROP_DIVERGENCE.md:25,30,49-50,99` — fields `units` and `dry`, setters `updStage`
(units -> stage mesh) and `updDry` (dry -> material).

**`units` — a discrete use counter.** Changes only on a player action. Pure data; no engine needed.
The arbiter owns it outright: a scoop is an **intent**, the arbiter decrements and broadcasts the new
value, both peers call `updStage`. Shape already proven in the tree — value-ops plus a host-canonical
array (L8 physMods, `[[lesson-set-state-syncs-as-value-ops-plus-canonical]]`).

This is where the arbiter earns its existence: **two players scooping at once is a double-spend**
today, because each peer decrements its own counter. The arbiter serialises and decides who got the
last use. That is the reason for the architecture, not a side effect.

**`dry` — an accumulator.** The planned fix (`docs/items/concrete.md` §3) parks the brain and pushes
two scalars on change. The push is more expensive than necessary: `dry` is a function of elapsed
time, so the arbiter stores **one stamp** (when it was poured) and every peer computes locally — with
all four benefits in §4. **Parking the brain still survives** from the planned fix, otherwise the
local accumulator fights the computed value.

---

## 6. The four kinds of engine read (census, 2026-07-20)

"The lane reads the engine" turned out to be far too coarse. Measured by inspecting call sites:

| kind | what it is | fate under the arbiter | evidence |
|---|---|---|---|
| **Intent production** | reading the LOCAL player to learn what they are doing | **stays in the game process, forever and legitimately** | `device_occupancy.cpp` — 9 of its 9 reads (`ReadMainPlayerLookAtActor:222`, `ReadActiveInterface(local):326,355,467`) |
| **Handle validation** | is this pointer still a live actor of this class | **disappears** — the arbiter holds eids, not pointers | `drive_sync.cpp` — ~12 of 15 reads (`R::IsLiveByIndex:124`, `R::ClassOf`, `IsDriveClass`, `EidForActor`) |
| **Outcome capture** | reading what an engine machine decided | **stays** — the engine computes, the arbiter records | `signal_catch_sync.cpp` — 5 of 6 reads (`CD::ReadCoordSignal:74,319,382,401`, `D::ReadSlewFromMovingDish:222`) |
| **Canon derivation** | reading the engine to BUILD the state we then treat as authoritative | **must be inverted to write-only** | the 32 sites in §7 |

The first three are not obstacles. Only the fourth is work.

---

## 7. The price, as a number

Canon-derivation reads across the tree — **32 sites in 9 lanes**:

| lane | sites |
|---|---|
| `interactables/meadow_db_sync.cpp` | 7 (`MS::ReadRow` x4, `MS::Count` x3) |
| `interactables/laptop_sync.cpp` | 5 (`ReadSlot`) |
| `interactables/signal_sync.cpp` | 5 (`ReadRow` x3, `Count` x2) |
| `interactables/keypad_sync.cpp` | 4 (`ReadState`) |
| `world/email_sync.cpp` | 3 (`ReadRow`) |
| `interactables/drive_sync.cpp` | 3 (`ReadDriveRow`) |
| `interactables/turbine_sync.cpp` | 2 (`ReadState`) |
| `interactables/laptop_buffer_sync.cpp` | 2 (`ReadSlot`) |
| `interactables/drive_rack_sync.cpp` | 1 (`ReadDriveRow`) |

The inversion, stated once: today `MS::ReadRow` builds truth from the engine and `MS::ApplyAddSignal`
pushes into it. After: the arbiter's record is truth, `MS::Apply*` is the only direction, and
`MS::ReadRow` survives solely as a reconciliation check — never as a source.

**So the first real increment toward the arbiter is not 68 kinds and not a rewrite: it is 32 reads to
invert, plus an arbiter-side record for nine lanes.**

`[?]` **Honesty bound on this number.** The census greps by NAME SHAPE
(`Read*Row|State|Slot|Contents|List`, `Count`). It can miss a canon read named otherwise and can
include a read that only gates. Treat 32 as an order-of-magnitude figure on a uniform measure, not a
per-site verified classification. The per-site pass is cheap now that the list exists — it is the
next step.

---

## 8. What this supersedes

`ROADMAP.md` **phase 6** fixed "dedicated = the HOST GAME RUNNING HEADLESS, driven by our DLL — NOT a
from-scratch server binary" as an *"architectural commitment, decided up front"* (2026-07-19). (Cite
the phase, not a line range: that entry now carries this document's own supersession note, so a line
citation would be circular — it rotted within the same session that wrote it.)

That commitment **fuses two separate questions — who ARBITRATES and who SIMULATES** — and it predates
both the arbiter concept (2026-07-20) and this session's `CServer.cpp` measurement. Split:

- **who simulates:** still the game process, still clients. That half stands.
- **who arbitrates:** a separate binary from day one. That half is replaced.
- **the Wine carrier:** no longer required for a zero-player server (§3), so phase 6 shrinks to the
  ghost-host question plus a Linux build of the arbiter.

A `/qf` round caught that I had inherited this ROADMAP framing and then cited it back as independent
confirmation of my own conclusion. Recorded so it is not repeated.

---

## 9. Blocking measurements — do these before designing further

1. **Per-site classification of the 32 canon reads** (§7). Cheap now; converts an order-of-magnitude
   figure into a work list.
2. **The two accumulator measurements are OWNED BY `COOP_WORLD_PROP_DIVERGENCE.md`** (its 2026-07-20
   section), not by this document — they are about the prop class, not about the signal tract:
   (a) is `dryTimer += DeltaSeconds` unconditional or gated (cheapest measurement outstanding; if
   gated, anchoring fails and the bucket returns to a syncer whole); (b) the census of
   self-simulating props and the SHAPE of each mutation — exactly ONE accumulator is confirmed, so
   rule-of-three is not met and §4's anchor rule currently rests on a single instance.
3. **Does `updDry` survive an externally written value with the brain parked?** The planned fix keeps
   parking; nobody has measured the setter under an external write without a tick.
4. **How does VOTV advance time** — tick-accumulated or formula-from-a-stamp? NOT measured.
   `getNamedDaytime` is only a formatter (2 definitions; the other 12 hits are CallFunc result pins).
   Freeze/re-anchor currently rests on the MTA template, not on VOTV code.
5. **Anchor census of the remaining spawners.** Only 2 of ~22 have a measured anchor
   (`pineconeSpawner`, `ticker_wispSpawner`); the rest are `NEEDS-PROBE` in `COOP_RNG_AUTHORITY.md`
   T1-1 and T2-6. Needs a fresh dump probe — it cannot be closed from the docs.

---

## 10. What this does NOT do

- It does not make the arbiter authoritative over **simulation**. With players connected, class-(c)
  world state is simulated by a client's engine and an engine-free arbiter cannot validate it — it is
  a syncer-table relay for that class. Consistent with `COOP_SYNCER_MODEL.md` **§10** ("does not make
  the host's simulation authoritative over physics"), but stated here because it is easy to
  over-promise.
- It does not remove the save blob. Bootstrap still ships one; the arbiter's canon covers only what
  crosses the wire. **That boundary is the progress scale:** every new sync lane extends the server's
  authority, so phase 8 arrives by accumulation rather than by a rewrite event.
- It does not solve the two-representations problem. During migration the engine's state and the
  arbiter's record coexist — the second parallel path RULE 2 dislikes. Tolerable only with an
  explicit end date, and only while the arbiter's record is authoritative and the engine's is a
  projection.

---

Related: `COOP_SYNCER_MODEL.md` (per-element authority — the other half) · `docs/ROADMAP.md`
phases 6-8 · `COOP_WORLD_PROP_DIVERGENCE.md` (the self-simulating prop class) ·
`COOP_RNG_AUTHORITY.md` (spawner anchors, ticker census) · `docs/security/MTA_PRECEDENT.md`
