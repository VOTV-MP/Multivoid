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

CONSEQUENCE: there is no quick component-toggle fix. The orphan mirrors the local
player because it has NO independent pose/input source yet. Truly fixing it =
driving the orphan independently (its own control rotation + movement), which is
the network/input phase — the core coop work, not a patch. This is on the
critical path: Phase 3 (UDP) sends the remote player's input/pose; we apply it to
the orphan (control rotation + movement), which both breaks the mirror AND is the
actual feature.

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
