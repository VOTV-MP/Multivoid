# 2nd player VISIBLE confirmed; F12 screenshot; pose-mirroring discovered — 2026-05-22

## Headline: the remote body RENDERS (visually confirmed)

External GDI capture of the `show` scenario (orphan placed 300u in front of the
local player, `ShowBody` forcing the 4 skeletal meshes visible) shows a full,
correctly-skinned `mainPlayer_C` body standing in the world (green "VOID" hoodie,
legs, boots). Saved `tools/shots/show-test.png`. So:

- `coop::RemotePlayer::Spawn()` + `ShowBody()` produces a visible second player.
- The skin/mesh comes entirely from the class defaults on `SpawnActor` — nothing
  extra needed to make the body appear.
- The earlier "no second pawn" reports were because the **text marker crashed**
  (see below) before a clean view, and in `play` the orphan spawned 2 m to the
  side and out of the initial view.

## The crashes were the TEXT MARKER (SetText), not the body

Granular per-call logging pinned every marker crash to `UTextRenderComponent::
SetText` (the last game-thread log line is always `marker ... before SetText`).
Removing the marker → `show`/`play` run crash-free and the body shows.

- The captured FText is VALID: `convOk=1`, `obj` and `ctrl` both non-null. So my
  first hypothesis (null shared-ref controller → refcount write to null+0x20) was
  WRONG.
- My second hypothesis (ProcessEvent frees our `std::wstring`-backed FString param
  → heap corruption) was ALSO disproven by decompiling ProcessEvent
  (`0x141465930`): for properties **within the parms region** that aren't
  `CPF_OutParm`, ProcessEvent does `memcpy(a3+off, frame+off, size)` to copy the
  value back to the caller — it does NOT call `DestroyValue` on them
  (`vtable+240`/`0x141465c68` runs only for properties **beyond** ParmsSize). So
  input FStrings/FTexts are not freed by us → no corruption from that path.
- REAL LEAD, still open: reflection reports `Conv_StringToText` ReturnValue size
  = **24** but `SetText`'s `Value` size = **16** for the *same* FText type. That
  is impossible and means our FText param marshaling for SetText is wrong (size
  and/or our 24→16 truncation). The faulting address varies across runs
  (`write 0x20`, `read 0x217..035000`) — consistent with handing SetText a
  partially-wrong FText that it then dereferences. NEXT: IDA-decompile the
  `SetText` exec thunk + the native to read the true FText param layout/size, and
  fix the marshaling (or build the FText differently). Marker is DISABLED in all
  scenarios until then.

## CRITICAL coop finding: the orphan body MIRRORS the local player's rotation

User (hands-on): rotating the local camera rotates BOTH the local body and the
orphan's body in lockstep — the orphan always shows its back, you can never see
its face. The orphan is a separate `mainPlayer_C` actor, yet its body orientation
tracks the **local** player's control/camera rotation in real time.

Interpretation: `mainPlayer_C`'s visible-body orientation is driven from the
**singleton local player** (e.g. `GetPlayerController(0)` / a global player/camera
ref read by the AnimBP or a Tick), not from `self`'s own controller. This is a
textbook SP-only assumption (CLAUDE.md principle 6 / methodology): the body-yaw
path reads "the player" globally instead of per-pawn.

This is the next real coop problem and is exactly the per-player routing work:
the orphan needs its OWN orientation source (eventually the network-driven input/
control rotation), not the host's. Likely fix sites: the body mesh's AnimBP
inputs, or wherever mainPlayer_C sets body/mesh yaw each tick. Investigate via
IDA/UE4SS (escalation ladder) — find what reads the control rotation and route it
per-instance. Do NOT broadly suppress; patch the specific read site (principle 4).

