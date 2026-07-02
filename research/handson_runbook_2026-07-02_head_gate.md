# Hands-on runbook — puppet HEAD-FREEZE root fix (state-gate defeat, 2026-07-02)

**Deployed:** DLL `8b4c819d88877574`, all 4 install folders hash-verified. Commit `5b2cb5ff`
(+ finding banner `4b2d5d0f`). Pak unchanged (`5edabac7`).

## What this build fixes
The back-turned head freeze ("движения головы отключаются напрочь когда puppet повернут спиной").
Root, statically proven from the cooked AnimBP: the head/neck FAnimNode_LookAt nodes are the ENTIRE
sub-graph of state `lookAtPlayer`; `lookingAtPlayer` (dot-test: is the OBSERVER's camera within the
front cone) gates that state's transitions. Back-turned -> flag false -> state exits (0.25 s
crossfade) -> head snaps to NEUTRAL. Fix: post-BUA UFunction hook re-asserts lookingAtPlayer=true
every anim update -- PUPPET-ONLY by identity (mainPlayer_C actor + null Controller); kerfur NPCs
and the local player untouched.

## The test (2 instances, same as the coop visual)
1. Host + client, join, stand near each other.
2. Turn the puppet's BACK to you (the other player faces away) while the other player wiggles
   their mouse/looks around.
3. **PASS:** the puppet's head keeps tracking the remote look at ANY body orientation -- no snap
   to neutral. Natural reach limit ~67 deg off the body stays (the node clamp -- a live neck, not
   the freeze); standing, the body starts turning at ~60 deg lead to absorb more.
4. **Kerfur NPC check (must stay native):** walk behind a kerfur -- its head still freezes/returns
   as before. That is intended (user constraint).
5. Host log at puppet spawn: `puppet: head-look state-gate hook installed -- post-BUA
   lookingAtPlayer=true on PUPPET instances only`.
6. Optional proof: `[dev] puppet_head_probe=1` in the host ini -> `[HEAD-PROBE]` lines must show
   `lookingAtPlayer=1` on every sample while back-turned (the fix's verifier).

## Verdicts
- Head tracks at all orientations = ROOT FIX VERIFIED -> close the thread.
- Head still snaps to neutral back-turned = the hook write is losing somewhere -> send the host
  log (grep `head-look state-gate hook`) + a `puppet_head_probe=1` run.
- Head tracks but pins at ~67 deg on extreme look-back and that feels wrong = the node clamp knob
  (separate 1-float tuning, ask).

## Honest status
Fix AS-BUILT + deployed, statically proven mechanism; hands-on VERDICT PENDING. Kerfur untouched by
construction (identity filter), not yet visually confirmed.
