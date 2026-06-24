# 05 — REVERSE symptom: client GHOST active kerfur (follow) — ROOT FOUND + FIX BUILT

**STATUS 2026-06-24: ROOT FOUND + FIX BUILT+DEPLOYED (MD5 `239b231c`), hands-on verify PENDING.**

## ROOT (CONFIRMED by 3-session timing compare) — a JOIN-TIME DUPLICATE + a one-shot sweep that fires too early

The ghost is **NOT** a turn_off-retire bug (that whole `R-client-retire` line below is a RED HERRING — see the
correction). The real root, proven by comparing three real sessions:

- The client's async live-save load spawns a **DUPLICATE active kerfur**: the real save-kerfur is adopted as the
  host mirror; a **second untracked `kerfurOmega_C` twin** materializes SECONDS later in the load tail.
- `npc_adoption`'s **one-shot ghost sweep** (`DestroyUntrackedClientNpcs`, npc_adoption.cpp Tick) is the safety
  net that kills the untracked twin. It was gated only on `g_snapshotDelivered && g_pending.empty()` -- **NOT on
  `HasLoadTailQuiesced()`**, unlike the fresh-spawn path right above it and the prop divergence sweep.
- So it can fire the instant `g_pending` drains, **before the late twin spawns**, find 0, latch `g_ghostSwept`,
  and never run again -> the twin survives as a **follow-ghost active kerfur**.

**3-session evidence:**
| session | ghost sweep fired | load tail quiesced | twin | outcome |
|---|---|---|---|---|
| 12:30 (GHOST) | 12:30:17, `0 orphans` | 12:30:**23** (6s LATER) | spawned after the sweep | survived = follow-ghost |
| 13:04 (clean) | 13:04:54, `1 orphan destroyed` | 13:04:56 | present by sweep time | destroyed |
| 12:58 (clean) | 2 sweeps, destroyed | -- | present | destroyed |

The turn_off is incidental: in 12:30 the host turn_off converted the *tracked* kerfur to a prop, leaving the
sweep-missed untracked twin as the lone visible active kerfur. **The ghost can occur with NO turn_off at all**
(pure join-time duplicate + sweep miss). This is the SAME snapshot-before-state-ready class as the L1 pile dup
([[feedback-snapshot-before-state-ready]]).

## FIX (AS-BUILT 2026-06-24, MD5 `239b231c`, deployed host+client+copy2+dev)

One condition added to the ghost-sweep gate (npc_adoption.cpp Tick): also require
`coop::remote_prop_spawn::HasLoadTailQuiesced()` -- the SAME load-tail quiescence signal the `ResolvePending`
fresh-spawn (same file) and the prop divergence sweep already use. Now the one-shot sweep waits until every late
twin is present before it runs + latches; deadline-capped in `remote_prop_spawn` so it can't hang; re-armed per
`OnClientWorldReady` (`g_ghostSwept=false`) for the 2-level-load. Minimal, reuses a proven pattern, no new state.

**Hands-on verify (PENDING):** in-window turn_off of an active+idle kerfur -> client shows NO follow-ghost
(host object only); grep the client log for `post-snapshot ghost sweep complete (N untracked orphan(s)
destroyed; adoption converged + load tail quiesced)` with the sweep firing AFTER load-tail quiescence.

---
## (SUPERSEDED) earlier R-client-retire investigation -- kept for the audit trail; the turn_off retire was a red herring

**Status (historical): REPRODUCED 2026-06-24; the root hunt below chased the turn_off retire before the
3-session timing compare revealed the join-time duplicate above.**

## The reproduced symptom (mirror of doc 02)

- **Save state:** kerfurs are **ACTIVE (NPC)** at the blob instant (the transferred save IS the live host world
  serialized then; a turned-ON kerfur round-trips as an ON NPC -- RCA line 126,
  `votv-kerfur-savetransfer-ghost-prop-RCA-2026-06-15.md`).
- **Host action in the join window:** host **turns the kerfurs OFF** (active->object) while the client is still
  loading.
- **Result:** host has the **object** (prop) lying on the ground; the **client stays stuck on the ACTIVE NPC**
  (a ghost). The turn_off conversion did not converge on the client.

This is the MIRROR of doc 02's off-prop case:
| | doc 02 (off-prop-at-blob) | doc 05 (active-at-blob, THIS) |
|---|---|---|
| save form | OFF prop | ACTIVE NPC |
| host window action | turn_ON (object->active) | turn_OFF (active->object) |
| client save-native | off-prop native (prop channel) | active-NPC native (npc channel) |
| symptom | dup object+active / camera / collision | **ghost ACTIVE NPC on client, object on host** |

