# Hands-on runbook — 2026-07-02 evening batch (EHH + wedge + nameplate + model profile + SKINS)

**Deployed (take 3b, SKINS + audit fix):** DLL `1B00C0DB22751C37` (protocol **v93** —
both peers must update; v92 peers are version-gated out with an "update your mod"
message) + pak `hl_einstein_v1sc.pak` `AE49002C2A5DB1DB` (v1 narrow profile) + preview
PNGs, all 4 installs; `rvi_scientist_v1sc.pak`/.bmp (user-placed) intact.
Take-2 was DLL `BD70FB08...` — superseded before its hands-on by the skins build; all
take-2 changes (EHH pairing `2b2e0531`, wedge drain `d4833b9b`, nameplate `9180a386`,
hygiene `9eda3faf`, v1-profile revert `e094093d`) are IN this DLL too, still verdict
PENDING. NO autonomous smoke ran (user at the PC — this runbook IS the test).
**Audits (2 agents on the diff):** perf/hot-path = all PASS (local_body::Tick alloc-free
at pump rate, 1 Hz gate real, no per-frame FS scan, no SkinChange double-relay; WARN:
remote_player.cpp 841>800 LOC — remote_player_spawn extraction QUEUED). Correctness =
one HIGH confirmed + FIXED before this deploy: SkinChange was not pre-world-sendable, so
a skin changed while a joiner sat in its 30-60 s load window was silently dropped
forever (the v90-b3 "mutation during the window" class) — SkinChange added to
IsPreWorldSendableKind (receiver is engine-free pre-puppet). Everything else clean:
wire bounds, forgery guards, name validation at all 4 boundaries, revert symmetry,
thread discipline, proto wiring.

## NEW: the v93 SKINS system (what to test first)
Every player now has a persistent body skin (`votv-coop.ini player_skin=`, written next
to `player_guid=`; a fresh identity gets `hl_einstein_v1sc` — so BY DEFAULT the HOST is
now ALSO a scientist; pick `dr_kel` in the browser if you want the host back to kel, it
persists).
1. **F1 → Cosmetics → Skins**: tile browser with previews (dr_kel + hl_einstein_v1sc +
   rvi_scientist_v1sc — previews read `<name>.png/.bmp` next to the pak; your placed
   .bmp files are exactly that). Current skin = green tile.
2. **Local first-person body**: look down — your OWN torso/legs wear YOUR skin (the
   immersion fix). Host default = scientist now (see above).
3. **Live change**: pick another skin mid-game — your body changes instantly, the OTHER
   peer sees your puppet change within a second, chat line "<nick> changed skin to ...".
4. **Persistence**: quit the client, rejoin — the skin came back from the ini.
5. **rvi_scientist_v1sc — RE-COOKED (late 2026-07-02)**: your first converted pak was
   broken (A-posed arm chunks; toe-half of each foot pinned to the pelvis) — the
   converter postmortem found 106/764 verts silently mis-skinned + a foreign repose
   profile. The pak on all 4 installs is now cooked FROM YOUR OWN manual pose
   (`..._my_pose_good.psk`, reproduced to 9e-5; toes ride the feet, the head prop rides
   the head; pak `ED666BE5`, old bad hash was `0AC43284`). Pick it — expect YOUR pose
   and proper limb tracking on both your body and your puppet on the other screen.
6. **dr_kel revert**: pick dr_kel — body back to kel WITHOUT the atlas texture stuck on
   it (the material override clear).
Log markers: `local_body: native kel mesh captured`, `client_model: ... -> skin '<name>'`,
`player_handshake: announced local skin`, `skin_registry: N skin(s) catalogued`.
If a skin shows KEL on the other peer: that peer's log will say
`client_model: skin '...' mesh NOT loadable (pak absent...)` — pak missing there.

## What changed
1. **EHH on E-drop (client)** — a cancelled use-PRESS now deterministically cancels its
   paired E-RELEASE (`_42`); no more condition re-derivation at release time.
2. **Pile ghost-wedge** — a GrabIntent for an eid that is dead on the host now broadcasts
   `PropDestroy(eid)`: every peer drains the stale row, the next aim re-resolves to the
   real pile. No more "host must hand-cycle the pile".
3. **Nameplate** — the translucent black backing box is gone (text outline carries
   readability). Restorable from git.
4. **Client model** — VERDICT IN (take 1): the v2 wide profile look was REJECTED
   in-game → the scientist is RE-COOKED on the **v1 narrow profile** (this pak,
   `AE49002C`; repose reproduces the v1 manual PSK at max 0.00009). v1 is the library
   DEFAULT again; v2 wide stays in `tools/client_model/profiles/` unused. The
   `hl_einstein_v1sc` rename stands.

## The test (host + client, normal pile play)
1. **Model look**: client's puppet on the host screen = the scientist back on the
   FAMILIAR v1 proportions (narrow, the look that passed 2026-07-02 morning). Confirm
   it matches what you liked.
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
Take-2 changes (EHH pairing, wedge drain, nameplate, v1-profile model) AS-BUILT +
audited / cook-validated — verdict PENDING. Take-3b SKINS: AS-BUILT, compiled clean,
deployed 4/4 (DLL `1B00C0DB`), NO smoke (user at PC), two audit agents: perf all-PASS,
correctness 1 HIGH found + root-fixed pre-handoff (SkinChange pre-world gate). Protocol
v92→v93: BOTH peers must run this DLL. QUEUED extractions: remote_player_spawn
(841>800), puppet_diag (967>800). The wedge's UPSTREAM root (keyed-prop GC-churn
re-bind by KEY) is still the next thread.
