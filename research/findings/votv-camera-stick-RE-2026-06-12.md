# Camera / wall-attachable surface-stick RE + the v68 mirror design

Date: 2026-06-12 (session 13). Trigger: user — "client sticks the camera and it
stays, but host sees it as falling down." RE performed by an agent pass over
the pak_re workflow (kismet-analyzer JSONs in research/pak_re/:
comp_wallAttachable.json, prop_wallAttachable.json, prop_camera_bad.json,
prop_camera_good.json, prop_base.json, mainPlayer.json); implemented same
session as protocol v68 (coop/prop_stick_sync + remote_prop gates).

## 1. Class topology — the stick is NOT camera code

`Aprop_camera_good_C : Aprop_camera_bad_C : Aprop_wallAttachable_C :
Aprop_corded_C : Aprop_C`. ALL camera variants inherit the stick from
**`Aprop_wallAttachable_C`** (whiteboards/signs/pryable variants included);
the mechanism lives in the ActorComponent **`Ucomp_wallAttachable_C`**
(`comp_wallAttachable` @+0x03B0 on the prop; comp fields: `prop` @+0x138,
`holding` @+0x142, `Alpha` @+0x144, `tr_A` @+0xD0, `tr_B` @+0x100,
`pryingRequired` @+0x168, `doStick` @+0x141; CDO start=-2.5 end=5.0
initForceStick=true). A fix at this level covers every wall-attachable.

## 2. The mechanism (bytecode ground truth)

- **Trigger is NOT a menu/RMB action.** The camera's menu action 8 is just
  camera on/off (`active = !active; updState()`). The stick is a
  **held-proximity poll**: every grab (re)arms `sticking()` (10 Hz latent
  loop, `Delay(0.1)` retries) while `canStick = doStick && (holding ||
  skipHolding) && !IsChildActor && comp->IsSimulatingPhysics &&
  !GetAttachParentActor()`.
- **Commit = ubergraph offset 45** (comp_wallAttachable ubergraph), reached
  via `Delay(0.5, resume@45)` after a successful surface trace
  (SphereTraceSingleForObjects along the `stick` arrow), or via
  `forceStick(skipHolding)` (a direct LOCAL `goto 45` — BeginPlay/loadData
  self-stick). The commit body:
  1. re-trace; bail to the retry loop if invalid/simulating/pawn;
  2. `switch(pryingRequired){false => prop->frozen; true => prop->Static} = true`;
  3. `prop->init()` — base prop_C init does
     `SetSimulatePhysics(NOT(static||frozen||sleep))` (physics OFF);
  4. `K2_AttachToComponent(HitComponent, KeepWorld×3, bWeld=true)` +
     `HitActor->OnDestroyed += dest` (auto-unstick when the surface dies);
  5. computes `tr_B` from ImpactPoint/ImpactNormal, enables comp tick,
     `alpha=0` — ReceiveTick lerps the actor to tr_B over 0.25 s (rate 4/s);
  6. `SpawnEmitterAttached(eff_OC_freeze, prop->StaticMesh, ...)` — the
     user's "closing circle" VFX (`/Game/particles/eff_OC_freeze`).
- **Unstick = re-grab**: `playerGrabbed_pre -> comp->unstick(false)`:
  non-pryable (cameras) clears `frozen=false; static=false; prop->init()`
  (SetSimulatePhysics(true) — which in UE4.27 also DETACHES the attached
  root) + re-arms sticking() after 1 s. Pryables need `withTool=true` (the
  crowbar path); plain grab just shows a hint. `dest` (surface destroyed)
  clears `static` only (SP bug-compat quirk) + re-arms.
- **SP save-load parity**: `loadData` restores frozen/static + init() but its
  `forceStick(false)` no-ops (`holding=false`) — SP itself accepts "frozen at
  pose, UN-attached" after a load. Frozen+pose is the sufficient core state;
  the attach is fidelity.

## 3. PE-visibility (what our detour can and cannot see)

- `mainPlayer` invokes playerGrabbed_pre/playerGrabbed/playerHoldPre via
  `EX_LocalVirtualFunction` — PE-INVISIBLE (so the grab/unstick edges are not
  directly observable). All prop->comp calls + the in-commit
  K2_AttachToComponent/SpawnEmitterAttached are local/native finals —
  invisible.
