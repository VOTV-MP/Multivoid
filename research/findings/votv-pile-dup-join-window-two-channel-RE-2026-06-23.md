# chipPile JOIN-WINDOW DUP — the two-channel race (DECISIVE, hands-on) — 2026-06-23

**Status: ROOT CONFIRMED by a controlled hands-on. Fix DESIGNED, NOT built (gated on 2 code checks).**
This supersedes the entire "L1 orphan / absence-removal at the join" line of work from earlier this
session — that was chasing a class of bug that **does not exist** (see "What is now FALSE" below).

## The decisive hands-on (user, 2026-06-23)
Setup: 6 items on a path — **2 kerfur + 4 chipPile** — host saves. Then, **in the join window** (the
client has received the scratch save but is NOT 100% loaded yet), the host drops/moves the items onto
the asphalt. The client then renders:
- **2 kerfur @ path** (old/save position) — NO dup.
- **4 chipPile @ path** (old, from the save) **AND 4 chipPile @ asphalt** (new, from the broadcast) =
  **8 piles instead of 4 — a DUP.**

## Root cause: chipPile has TWO identity channels; kerfur has ONE
- **chipPile travels by TWO channels:** (1) the **scratch save** — the client loads the host's save, so
  the pile exists as a CLIENT-LOCAL NATIVE `actorChipPile_C` at the SAVE position; (2) the **broadcast
  convert** — the host's live grab/move broadcasts a `PropConvert` (ToClump/ToPile) that materializes a
  host-eid **PROXY** at the NEW position. In the join window these two channels DIVERGE (the save is the
  old position, the broadcast is the new one).
- **kerfur travels by ONE channel** (the save only — a plain prop in the save, no separate convert-
  broadcast double path). So a host kerfur move in the window does not produce a second kerfur. **Kerfur
  is the clean CONTROL that proves the two-channel pile path is the cause.**

## Why the existing 1cm dedup is blind to it
The join reconcile de-dups a client native against a host proxy by **POSITION (1cm)** — `kDestroyR2Cm`
in `remote_prop_spawn.cpp` (the destroy loop, ~line 405-425). For an UNMOVED pile the save-native and the
proxy are co-located → matched → the native destroyed → one mirror. But for a pile MOVED IN THE WINDOW,
the save-native (old pos) and the broadcast-proxy (new pos) are **far apart (>1cm)** → the position dedup
**cannot match them** → both survive → **two distinct objects = a persistent dup.** This is the SAME
">1cm → dedup blind" failure mode the session kept circling, but **created by the timing window, not by
host-drift** (host-drift can't create it — see below).

## The dup is PERSISTENT, NOT self-resolving (correction)
- **kerfur self-heals:** a kerfur with a wrong position is ONE object; a client interaction re-syncs its
  position (host teleports it to the right spot) → converges.
- **a pile dup does NOT self-heal:** it is **TWO DISTINCT objects** on the client (the scratch-save
  native + the broadcast proxy). Interacting with ONE (e.g. grabbing the proxy) does not touch the OTHER
  (the native stays). The earlier-session belief that "the pile dup self-corrects" was WRONG — grabbing a
  dup pile only *removed the one you grabbed*; the other stayed. **Severity is NOT reduced — it is a
  persistent extra world object, not a cosmetic phantom. The fix is required.**

## The dup is CLIENT-LOCAL — the host state is CORRECT (lowers risk + fixes the locus)
The dup does NOT leak to the host. The host sees the correct SINGLE state (one pile at the new position).
The extra object exists ONLY on the client, born from its TWO inbound channels (scratch-save native +
broadcast proxy) that it assembles independently. Consequences: (1) host authority is intact — no
host-side corruption, lower risk; (2) **the fix is purely CLIENT-side channel reconciliation** (the client
must collapse its two inbound representations of one pile), NOT anything host-side. This rules out fix
variants that touch host state and points squarely at the client's apply path (the OnConvert / save-load /
dedup ordering on the client).

