# Tester-log triage: huoyan1231 host log, b125 (2026-07-26)

**Status: TRIAGE MAP (converged /qf, 8 rounds, "that holds" at R8). Root-cause map only — fixes are
NOT designed here; each fix direction below names its own later design pass.**

Source: `Game_0.9.0n_HOST/.../multivoid_tester_huoyan1231.log` (host side, b125, session 15:44-16:26,
host nick 'votvhost' = tester huoyan1231, one client slot 1 'gediao' joined 15:46:13, left 16:24:42).
Second reporter SirWilliam = a DIFFERENT session, NO log, build unknown (the b122+ equality gate only
guarantees his host+client shared ONE build, not which).

## Per-bug binding (testers' own words -> root row)

| # | Tester symptom | Root row | Confidence |
|---|---|---|---|
| 1 | "frame rate drops to ~10 fps... only for the host" | R-I | measured symptoms; attribution partial |
| 2 | "friend moved a cabinet... didn't appear in my game initially, showed up after some time" | R-J | OPEN; R-B prime suspect |
| 3 | "[client] object touches the garbage -> duplicates endlessly" (SirWilliam) | R-D primary [provisional] + R-C contributory | assumes >= b122; needs his log |
| 4a | "my friend lost a hamburger" (client-side loss) | R-B | measured mechanism |
| 4b | "I lost a reel of magnetic tape delivered by drone" (host-side) | R-H | OPEN |
| 5a | "fell to their death... at that exact moment the game disconnected" | R-E | measured: designed behavior |
| 5b | "couldn't rejoin... have to close and reopen the game" | R-F | inferred; client-side; repro planned |
| 6 | "ESC -> Exit... TAB still shows your friend" (SirWilliam) | R-F | assumes >= b122; needs his log |

Found-unreported: R-G (join-tail destroy leak; one APPLIED kill measured), R-A(b) un-retrofitted
sharers, the stuck carry latch (R-D), a live host-side key-dupe pair ('0glxPzH2...' destroyed on TWO
different actors 16:19:03 + 16:20:01), SkyState join losses (self-healing periodic push — no action).

## The rows

### R-A [measured] ONE delivery-guarantee class: a reliable line silently dropped at send time
> **ALL THREE SHAPES FIXED 2026-08-23. (a)+(c): `coop/net/send_backlog` (linux-fps R-4b;
> design `research/findings/network/votv-reliable-delivery-guarantee-DESIGN-2026-08-23.md`).
> (b): the not-ready sharers got ready-edge seeds the same evening (`0676e5a8`, the shared
> `coop/session/join_seed` helper; drill-proven RED->GREEN; design
> `votv-signal-email-ready-seeds-DESIGN-2026-08-23.md`). NOT hands-on.**
Three shapes, one owner for the design pass:
- **(a) enqueue rejection deleted.** `session.cpp:176` sends with `bDeleteFailedMessages=true`; on
  rc=-25 (GNS enqueue refusal, send buffer full) the message is deleted and the false return is
  ignored by ~60+ call sites. ALL 164 in-log losses (PropSpawn x131 + SkyState x33) land in ONE
  second, 15:46:47, inside the join snapshot drain: drain = 100 props/tick
  (`prop_snapshot.cpp:92-96`, sized for CPU budget, blind to the link), pendRel plateau 455-515KB ~=
  the GNS 512KB default send buffer, right after a ~500KB save transfer. Loss is CONTIGUOUS in
  candidate order (once the buffer caps mid-tick the rest of the chunk rejects): the lost run is one
  band ~#600-#930 of the eid-ascending enumeration (`registry.cpp:287-296` iterates id 0..max).
  Steady state on this log is clean (zero rc warns outside 15:46) but any future >512KB burst
  re-opens the shape.
- **(b) not-ready skip = permanent loss.** Already codified (B2 seed-delta lesson); the two
  un-retrofitted sharers (signal_sync, email_sync) were seeded 2026-08-23 (`0676e5a8`).
- **(c) the 60+ rc-ignoring call sites** are the class's blast surface, not individual bugs.
Fix direction (later design pass): pace the drain against the send-buffer budget + absorb enqueue
rejections at the session layer (MTA packet-queue precedent); NOT per-kind retry patches.
**Instrument fix (cheap, do first): log eid+key on enqueue rejection** — the current warn carries no
payload identity, which is why the 131 lost props are unenumerable from this log.

### R-B [measured; census CLOSED] Client keyed-birth authority hole (v122 residual) — exactly TWO sites
`prop_container_extract.cpp` takeObj-POST (NO role gate) and `trash_collect_sync.cpp`
EnsureHeldItemBroadcast (~:397, role-unconditional) both stamp
`p.elementId = (eid==kInvalidId) ? 0 : eid`; on a client, `GetPropElementIdForActor` returns
kInvalidId for keyed props since v122 no-passive-mint -> elementId=0 -> the host's PropSpawn range
gate drops it ("out of allowed peer range" x41 in-log, correlated with client container/grab
activity). All other SendPropSpawn callers verified host-gated (host_spawn_watcher:241,
prop_snapshot:620, prop_lifecycle:184-212 client-skip; kerfur = own wire). Consequence: a
client-extracted item exists ONLY client-side (bug 4a, the hamburger); two carried-then-placed
episodes measured (see R-J); 251 keyed deferred destroys corroborate the client-born-keyed pool.
Fix direction: ONE host-authored birth route (extract/held intent, symmetric with PropDropIntent)
covering both sites. A client-side suppress gate was REJECTED as a RULE-1 crutch (it would hardcode
bug 4: the item would never exist cross-peer).

