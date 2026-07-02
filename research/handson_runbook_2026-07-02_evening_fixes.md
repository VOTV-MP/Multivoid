# Hands-on runbook — 2026-07-02 evening batch (EHH + wedge + nameplate + new model profile)

**Deployed:** DLL `BD70FB082E3B6726` + pak `hl_einstein_v1sc.pak` `2114216399348297`,
hash-verified 8/8 across all 4 installs; stale `scientist.pak` removed from all 4.
Commits: `2b2e0531` (EHH pairing) + `d4833b9b` (wedge drain) + `9180a386` (nameplate) +
`9eda3faf` (latch hygiene) + `6f4d41d1` (profile library + rename). Audit on the two fixes:
all PASS, zero blocking findings.

## What changed
1. **EHH on E-drop (client)** — a cancelled use-PRESS now deterministically cancels its
   paired E-RELEASE (`_42`); no more condition re-derivation at release time.
2. **Pile ghost-wedge** — a GrabIntent for an eid that is dead on the host now broadcasts
   `PropDestroy(eid)`: every peer drains the stale row, the next aim re-resolves to the
   real pile. No more "host must hand-cycle the pile".
3. **Nameplate** — the translucent black backing box is gone (text outline carries
   readability). Restorable from git.
4. **Client model** — NEW repose profile (your `hl_einstein_v1sc_new_profile.psk`):
   reproduced at residual 0.00005 (float-zero), wide T-pose matching the anthro template
   proportions (209.5 vs 215 arm span; old narrow was 177). Model + pak + packages renamed
   to `hl_einstein_v1sc`. Profile LIBRARY at `tools/client_model/profiles/` (v2 wide =
   default, v1 narrow kept).

## The test (host + client, normal pile play)
1. **Model look**: client's puppet on the host screen = the scientist with the NEW
   proportions (wider shoulders/arms — should deform less during animation). Verdict:
   better or worse than before?
2. **Nameplates**: no black rectangle behind nick/health bar; text still readable.
3. **E-drop**: carry a pile clump, E-drop it repeatedly — NO "EHHH" on release.
   Client log marker per drop: `[USE-RELEASE] paired E-release CANCELLED`.
4. **Wedge probe**: grab/drop piles rapidly, many cycles. If a grab is ever silently
   eaten, press E again — it must self-heal within one press (host log:
   `broadcasting PropDestroy(eid) so every peer drains its stale ghost row`). A pile
   should NEVER stay ungrabbable.
5. (Head fix already VERIFIED both peers this morning — no re-test needed.)

## If something's off
- EHH still sounds on drop → client log around the drop: is `[USE-RELEASE] paired
  E-release CANCELLED` present? Send the lines.
- A pile wedges → host log: `DENIED eid=... -- eid unresolvable` present? PropDestroy
  broadcast line present? Send both peers' lines for that eid.
- Model missing / kel skin on client puppet → host log `client_model:` lines (pak
  mount / LoadObject path) — the paths changed to hl_einstein_v1sc this build.

## Honest status
All four changes AS-BUILT + deployed + audited (the two fixes) / cook-validated (the
model: ue_skelmesh round-trip OK, winding matched, 19/19 tiles). Hands-on verdict
PENDING for all four. The wedge's UPSTREAM root (keyed-prop GC-churn re-bind by KEY)
is the next thread — today's fix makes the symptom self-heal, the re-bind will stop
the ghosts from forming at all.
