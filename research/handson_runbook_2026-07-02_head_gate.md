# Hands-on runbook — puppet HEAD-FREEZE root fix (state-gate defeat, 2026-07-02)

## TAKE 2 — host-side FAILED install root-fixed (`b77793d7`)

**Deployed:** DLL `AFDCC37FFD359142`, all 4 install folders hash-verified. Pak unchanged
(`5edabac7`).

**Take-1 verdict (13:01 session):** ASYMMETRIC — client saw the host's puppet head track
back-turned (fix works), host still saw the client's puppet freeze. Logs pinned the root in
minutes: host log 13:01:45 `head-look state-gate hook FAILED` right after
`ufunction_hook: table full (4 slots)`. The Func-hook table held 4 slots; the HOST fills all
4 before any puppet spawns (save-indicator x2 on both peers + host-authoritative
trash-collect x2), so the head-gate hook — the 5th — was refused on the host ONLY. The
client had two free slots -> installed -> the asymmetry. NOT an anim/gate problem; the
take-1 mechanism is correct and proven by the client side.

**Take-2 fix:** the thunk table is now generated from `kMaxNativeHooks = 16` (was a
hand-enumerated 4); a FAILED head-gate install now logs at ERROR (the take-1 FAILED hid
inside an INFO line).

## The test (unchanged, 2 instances)
1. Host + client, join, stand near each other.
2. Turn the puppet's BACK to you while the other player wiggles their mouse/looks around.
3. **PASS (now expected on BOTH peers):** the puppet's head keeps tracking at ANY body
   orientation — no snap to neutral. Natural reach limit ~67 deg off the body stays (a live
   neck, not the freeze).
4. **Kerfur NPC check (must stay native):** walk behind a kerfur — its head still
   freezes/returns as before. Intended.
5. **Log check, BOTH peers this time** (`votv-coop.log` at puppet spawn):
   `puppet: head-look state-gate hook installed`. There must be NO
   `ufunction_hook: table full` and NO `head-look state-gate hook FAILED` anywhere.
6. Optional proof: `[dev] puppet_head_probe=1` in the ini -> `[HEAD-PROBE]` shows
   `lookingAtPlayer=1` while back-turned.

## Verdicts
- Head tracks at all orientations on BOTH screens = ROOT FIX VERIFIED -> close the thread.
- Still freezes on one peer = grep that peer's log for `state-gate hook` + `table full`,
  send the lines.
- Tracks but pins at ~67 deg on extreme look-back and that feels wrong = the node clamp
  knob (separate 1-float tuning, ask).

## Honest status
Take-1 mechanism (post-BUA hook, puppet-only identity filter) HANDS-ON PROVEN on the client
peer. Take-2 removes the host-side install refusal (capacity); the hook code itself is
byte-identical. Hands-on verdict for the HOST side PENDING. Kerfur untouched by
construction, take-1 hands-on had no kerfur regression reported.