- **The latent resumes ARE visible**: FLatentActionManager resumes every
  Delay/RetriggerableDelay via `CallbackTarget->ProcessEvent(
  ExecuteUbergraph_comp_wallAttachable, &LinkID)`. A player-driven stick
  ALWAYS enters the commit through the `Delay(0.5, 45)` resume — so a POST
  observer on `ExecuteUbergraph_comp_wallAttachable` gated on
  `EntryPoint==45` sees every real stick. Resume traffic on that UFunction is
  only the 10 Hz poll (per held wall-attachable), the re-arms, and the
  commit — negligible.
- The only PE-invisible path to 45 is `forceStick` (BeginPlay/loadData) —
  which both peers run locally anyway, and which provably no-ops on loads.
- The receiver-side replay (our PE dispatch of `forceStick(true)`) enters the
  ubergraph via `EX_LocalFinalFunction` — it does NOT re-dispatch
  ExecuteUbergraph through PE, so the observer cannot echo.

## 4. Why it desynced (the coop pipeline gap)

The receiving peer's pipeline had NO runtime carrier for frozen/static/attach
changes (PropSpawn.physFlags kFrozen/kStatic are spawn/JOIN-time only — late
join was already correct), and remote_prop's release paths unconditionally
re-enabled physics: the explicit `OnRelease` (`DriveSimulate(mesh, true)`),
the 500 ms PropPose stream-stop timeout, and the switched-prop implicit
release. Chain: client sticks (its own frozen=true, physics off) -> its hold
breaks -> PropRelease/timeout on the host -> host re-simulates -> falls. The
host's own `frozen` was never written, so host SP logic also disagreed.

## 5. The v68 fix (implemented; protocol v68, ReliableKind::PropStickState=64)

MTA precedent: SET_ELEMENT_FROZEN + ATTACH_ELEMENTS element-state RPCs
(CElementRPCs.cpp) — state message, not event replay.

- **Sender** (any peer; relayed like symmetric prop state): the EntryPoint==45
  POST observer records {prop, idx} (worker-safe record-then-act);
  `prop_stick_sync::Tick` drains on the NEXT net-pump pass (no settle delay —
  see §6), verifies the prop ended frozen/static (the commit can bail), and
  broadcasts `PropStickStatePayload{key, eid, flags(frozen|static), commit
  pose}` (64 B, same reliable lane as PropRelease — `session_lanes.h` pins the
  pairing with explicit Lane::Normal cases). The sender's own pose stream also
  ENDS at the commit: local_streams treats a frozen/static held wall-attachable
  as stream-end, firing its release edge in the same pump pass as — and
  therefore lane-ordered after — the stick broadcast (TickGameplay runs before
  the stream tick), and guaranteeing a lingering engine hold can never stream
  the 5 poses that would re-unstick the receiver's just-stuck mirror.
- **Receiver**: resolve key-first/eid-fallback -> `ClearAnyDriveFor` (drive
  cache clear, NO physics re-enable) -> re-pose -> `SetActorSimulatePhysics
  (true)` (the canStick precondition; the drive had it kinematic) -> PE
  `comp->forceStick(true)` = FULL SP replay (field write + attach +
  OnDestroyed binding + eff_OC_freeze VFX + glide). If the replay's re-trace
  diverges (frozen still false): raw `WriteFrozen/WriteStatic(true)` +
  `SetActorSimulatePhysics(false)` + re-pose — exactly SP's save-load
  degraded mode.
- **Unstick: deliberately NO message.** The unstick is a grab and grabs
  stream PropPose. remote_prop unsticks a stuck WALL-ATTACHABLE mirror
  (class-gated — a frozen NON-attachable never legitimately streams and is
  skipped outright) only on a SUSTAINED stream: >=5 fresh poses within a
  400 ms streak window (`g_pendingUnstick` per slot). The 1-2 stale PropPose
  packets in flight between the sender's commit and its hold-break must NOT
  unstick the mirror. `UnstickForDrive` = clear flags +
  SetActorSimulatePhysics(true) (detaches); the drive then re-disables
  simulate for the kinematic carry.
