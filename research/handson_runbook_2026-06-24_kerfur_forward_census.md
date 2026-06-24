# Hands-on runbook — kerfur forward off->active dup: CENSUS PROBE (names the silent 7th)

**Deployed:** MD5 `4A3112CFF0E344129618D235A399AEAA` (short `4A3112CF`) on HOST + CLIENT + CLIENT2 + DEV.
Build clean. Proto unchanged. **Diagnostic only — no behaviour change** (reads + logs at quiescence, never
mutates/destroys). Code HELD uncommitted (it's a probe; commit decision after it names the root).

## What this build adds
A one-shot CLIENT-side **kerfur census** (`coop/dev/kerfur_census.cpp`) that fires ONCE at load-tail
quiescence, AFTER both the divergence sweep and the ghost sweep. It walks the GUObjectArray and logs EVERY
live kerfur form — active NPC (`kerfurOmega_C`) and off prop (`prop_kerfurOmega_C`) — with eid / key /
position / **bound-vs-unbound** status. This catches the "silent" 7th actor (a save-loaded form that no
bind/spawn/sweep line names) that the 15:11 log could not show.

## Setup — the SAME combined test that produced 6-vs-7 at 15:11
- Same 6-kerfur save: **4 off + 2 active**.
- Client connects; **in the join window the host SIMULTANEOUSLY turns one OFF kerfur ON and one ACTIVE kerfur
  OFF**.
- Host ends at 6 (4 off + 2 active). The bug: client showed 7 (the off->active one as active + object).
- **Use a CLEAN bracket** (so the sweeps run — confirm `claim tracking ARMED` + `divergence sweep FIRING`
  appear, NOT the 14:42 `applied 0/0` SnapshotBegin-lost flake). If the bracket doesn't arm, reconnect.

## What to read in the CLIENT log
Grep `[KERFUR CENSUS]`. At quiescence you get a block:
```
[KERFUR CENSUS] client load tail quiesced -- enumerating every live kerfur form ...
[KERFUR CENSUS]   NPC  actor=... class='kerfurOmega_C' pos=(...) BOUND eid=3145
[KERFUR CENSUS]   NPC  actor=... class='kerfurOmega_C' pos=(...) BOUND eid=3147
[KERFUR CENSUS]   PROP actor=... class='prop_kerfurOmega_C' key='gPXK...' pos=(...) BOUND eid=4346
   ... (the 4 off + 2 active = 6 bound) ...
[KERFUR CENSUS]   <one extra line with *** UNCLAIMED *** or *** UNTRACKED *** = the silent 7th>
[KERFUR CENSUS] TOTAL N live NPC (X UNTRACKED) + M live PROP (Y UNCLAIMED). ...
```

## The decision (the census names the root)
- **An `*** UNCLAIMED ***` PROP line** (a live `prop_kerfurOmega_C` with NO host mirror) = the client's stale
  local OFF-prop for the turned-ON kerfur survived (the host has it ACTIVE, never sent it as off, nothing
  claimed it, the sweep didn't doom it). -> **identity-key root -> scope A** (doc 03 save-time exact key);
  forward-dup = the **third mirror-identity instance** -> generalize the mirror-identity layer.
- **An `*** UNTRACKED ***` NPC line** (a live `kerfurOmega_C` with NO host mirror) = a stale local ACTIVE twin
  the ghost sweep missed. -> **retire-side root -> own retire-fix** mirroring the reverse follow-ghost;
  forward-dup = a **retire-pair with the reverse** -> generalize the retire-authority layer.
- Note the eid/key/pos on the offending line + WHY it wasn't reconciled (unclaimed vs claimed; not-tracked vs
  keyed). The TOTAL line gives the counts at a glance: host is 6, so any UNTRACKED-NPC or UNCLAIMED-PROP > 0 is
  the dup half.

## Acceptance for this probe run
1. The `[KERFUR CENSUS]` block prints at quiescence (after a clean bracket).
2. It lists all 6 bound forms + names the 7th as exactly one `*** UNCLAIMED ***` PROP **or** `*** UNTRACKED ***`
   NPC (with eid/key/pos).
3. Behaviour unchanged otherwise (the census mutates nothing; the dup is still visible — the probe only
   *reports* it).

## After the run
Paste the `[KERFUR CENSUS]` block. I classify the root (identity-key -> scope A, or retire-side -> retire-fix),
record which bucket the forward-dup joins for the generalize audit-map, and we build the named fix (design
first, edge-vet, per the whole track). The census probe itself MAY stay (new rule 2026-06-24: RULE 2 does not
apply to probes/diagnostics/tools).