## What is now FALSE (RULE 2 — retire it)
- **"Pre-connect host-drift creates orphans" is FALSE.** The join is a SAVE-TRANSFER join: the host takes
  a FRESH scratch save AT connect (`save_transfer.cpp` stamps a captured blob at connect; the user
  OBSERVED the save fire at connect). A fresh save captures the host's CURRENT state, so ANYTHING the host
  did BEFORE the save is IN the save → the client loads it → host==client → **no pre-connect divergence
  exists as a class.** Divergence is ONLY possible from a host change AFTER the save snapshot.
- **Therefore the "[PILE-CENSUS] orphan census + absence-removal AT THE JOIN" line is the WRONG fix and is
  CANCELLED.** It searched for join-time orphans that the save-transfer makes impossible — which is why
  every census reported `0 live orphans (totalLiveNatives=870 proxyMatched<=1cm=870)` on every clean run.
  That 0 was always CORRECT (nothing to find), not a metric bug (though 3 real census bugs WERE found +
  fixed along the way — silent-on-empty `a54fbd22`, stale-index→fresh-walk `ff5cdac6`, the totalLive
  diagnostic `ac13394d`; the census + `coop::util::IncrementalObjectScan` + the host-drift scenario remain
  as diagnostic infra but the orphan-at-join premise is dead).
- The autonomous **same-machine identical-save drift smoke cannot reproduce the real bug** — the bug needs
  the JOIN-LOAD WINDOW + a host move in it, which the drift scenario (pre-connect) structurally can't make.

## The fix — DESIGNED, 2 options, NOT built (gated on 2 code checks)
The dup = one pile expressed as TWO objects (scratch-native@old + broadcast-proxy@new) that the
position-dedup can't reconcile because they diverged in the window. The clean fixes:
- **(a) HOLD the broadcast convert until the client signals load-100%** (queue host pile-converts during
  the join-load window; flush in-order after load-complete). Then the client loads the save FIRST, then
  applies the convert IN ORDER → no divergence → the existing path re-skins the existing mirror. Cleanest
  — kills the race at the source (MTA push-in-order). **GATE: does the broadcast convert actually leave
  the host BEFORE the client is load-complete, and is there a load-complete signal to queue against?**
- **(b) dedup MATCH-BY-EID instead of 1cm position** — if the scratch-save native and the broadcast proxy
  of one pile share an eid, reconcile by eid (distance-independent). **GATE: do the client's save-loaded
  native and the host-eid proxy share an eid?** (Likely NOT — the save-native is keyless/lazily-eid'd with
  a CLIENT-minted eid, the proxy carries the HOST eid; if so, (a) is the path, or (c) a post-load
  reconcile that links them by the pile's stable level-identity, not distance.)

## NEXT (build, after the 2 gate checks)
1. Trace the save-transfer + join-load timeline: when is the scratch save taken, when does the client
   signal load-complete, when does a host grab/move broadcast its convert relative to that — confirm the
   broadcast lands during the window (`save_transfer.cpp`, `event_feed` SnapshotBegin/Complete,
   `remote_prop::OnConvert` spawn-fresh-for-unknown-eid path).
2. Confirm the eid (non-)link between the save-native and the proxy (`prop_element_tracker` lazy eid mint
   vs the host eid on the proxy).
3. Build (a) if queueable (preferred), else (b)/(c). Then re-run the user's controlled repro (drop in the
   window) — dup must become 0.

## Controlled re-test protocol the user proposed (if the trigger needs further isolation)
A/B/C/D over {1st vs 2nd connection} x {move IN-window vs move AFTER load-complete}, to split the
timing-window trigger from any reconnect-state factor. The decisive hands-on already PROVED the window is
necessary (move-in-window → dup) and two-channel pile is the cause (kerfur control clean); the A/B/C/D
grid remains only if a reconnect-number co-factor is later suspected. Requires a visible "client
load-100%" marker so the user can reliably tell in-window from after-window (NOT YET added — a candidate
next-build item: log/marker on the client's load-complete).