- **Release gates**: `StickHoldsPhysicsOff(actor)` (live + Aprop_C lineage +
  frozen||static) now gates the OnRelease re-simulate+velocity+thrown, the
  500 ms timeout, and the implicit release — a stuck prop stays stuck when
  the sender's hold ends.
- **No init() dispatch anywhere in the module**: init is overridden along the
  camera lineage (per-instance override resolution is the kerfur
  PickDropPropFn problem again); its only load-bearing effect here is the
  SetSimulatePhysics recompute, applied directly. The forceStick replay runs
  the real BP init internally.
- ue_wrap/prop.cpp gained `WriteFrozen` / `WriteStatic` (live-prop field
  writes @0x02DA/0x02D8).

## 6. The audit verdict + the applied fix batch (post-compact, same session)

The full v68 audit landed: **zero perf CRITICALs** (hot-path table clean —
the non-45 observer path is 2 null checks + 2 offset guards + 1 compare; the
125 Hz Tick is mutex + count==0 when idle), one correctness CRITICAL = this
exact ordering question. Verdict math: the hold breaks 0-100 ms after commit,
`local_streams` fired PropRelease on the next 8 ms pump tick while the stick
waited out kSettleMs=450 on the SAME lane — faithful FIFO delivered the wrong
order on EVERY client stick: release-first → re-simulate → fall ≈ 342-450 ms
≈ 58 cm visible dip → stick arrives → snap-back self-heal.

Applied (build 17:53:24, deployed x4; protocol stays 68 — no wire change):

1. **F1 (the CRITICAL)**: settle delay REMOVED — Tick drains every recorded
   commit on the next pump pass (recordedMs/NowMs machinery deleted with it).
   `frozen` is already true at POST-observer time (the commit body ran inside
   the observed dispatch), the commit pose is where the trace succeeded, and
   the receiver's forceStick replay re-derives the settled pose + plays its
   own glide/VFX. Ordering is now STRUCTURAL: within one net-pump pass,
   TickGameplay (the stick broadcast) runs before local_streams::Tick (the
   release edge), and the hold-break can never precede the commit.
2. **F2**: sender-side stream-end gate (local_streams) — a held
   wall-attachable with frozen/static set stops streaming immediately and
   fires its release edge in that same (stick-first) pass. Kills the audit's
   tail risk: a lingering engine hold could otherwise stream 5 fresh poses
   and 5-streak-unstick the just-stuck mirror permanently.
3. **F3**: `g_disabled` latch — the signature-drift refuse path previously
   latched g_installed only; a stick from a peer on an old game build would
   have written `frame[g_skipHoldingOff]` past the 16-byte dispatch buffer.
   OnStickState now drops (one-shot warn) when disabled.
4. **F4**: install also validates `R::FunctionFrameSize(forceStick) <= 16`
   (ProcessEvent memcpys PropertiesSize from our buffer — the DrivePropThrown
   house pattern).
5. **F5**: explicit `LaneForKind` cases pin PropStickState + PropRelease to
   Lane::Normal with the pairing rationale — previously both rode `default:`
   and a future single-kind lane move would have silently split the ordering.

Remaining audit NOTEs (recorded, deliberate): remote_prop.cpp past the 800
soft cap (extraction proposal: lifecycle/mirror block →
remote_prop_lifecycle.cpp — queued with the npc_sync/prop_lifecycle debt);
stick pose magnitude unclamped (NaN-gated only — PropPose itself doesn't
clamp positions either; the proper shape is a shared pose validator across
all pose receivers, a separate sweep); BP-pointer latches go stale after an
in-process world reload (house-wide shape, queued for the stale-detect
sweep); ~1 s install-window stick loss on a just-booted receiver (relay
unaffected; converges via snapshot on rejoin).

## 7. Stuck-state fields (for reference)

| Field | Where | Role |
|---|---|---|
| `frozen` | Aprop_C +0x02DA | THE camera stick bit |
| `Static` | Aprop_C +0x02D8 | the pryingRequired variant |
| attach parent | engine AttachParent (KeepWorld, bWeld) | fidelity (moving surfaces, OnDestroyed unstick) |
| final pose | actor transform post-glide (tr_B) | a frozen write without it floats mid-air |
| comp transients (holding/Alpha/tr_A/tr_B) | Ucomp_wallAttachable_C | NOT needed on the wire |