### R-C [mechanism measured; client outcome UNMEASURED] Trash clone-family key divergence
Measured chain: VOTV's save ships duplicate interactable keys (known lesson); at world load 15:44:58
the host re-keyed 81 trashBitsPile_C (one shared key) to `rk_%016llx` minted from
random_device+steady_clock (`prop_synth_key.cpp:123-131`) — **non-derivable by construction**, a
peer can never independently converge on them. No save happened at join ([SAVED-DETECT] 0/0) -> the
transferred save carries the OLD duplicate keys -> the join-drain PropSpawns are the SOLE carrier of
the rk_* keys (the save-time match-key bridge rides INSIDE the same payloads, v86 Path 1c) — and
that carrier lost a contiguous eid band the family (minted contiguously mid-walk) plausibly
occupies. UNMEASURED from the host log: whether the family's eid band overlaps the lost band, and
the client-side adoption outcome. Falsifier: rk_* keys present in gediao's client log -> the
divergence story dies, bug 3 re-opens as pure R-D. ZERO rk_* keys ever appeared in client-streamed
lines (necessary but not sufficient for divergence).
Design-pass choice: deterministic disambiguator (derive the re-key from save key + stable ordinal so
both peers mint IDENTICAL keys, no wire dependence) vs guaranteed delivery of host-minted keys.

### R-D [measured symptom; PROVISIONAL binding, flip criterion stated] The 1 Hz trash loops + keyless parallel universe
- Grab-certificate loop: "clump BORN from pile eid=N (grab certificate; identity MIGRATED at birth;
  held-edge consumes)" -> RE-PILE convert -> ToPile broadcast -> "certificate expired, never reached
  the hand, unowned=1" -> repeat at 1 Hz (x16 on eid=2172 @15:55; x30 on eid=3131 @16:03).
- Stuck TRASH-CARRY FLIGHT latch: eid=2171 published identical coords 1 Hz for ~30 min, cleared only
  by the disconnect ForgetEid. Latch was client-intent-authored ([GRAB-INTENT] OnGrabHolderLeft).
- The keyless parallel universe: 1726 deferred destroys with empty key + client-band eids (>=40960)
  = the client's local keyless trash sim the host NEVER learned; 'None' x37 keyless pose no-matches.
Bug 3 (garbage duplicates endlessly) binds here PRIMARY, provisionally. **Flip criterion for the
pending code read (trash_collect_sync + puppet_carry_drive):** if the certificate re-arm is (i)
host-local polling -> distinct root, binding stands; (ii) driven by incoming client intent packets
-> R-D is DOWNSTREAM of R-B/R-C and the primary binding flips. The user's 9-line ROCK-DROP
diagnostic WIP already probes this area. [SirWilliam row: same-class candidate, unconfirmed — his
session, his build.]

### R-E [measured: the design IS the bug] Death = designed session eject
Death policy (2026-06-01 client-death OOM fix, `net_pump.cpp:619-663`): on local death the client
synchronously tears down coop state, Session::Stop(), FleeToMainMenu with the transparent
ProcessEvent bypass (the world-teardown hang/OOM cure). Log: 16:24:40 ragdoll -> 16:24:42 peer
closed reason='session stop'. Working exactly as coded; the testers correctly experience it as a
bug. Fix = in-session death/respawn handling — its own design pass (/qf), principle-8 pressure.

### R-F [inferred; client-side; repro planned] Rejoin + ESC-Exit lifecycle
Host log shows ZERO redial after the 16:24:42 leave — the client never attempted to reconnect. Both
testers independently report rejoin requires a full game relaunch; SirWilliam additionally reports
ESC->Exit leaves the session live (TAB list still shows the friend), which would ALSO explain rejoin
refusal (Start while running_). Candidates: g_fleeing latch not reset at menu, Session::Start
refusal state, WSACleanup in ~SignalingClient, the native-quit predicate missing the pause-menu Exit
path. Needed: mp.py 2-peer repro (join -> die/quit-to-menu -> rejoin attempt) + gediao/SirWilliam
client logs. The repro decides latch-vs-predicate.

### R-G [measured] Join-tail destroy leak lands ~1s POST-ClientWorldReady
World-ready 15:46:46; the client destroy flood lands 15:46:47: mass deferrals + ONE APPLIED kill —
key 'drone_InventoryContainer' destroyed the host's LOCAL actor. The quiescence predicate runs
client-side, so the host log cannot distinguish "predicate blind to its own destroy churn" from
"churn genuinely post-dates quiescence" — both name the SAME fix locus: the client's announce
predicate / the client's outbound destroy seam during its load window (per the existing lesson:
move/extend the EXISTING anchor, no second compensation layer). Flagged design question: should a
client's load-window native destroys — and its local keyless-universe churn (the 1726) — broadcast
at all.