Unwanted orphan gizmos (user-corrected): when the orphan spawns, TWO debug
visualizers appear with it — the "soccer ball + red arrow" AND a white
"cigarette"-like rod. BOTH belong to the spawned orphan (they appear on spawn),
NOT the local player (the cigarette is NOT the user's equipped item). These are
editor/debug components (UArrowComponent + a billboard/primitive, or normally-
hidden mainPlayer_C components that gameplay hides on possession) that the
unpossessed orphan leaves visible in shipping. TODO: enumerate the orphan's
non-skeletal components (the `skin` scenario dumps them) to identify the two
gizmos, then hide them (targeted SetHiddenInGame on those specific components,
principle 4 — not a broad hide-all).

## New tooling this session

- **F12 screenshot, in-mod, toast-free** (`harness/screenshot.{h,cpp}`): a
  background thread watches `VK_F12` and PrintWindow-captures the game window to
  `coop-screenshots/coop-<timestamp>.png` next to the mod DLL (the game's Win64
  dir), encoded via GDI+. No engine `HighResShot`, so NO "screenshot saved" toast
  and NO focus theft. Started unconditionally from `harness::Start()`.
- **External capture** for agent-side verification: `tools/capture-window.ps1`
  (already existed) + new `tools/shot.bat` wrapper. Windows-PowerShell GDI grab,
  also toast-free.
- **HighResShot is BANNED in-game** (all four `ExecuteConsoleCommand("HighResShot
  ...")` calls removed from the harness). The engine notification toast (bottom-
  right) distracts the human tester; documented in CLAUDE.md.
- Test resolution standardized to 1920x1080 (`tools/play-coop.bat`).
- `play` now spawns the orphan EXACTLY on the local player's position (user
  request) with body shown, no marker, no screenshot.

## Escalation ladder (now in CLAUDE.md), per user

reflection → IDA (IDA Pro MCP) → UE4SS (Lua probe). User OK'd installing UE4SS to
get what's needed; none of IDA/UE4SS ship (RULE №3).

## GAMMA STOMP — root-caused + FIXED

User: with the 2nd pawn spawned, the LOCAL gamma slider does nothing and the
world goes dark. Root cause: a `mainPlayer_C` carries unbound
`PostProcessComponent`s (`PostProcess_pl`, `PostProcess_overlays_OBSOLETE`) that
apply to the WHOLE screen; a 2nd pawn's defaults override the local player's
exposure/gamma. Fix (foundational, per-player routing — a remote pawn must not
own the local viewpoint): `coop::RemotePlayer::NeuterLocalSystems()` calls
`UActorComponent::K2_DestroyComponent(Object)` on the orphan's
`PostProcessComponent`s on spawn. New `engine::DestroyComponent(comp, ctx)`
(param `Object`, validated live: "destroyed PostProcess_pl /
PostProcess_overlays_OBSOLETE", no crash). Wired into `play` + `show`. (User to
confirm the gamma slider responds again.)

## Gizmos — orphan vs local player

`HideGizmos()` hides the orphan's `ArrowComponent` (grabrot/cameraRoot/heavyRot)
+ `BillboardComponent` (crouchRoot/viewmodel/unrag/pmPivot) — the red arrow and
the white "cigarette" rod. Confirmed gone on the orphan. The remaining
"football" is a StaticMeshComponent (NOT arrow/billboard) still to identify +
hide. NOTE the user also reports gizmos appearing on the LOCAL player when the
orphan spawns — needs isolation (spawn triggers a shared/global visibility?).

## Pose/anim mirror — structural findings (CXX header)

`mainPlayer_C` (from `CXXHeaderDump/mainPlayer.hpp`): the third-person body is
`mesh_playerVisible` (a SkeletalMeshComponent). `animInst_playerView` is class
`UAnimBlueprint_kerfurOmega_regular_C` (the first-person view anim). The body
has its own AnimBP. The body's orientation/anim **tracks the LOCAL player's
control rotation in real time** (user: rotate camera → orphan body rotates in
lockstep, always shows the back, constant walk anim, uses the player skin). A
static actor rotation would NOT produce real-time tracking, so the body anim/tick
reads a **global/singleton local-player reference each frame** (a VOTV SP-only
assumption), NOT the orphan's own state. (To be 100% confirmed by driving the
orphan's own rotation once we have `engine::SetActorRotation` + a controller.)

UPDATE — EXPERIMENT RESULT (self-read, NOT global): added
`engine::SetActorRotation` (K2_SetActorRotation) and rotated the orphan in `show`
to FACE the local player (yaw=-30). The body then RENDERS ITS FRONT facing the
camera (`tools/shots/rot-test.png`). So the third-person body orientation follows
the orphan's OWN actor rotation — a SELF-READ, controllable per-instance. The
earlier "global read" inference was WRONG.