## NEW critical detail (user, 2026-06-24): the client ghost is in FOLLOW mode, NOT at save position

The user confirmed the symptom precisely: host turned ONE active kerfur OFF in the join window -> host gets an
OBJECT on the ground; the client does NOT see that object and instead has a **ghost kerfur in FOLLOW mode walking
behind the player** (not standing where it was). The user's mechanism note: a save kerfur that was active AND
**IDLE** may, on save load, undergo an **idle -> FOLLOW mode transition** and possibly **spawn NEAR the local
player, NOT at its save-time position**.

**Why this is decisive for one-fix-vs-two:** the off-prop fix (doc 03) keys identity by the **save-time
POSITION** -- valid because an off-prop sits AT its save position. But a **follow-ghost active NPC is NOT at its
save position** (it walked to the player on the idle->follow transition). So a save-time-position key may MISS
it -> the doc-03 `P_live` symmetry likely does NOT carry to the reverse as-is. This **leans toward a DIFFERENT
mechanism** (npc_adoption load + idle->follow mode-transition + spawn-near-player), not a mirror of the off-prop
case. **But do NOT assume** -- a fresh reverse-case log decides. (If the position-key really misses, the reverse
fix needs a NON-position cross-peer anchor for the active form, or it must retire by the npc mirror's host eid
rather than by position -- a different design than scope A.)

## Why this might be the SAME root or a DIFFERENT one (do NOT assume) -- expanded fork (3 roots, was R1/R2)

The off-prop fix (doc 03, save-time-exact-position key on the PROP channel) does NOT automatically cover this --
the reverse rides the **npc channel** (the client's ghost is an ACTIVE NPC save-native, retired via the npc path,
not the prop divergence sweep), AND the follow-mode/position detail above may break the position key. THREE
candidate roots, to be split by a fresh reverse-case log:

### Candidate R-host (turn_off in the window never broadcasts the conversion to the client)
The host went to object but the client was never told. Either the death-watch poll
([kerfur_convert.cpp:612](../../src/votv-coop/src/coop/kerfur_convert.cpp#L612), `POLL turn_off`) did not fire for
the active-at-blob kerfur in the window, or `BindFormActor ->prop(turn_off)` / the `KerfurConvert` broadcast was
gated during the join. If R-host, the fix is host-side (detect + broadcast the window turn_off); the position key
is irrelevant. **Log tell:** host has NO `POLL turn_off` / `BindFormActor ->prop` line despite the object on the
ground.

### Candidate R-client-mode (idle->follow transition moves the ghost off save-pos; position key misses)
The client loads the save NPC, the idle->follow mode-transition walks it to the player (spawn-near-player, NOT
save-pos), so even if the `KerfurConvert` arrives, an exact-save-position retire can't find it -> ghost persists,
now in follow. If R-client-mode, the doc-03 `P_live` position key is INSUFFICIENT for the active form -> the
reverse needs a different anchor (npc mirror host-eid, or suppress the idle->follow transition for an
about-to-convert kerfur). **Log tell:** client logs an idle->follow mode change + a spawn position that differs
from the save position; the convert arrives but no retire matches.

### Candidate R-client-retire (convert arrives but the npc-mirror retire fails)
The client receives `KerfurConvert(NPC->prop)`, `MaterializeKerfurMirror(toNpc=false)` spawns the prop, but the
OLD-form NPC retire by `oldEid` finds no live mirror (the save-native was adopted under a different eid, or not
yet adopted) -> the active NPC persists as a ghost beside the new object. This is the closest to the original R1
(SAME window-race family); **here the `P_live` hook could still help IF the ghost is at save-pos -- but the
follow-mode detail suggests it may not be.** **Log tell:** client `applied KerfurConvert ... ->prop` present, but
no destroy of the old NPC mirror / a leftover `kerfurOmega_C` mirror at the end.

### (superseded) earlier R1/R2 framing
The original R1 (npc-channel retire-by-identity) + R2 (delivery/gating) collapse into the three above; R-host =
old R2, R-client-retire = old R1, R-client-mode is NEW (from the follow-mode detail). Kept for continuity:
The `KerfurConvert(NPC->prop)` broadcast itself never reaching/applying on the client in the window (reliable
channel gated during load, `OnKerfurConvert` early-returns pre-world-ready, or the host's death-watch poll
doesn't fire the convert for an active-at-blob kerfur). Then it is a delivery/gating break, not an identity
mismatch -- a different fix.

## The reverse-case LOG decision tree (diagnosis-first; splits the THREE roots above)
Read host + client logs of a FRESH reverse repro; the pattern names the root:
1. **Host `kerfur_entity: BindFormActor ... ->prop(turn_off)` / `POLL turn_off (kerfur NPC eid=N died)`** -- did
   the host detect the window turn_off + broadcast it?  NO (object on ground but no such line) -> **R-host**
   (host-side: poll/converge/broadcast missed the active-at-blob window turn_off).  YES -> continue.
2. **Client `applied KerfurConvert ... ->prop oldEid=N`** -- did the client receive+apply the convert?  NO ->
   delivery/gating (a host-broadcast-arrived-but-not-applied variant of R-host).  YES -> continue.
3. **Client old-form retire + the GHOST POSITION + MODE:** did the client destroy the live `kerfurOmega_C` mirror?
   - If a leftover `kerfurOmega_C` mirror remains AND the client logged an **idle->follow mode change + a spawn
     position != the save position** -> **R-client-mode** (the ghost walked off save-pos; the doc-03 save-time
     POSITION key MISSES it -> scope-A symmetry does NOT carry; needs a non-position anchor).
   - If a leftover mirror remains but the ghost IS at save-pos / not in follow -> **R-client-retire** (retire by
     `oldEid` missed; the `P_live` position retire could still fix it).
**Capture tool:** `tools/capture-kerfur-repro.ps1` -- run it AT the symptom (object on host / follow-ghost on
client), BEFORE any relaunch. It copies both peers' live `votv-coop.log` to `research/kerfur_repro_<ts>/` and
prints the turn_off/KerfurConvert/follow/idle lines so the split is immediate.

## LOG GREP 2026-06-24 -- the captured session was the WRONG (forward) run; reverse log was overwritten

Grepped EVERY votv-coop log across all 4 game folders, both peers. **ZERO turn_off events anywhere** -- the
signature (`POLL turn_off (kerfur NPC eid=N died invisibly)`,
[kerfur_convert.cpp:612](../../src/votv-coop/src/coop/kerfur_convert.cpp#L612); `BindFormActor ... ->prop(turn_off)`)
is in none. The user CONFIRMED (2026-06-24) the turn_off really fired (host got an object, client got a
follow-ghost), so the captured live 12:02 session is simply the WRONG run: `votv-coop.log` truncates on launch,
the user relaunched, and the forward session clobbered the reverse one (the `.prev` rotations hold only stale
06-14 / 06-22 sessions). The 12:02 session is a post-window **turn-ON** (5274->5277) that applied cleanly -- not
the reverse.

**Two facts the forward log DOES establish (still inform the design):**
1. **Active-at-blob kerfurs serialize as ON NPCs and adopt CLEANLY via npc-adopt at join** (client 12:01:15:
   `npc-adopt: bound LOCAL save NPC ... as host mirror eid=5275, class-match, NO duplicate spawn`). At JOIN there
   is no ghost -- the reverse symptom is specifically the host turn_off DURING the window on the npc channel.
2. **The KerfurConvert apply path WORKS when delivered** (forward 12:02:06 retire+materialize clean). So a reverse
   failure is NOT a broken apply primitive -- it is R-host (not broadcast), R-client-mode (position key misses the
   follow-ghost), or R-client-retire (retire-by-oldEid misses).

**VERDICT: cannot split the root -- no log on disk contains the reverse turn_off event.** Diagnosis-first: do NOT
guess. A FRESH reverse repro must be captured with `tools/capture-kerfur-repro.ps1` at the symptom moment (before
relaunch). Then the decision tree splits R-host / R-client-mode / R-client-retire in one read.

## AS-DIAGNOSED 2026-06-24 (FRESH reverse log, 12:30 session -- user-confirmed symptom: client active kerfur / host object)

A real turn_off IS in this log (host `HOST executing turn_off eid=5273` -> `npc-sync[silent release]: Npc eid=5273
released (no EntityDestroy broadcast)` -> `BindFormActor ->prop(turn_off) oldEid=5273 -> newEid=5275 (KerfurConvert
broadcast)`; client `applied KerfurConvert ... ->prop`). The three-root split resolves:

- **R-host: RULED OUT.** The host detected the turn_off, released the Npc, AND broadcast the convert; the client
  received + applied it. Not a host-side broadcast gap.
- **R-client-mode (position-key miss): RULED OUT as the ROOT.** The convert prop spawned at the kerfur's own spot
  (1727.6,-567.2,6206.7); the client player/puppet was at ~(1300,-526) -- NOT co-located. The position key is
  **never even reached** -- the retire short-circuits on a liveness check before any position match. The
  follow-walk the user sees is the orphaned NPC reverting to its LOCAL AI *after* the failed retire, not a
  save-load idle->follow spawn-displacement. So save-pos-vs-player is moot for the FIX.
- **R-client-retire: CONFIRMED (the root), two compounding code defects on the `kerfur_convert`/`npc_mirror`
  channel:**
  1. **The NPC retire SKIPS the authoritative destroy.** `npc-sync[client OnDestroy]` gates the destroy on
     `R::IsLive(actor)` ([npc_mirror.cpp:403-409](../../src/votv-coop/src/coop/npc_mirror.cpp#L403-L409)): if the
     actor reads not-live it logs `already not-live -- skipping K2_DestroyActor (engine destroyed it elsewhere)`
     and does NOTHING. For an ADOPTED mirror (the client's own real save-kerfur) the actor's cached
     InternalIdx/generation can go stale between adoption (12:30:20) and turn_off (12:30:40) -> `IsLive`
     false-negatives -> the authoritative destroy is skipped -> **the active save-kerfur is never destroyed**.
     Host stops driving it (silent release) -> it reverts to local follow-AI = the ghost the user sees.
  2. **The death-watch POLL cannot distinguish a HOST-release from a LOCAL conversion.** The poll
     ([kerfur_convert.cpp:592-626](../../src/votv-coop/src/coop/kerfur_convert.cpp#L592-L626)) fires for ANY kerfur
     NPC mirror whose actor reads not-live, INCLUDING host-owned adopted mirrors (`m_mirror`, host-driven). On
     this host-initiated turn_off it MISFIRED `POLL turn_off ... client requests host` + `ClaimConversionGhosts`
     (parked a spurious local prop). It has no `m_mirror`/owner gate -- a host-driven mirror's death is NEVER a
     local conversion, yet the poll treats it as one. (The existing guard at :575-589 only blocks the JOIN
     load-tail churn via `HasLoadTailQuiesced`; a post-quiescence host turn_off sails through.)

### VERDICT (the user's 4 questions)
1. **Root:** R-client-retire -- the convert's NPC retire short-circuits on a stale-liveness `!IsLive` skip
   (npc_mirror.cpp:407) leaving the active mirror orphaned, COMPOUNDED by the poll misfiring a local-conversion
   for a host-owned mirror (kerfur_convert.cpp:614-620). NOT R-host, NOT R-client-mode.
2. **Channel:** `kerfur_convert` + `npc_mirror` -- the SAME convert channel as off-prop (the convert arrives and
   applies; the retire WITHIN it is broken). NOT a separate `npc_adoption` retire path.
3. **Position / mode:** the ghost is NOT a save-pos-vs-player key problem -- the position key is never used; the
   retire short-circuits first. The follow behavior is the orphan reverting to local AI after the failed retire.
4. **One-build-vs-two + the active-side key:** **TWO builds** (off-prop scope A unchanged + a SEPARATE reverse
   fix), BUT the reverse needs **NO new cross-peer position anchor** -- the active form ALREADY has a working
   cross-peer key: the **host-range mirror EID** (`oldEid=5273`, carried on KerfurConvert). The reverse is NOT an
   identity-key problem at all; it is a retire-authority + poll-arbitration bug. So the reverse fix is SMALLER
   and channel-local, independent of the off-prop save-pos work:
   - **(fix a)** make the NPC->prop convert AUTHORITATIVELY retire the adopted mirror by its drained Element/eid
     -- destroy it unconditionally (or via a robust liveness, not the stale-cached `IsLive` short-circuit) so a
     false-negative can't strand it. The `OnKerfurConvert` apply OWNS the retire; do not delegate it to a racy
     liveness gate.
   - **(fix b)** gate the death-watch poll's local-conversion path on `!m_mirror` (host-owned mirrors excluded)
     -- a host-driven mirror's death is the host's authoritative convert, already handled by `OnKerfurConvert`;
     the poll must not treat it as a client-initiated local conversion (no redundant request, no spurious parked
     ghost).

**RESIDUAL (one open sub-point, does NOT change the verdict):** the exact reason `IsLive` read not-live for a
visibly-active actor (stale InternalIdx vs the adopted actor genuinely mid-destroy vs a second untracked
instance) is not 100% nailed from a log that ends 2 s after the turn_off. The fix above (own the retire by eid +
exclude host-mirrors from the poll) is correct regardless of which of those it is -- it removes the dependence on
the liveness read entirely. Confirm at build with a longer post-symptom capture.

## BUILD-PREP CORRECTION 2026-06-24 -- two code facts complicate fix(a) AND fix(b); a longer log is needed before building

Reading the retire/liveness/convert code to WRITE the fix surfaced two facts that make "build a+b now" premature
(diagnosis-first: do NOT ship two half-designed fixes):

**Complication 1 (fix a): `IsLive` reads the actor's OWN FRESH index, not a stale cached one.**
`R::IsLive(obj)` ([reflection.cpp:171-184](../../src/votv-coop/src/ue_wrap/reflection.cpp#L171-L184)) SEH-reads
`obj->InternalIndex` from the object's own memory, then `IsLiveByIndex`. So `IsLive(C395C0)=false` at the convert
means the engine reported C395C0 **genuinely not-live** (freed/faulted, or PendingKill/Unreachable, or its slot
recycled) -- it is NOT the stale-cached-index path (that is only `IsLiveByIndex(actor, el->GetInternalIdx())`,
used by the POLL). AND the client log shows **exactly ONE** active kerfur actor (C395C0, adopted 12:30:20; only
other mention is the 12:30:40 skip) -- **no second instance, no respawn, no fresh-spawn**. So two possibilities
the 2s-truncated log (ends 12:30:42) cannot separate:
- **B1:** C395C0 IS the ghost and `IsLive` FALSE-NEGATIVED it (PendingKill set transiently / slot recycled /
  fault on a still-rendering actor). Then fix(a) must KILL the live orphan -- but it must NOT blindly force
  `K2_DestroyActor` (a genuinely-freed pointer = use-after-free crash). It needs a robust re-validate-then-kill.
- **B2:** C395C0 genuinely died and the visible ghost is a SECOND untracked kerfur the truncated log never shows
  (a join-churn respawn / a dup the adoption missed). Then destroying C395C0 is irrelevant -- the bug is an
  adoption/dup, a different fix entirely.
The 2s log does not disambiguate B1 vs B2, and the fix(a) design DIFFERS fundamentally between them. Per
diagnosis-first: do NOT build fix(a) until a longer log says which.

**Complication 2 (fix b): `IsMirror` does NOT distinguish host-driven from client-initiated turn_off.**
On a CLIENT every kerfur is a host-owned mirror (`m_mirror=true`, incl. the adopted save-kerfur). The
client-initiated turn_off (legitimate, must request the host) ALSO goes through this SAME poll on a mirror
(the menu interceptor is PE-invisible -- kerfur_convert.cpp:353-356). So gating the poll on `!IsMirror()` would
**break the legitimate client-initiated turn_off** (acceptance #3 regression). fix(b) needs a real INITIATOR
signal -- e.g. "a KerfurConvert for this eid just arrived/is arriving from the host" (host-driven -> poll skips)
vs "the local player pressed turn_off on this kerfur" (client-driven -> poll fires). That is a different,
careful design than the one-line `!m_mirror` gate first sketched.

**VERDICT UNCHANGED, fix DESIGN gated on a longer log:** R-host OUT, R-client-mode OUT, channel = kerfur_convert,
the poll double-handles, and no new identity key is needed -- all still hold. What is NOT yet buildable is the
PRECISE fix(a) (B1 vs B2) and fix(b) (the initiator distinguisher). Need ONE more capture: reproduce the reverse,
then **STAY CONNECTED ~30 s with the ghost visible**, THEN run `tools/capture-kerfur-repro.ps1`. That log shows:
is C395C0 referenced/alive after 40s, does a 2nd kerfurOmega_C actor appear, what drives the ghost's
follow-pose. That picks B1 vs B2 and reveals the initiator signal for fix(b). THEN build.

## NEXT (no build, no root-split until a fresh reverse log is on disk)
1. User re-runs the reverse repro: a save with an active+IDLE kerfur -> host turn_off it during the client's join
   window -> observe object-on-host + follow-ghost-on-client.
2. AT the symptom (before relaunch), user runs `pwsh -File tools/capture-kerfur-repro.ps1 -Tag reverse_turnoff`
   -> both logs land in `research/kerfur_repro_<ts>_reverse_turnoff/`.
3. Claude greps -> splits R-host / R-client-mode / R-client-retire -> THEN decides one-fix-extends-off-prop
   (only if the ghost is at save-pos, R-client-retire) vs a separate npc-channel reverse fix (R-host or
   R-client-mode -- the follow-mode/position detail makes this the likelier branch, but the log decides).
Related: doc 02 (off-prop symptoms), doc 03 (off-prop fix design + the `P_live` hook),
`docs/COOP_MIRROR_IDENTITY_WINDOW_RACE.md` (the class), [[feedback-snapshot-before-state-ready]].