### R-H [OPEN] Host-side reel loss UNASSIGNED
Drone delivery WORKED host-side (16:04:49 spawned reelbox+reel_small+reel_big — after R-G's
container kill at join, notably); reel_small last seen 16:10:57 in a normal host grab cycle; no
destroy after. The loss is not visible in this log (possibly another day/session). Candidates: the
client key-routed destroy batch (50 applied total; 12-in-40s at 16:19), native loss (fell through
world). Needed: tester's when/where; gediao's log. The measured host key-dupe pair ('0glxPzH2' x2
actors) proves live key-collision destroys are possible.

### R-I [measured symptoms; attribution partial] Host FPS storms
212 hitch frames >40ms; storm minutes 15:57 (2.5s total stall), 15:59 (4.3s), 16:05 (3.9s), 16:06
(8.6s, max single frame 1372ms) — matches "drops to ~10fps at times". Only NAMED source:
net_pump::Tick (10-35ms bursts during storms; 1367ms once at world load); the remainder is
engine-side by the probe's design. Steady tax measured: sync:event_cue 4.2ms avg x2293 (~1/s),
sync:interactable 9.6ms avg (max 158ms), reseed:KnownKeyedProps 16.3ms/20s (max 57ms), RebuildIndex
family ~6ms each; NumObjects ~255k; private 4.8GB. Storms correlate with exactly the R-B/R-C/R-D/R-G
anomalous traffic (container extract + deferred-destroy floods + grab/release bursts). Plan:
fix R-A..R-D first, re-profile; a dedicated profiling pass only if storms survive.

### R-J [OPEN] The cabinet (bug 2)
Prime suspect = an R-B expression: TWO measured carried-then-placed episodes where the client
streamed a key the host never knew and RELEASED it at ~zero velocity ('fvdcsF3K1LBe_8xEc5Lg4w' x30
@16:12; 'VV5UcYyVYHc8g9mF7CcQsg' x6 @16:17:58) — a client-authored container carried and placed,
invisible to the host. NO heal lane observed in-log for either key (zero adopt/bind lines), so the
tester's "showed up after some time" mechanism is UNKNOWN. Needed: gediao's log + tester's
when/where. Falsifier: the cabinet appears in the client log as a HOST-keyed prop pose-streamed
normally -> R-B excluded, re-open as a pose-lane bug.

## Priority proposal (design passes, each its own /qf)
1. **R-B** client-birth authority route (kills bug 4a + the R-J suspect + shrinks the deferred-destroy mass)
2. **R-A** the delivery-guarantee class (drain pacing + session absorption + the b2-sharers retrofit; do the instrument fix immediately)
3. **R-C/R-D** trash identity + grab handshake (ONE pass; starts with the R-D code read + flip criterion)
4. **R-F** mp.py lifecycle repro (rejoin/Exit) — cheap, high tester-pain
5. **R-G** join-tail destroy anchoring (+ the 'should local churn broadcast' design question)
6. **R-E** death/respawn design pass (big; principle 8)
7. **R-I** re-profile after 1-3
8. **R-H/R-J** close with tester follow-up + client logs

## Needs from outside (explicit)
- ~~gediao's client log; SirWilliam's client log + his build number~~ **UNOBTAINABLE (user,
  2026-07-26: "Больше клиентских логов не получить")** — every "needs client log" row resolves
  instead via the mp.py 2-peer repro (the client side of the rig IS a client log source) + the
  named instrument fixes (eid+key on enqueue rejection; adopt-line logging).
- Tester answers: when/where the cabinet appeared late; when the reel was noticed missing
- mp.py 2-peer repro run for R-F (and for R-B/R-C/R-D client-side outcomes)
- Code read for R-D (flip criterion in-row)

## /qf ledger (8 rounds, fresh critic each; "that holds" at R8)
R1 instrument gap; rk_-client-mint refuted by grep; reel split out; latch honesty. R2 census -> one
session-layer owner; suppress-gate rejected (RULE-1); R-G anchor measured post-ready; cross-session
rows tagged. R3 R-J own row; re-key randomness measured; burst-vs-buffer arithmetic; two-site
census. R4 losses grep-confined to the join minute; 1832 deferred destroys re-attributed by identity
shape (1726 keyless client-band -> R-C/R-D, not R-B); B2 lesson unified into R-A. R5 R-C membership
unpinnable -> DOWNGRADED, the PILE-1C bridge surfaced; SkyState measured self-healing. R6 contiguity
mechanism; bridge asymmetry resolved (bridge rides the dropped payloads); R-D flip criterion
pre-named; build assumption explicit. R7 eid-order measured (registry.cpp:291); loss band bracketed
#600-#930 by chunk lines; per-row falsifiers committed. R8 "that holds".