So the "it copies my direction / I only ever see its back" was an OVERLAP
ILLUSION in `play`: the orphan spawned exactly ON the local player with an unset
rotation (which happened to match the local spawn), so the user was partly seeing
their OWN camera-tracked body. With a small offset + an explicit SetActorRotation,
the orphan is a clearly separate, independently-oriented second player.

REMAINING anim issue: the orphan's body is stuck in a WALK/RUN animation while
stationary (visible in rot-test.png mid-stride). The AnimBP isn't settling to
idle for an unpossessed/teleported pawn (likely reads a nonzero speed or defaults
to a moving state without a controller). Secondary; investigate the body AnimBP's
speed input next.

## ROOT CAUSE of control/view/gamma hijack: orphan auto-acquires a 2nd PlayerController

User symptom: the instant the 2nd pawn spawns, the world goes BLACK, the gamma
slider does nothing, and the LOCAL player loses WASD + mouse-look (only jump /
crouch / sit respond). Component-level fixes (destroy PostProcess, disable tick)
did NOT help -- because they weren't the cause.

### How the definitive data was found (TOOL-USE PATTERN -- reuse this)

The bug triggers on the orphan's spawn/BeginPlay, so it reproduces even
autonomously. Investigation ladder that worked:

1. **Standalone autonomous tests didn't reproduce the user-visible hijack** (no
   input -> input/view theft is invisible). First clue: the hijack is tied to the
   live controller/input, not the static scene.
2. **UE4SS Lua `RegisterHook` on the suspects FAILED to catch it** -- hooked
   `Controller:Possess`, `Actor:EnableInput`, `PlayerController:SetViewTargetWithBlend`,
   `Pawn:PossessedBy`. NONE fired on spawn -- not even the LOCAL player's own
   possession at level load. LESSON: **UE4SS UFunction hooks only fire on
   ProcessEvent-dispatched calls; native C++ engine calls (Possess, EnableInput,
   AutoPossess) bypass them.** Hooking is the wrong tool for native engine paths.
3. **WINNER: read OBSERVABLE STATE before vs after the event, and diff it.** A Lua
   `DumpPlayerState` read PROPERTIES (which always reflect truth, no hook needed):
   `PlayerController.Pawn`, `.AcknowledgedPawn`, `PlayerCameraManager.ViewTarget.Target`,
   and `orphan.Controller`. The diff was unambiguous:
   - PRE-SPAWN:  `orphan.Controller = nil`
   - POST-SPAWN: `orphan.Controller = PlayerController`
   while the LOCAL `PC.Pawn`/`ViewTarget` stayed on the local pawn -- so the orphan
   got its OWN, SECOND PlayerController.

GENERAL PATTERN: when a hook won't fire (native call) or you can't see the cause,
**snapshot the relevant object state immediately before and after the trigger and
diff the snapshots.** Property reads beat call-hooks for native engine behaviour.
(Escalation ladder: reflection -> IDA -> UE4SS; and within UE4SS: state-read
before hooks.)

### The fix (root cause, per-player routing)

`mainPlayer_C` auto-possesses Player0, so every spawned copy acquires a
PlayerController; that 2nd controller fights the local player's input/view. Fix in
`coop::RemotePlayer::NeuterLocalSystems()`: `engine::GetController(orphan)` ->
`DetachFromController(orphan)` (unpossess; the body keeps existing) ->
`DestroyActor(controller)` (delete the stray 2nd PlayerController). New engine
wrappers: `GetController` (APawn::GetController), `DetachFromController`
(DetachFromControllerPendingDestroy), `DestroyActor` (K2_DestroyActor),
`SetActorTickEnabled`. SAFE because the UE4SS data confirmed the orphan's
controller is a DISTINCT 2nd PlayerController (local `PC.Pawn` stayed local), not
the local PC.

## Next

1. FOUNDATION for independent orphan drive (also the real pose-mirror fix):
   `engine::SetActorRotation` + give the orphan its own controller (AIController,
   NOT split-screen/CreatePlayer per [[project_phase1_player_and_spawn]]); verify
   whether the body then follows the orphan's own control rotation (confirms the
   global-read vs self-read question).
2. Identify + hide the "football" StaticMeshComponent; isolate the local-player
   gizmo appearance on spawn.
3. Phase 3 UDP transport -> drive RemotePlayer location + rotation + movement from
   snapshots (this is what ultimately removes the mirror).
4. Marker SetText FText marshaling (IDA) — deferred, marker disabled.
