// ue_wrap/sdk_profile_names.h -- THE VERSION SURFACE, part 2: content names.
//
// Split out of sdk_profile.h 2026-07-04 (modular file-size rule: the combined
// header hit the 1500-LOC hard cap). Same contract as sdk_profile.h -- one
// game build per revision -- but a DIFFERENT failure mode: everything here is
// a game-CONTENT identity (blueprint class names, UFunction names, level
// names, save-landmark coordinates). These change when the game's blueprints
// or maps change, NOT when the engine binary recompiles. Re-derive from the
// CXX header dump / bp_reflect on a game-version bump.
//
// sdk_profile.h #includes this header, so consumers keep a single include and
// the P::name:: spelling everywhere.

#pragma once

namespace ue_wrap::profile {

// ---- content names (change with game content, not the engine) ------------
namespace name {
inline constexpr const wchar_t* MainPlayerClass = L"mainPlayer_C";
inline constexpr const wchar_t* GamemodeClass = L"mainGamemode_C";
// AmainGamemode_C::transition(FName LevelName) -- VOTV's own level-travel verb
// (mainGamemode.hpp). Used by the local-death flee with the full path "/Game/menu"
// to reach the main menu (the short name does not resolve). See engine::ReturnToMainMenu.
inline constexpr const wchar_t* MainGamemodeTransitionFn = L"transition";
inline constexpr const wchar_t* ActorClass = L"Actor";
inline constexpr const wchar_t* WorldClass = L"World";
inline constexpr const wchar_t* SetActorLocationFn = L"K2_SetActorLocation";
inline constexpr const wchar_t* GameplayLevel = L"untitled_1";

// КПП (kontrolno-propusknoi punkt / guard-checkpoint) spawn point on the
// `s_may2026` save -- the visible landmark at the base entrance. Host
// naturally lands here when the save loads (per the LoadStorySave log:
// "in gameplay (mainPlayer @ -37695,69978,6420)"). The CLIENT instance
// loads the same save but lands ~14 m away at a different wakeup spot
// (per [[project-autotest-spawn-pose]]); the user's "spawn remote on КПП"
// rule (2026-05-23) wants the client teleported to this known landmark
// after gameplay loads. Value is the actor center (capsule centre), in
// world cm. Re-derive per game version.
inline constexpr float kKPPSpawnX = -37695.f;
inline constexpr float kKPPSpawnY =  69978.f;
inline constexpr float kKPPSpawnZ =   6420.f;

// Engine classes/functions we dispatch through (stable engine names, not VOTV
// content -- but kept here so the porting surface is one file). The persistent
// GameInstance subclass is VOTV content (the world context that survives a
// level load).
inline constexpr const wchar_t* KismetSystemLibraryClass = L"KismetSystemLibrary";
inline constexpr const wchar_t* ExecuteConsoleCommandFn = L"ExecuteConsoleCommand";
inline constexpr const wchar_t* GameInstanceClass = L"mainGameInstance_C";

// 2026-05-25 LATE +5h (F3 RestoreVitals dev feature): the live UsaveSlot_C
// instance holds the player's CURRENT runtime vitals (food/sleep/health/
// coffeePower), not just the persisted save record -- the BP code mutates
// these fields in-place, then the SaveGameToSlot UFunction serializes the
// whole object on save. So `FindObjectByClass(SaveSlotClass)` returns the
// object whose float fields ARE the live vitals; writing them via reflection
// max-outs the meters immediately. Field names confirmed via the
// CXXHeaderDump of saveSlot.hpp (food @0x00E4, sleep @0x00E8, health
// @0x0428, coffeePower @0x04D0 -- but offsets resolved at runtime via
// FindPropertyOffset, not hardcoded, per the BP-cooked-offset-volatile
// rule in reflected_offset.{h,cpp}).
inline constexpr const wchar_t* SaveSlotClass = L"saveSlot_C";

// Actor spawning (the BlueprintCallable deferred-spawn pair the K2
// SpawnActorFromClass node uses) + transform get/set.
inline constexpr const wchar_t* GameplayStaticsClass = L"GameplayStatics";

// Phase 5N1 (NPC sync foundation, 2026-05-25): UGameplayStatics's
// BeginDeferredActorSpawnFromClass UFunction is the canonical deferred-
// spawn entry every BP "spawn actor of class" graph compiles down to.
// Intercepting it gives us ONE engine-level hook for all NPC + spawner
// classes; replaces the 14-per-spawner-observer plan (architecture-doc
// Hook surface table line 222) with 1 observer. See
// research/findings/votv-npc-sync-prereqs-RE-2026-05-24.md section 4.
// The constant for this UFunction is `BeginDeferredSpawnFn` (declared
// further down in this file, alongside the engine-side reuse from
// remote_prop.cpp + engine.cpp -- RULE 2: one name across all callers).

// Phase 5N1 NPC class allowlist (12 enemy NPC classes per
// votv-npc-sync-prereqs-RE-2026-05-24.md section 1 verdict). On CLIENT,
// every BeginDeferredActorSpawnFromClass for a class in this list is
// SUPPRESSED (interceptor returns true; ActorClass param zeroed so the
// BP graph receives nullptr and bails per UE4's standard nullptr-check
// pattern). Host runs spawners normally; client's NPC actors arrive via
// the EntityPoseBatch wire (Inc2; not implemented yet).
//
// The original 12 enemy classes are confirmed targeting-compatible with our
// mainPlayer_C orphan puppet (RE section 1 verdict: "ALL 12 enemy classes
// either use AIPerception/PawnSensing or generic Cast to AActor / Cast to
// mainPlayer_C. The orphan puppet -- being a real Pawn of the same class
// -- is reachable by every targeting path inspected."). The 2 event/late-game
// additions (killerwisp_C, ventCrawler_C; 2026-06-13) ride the same pose-mirror
// path. killerwisp_C IS targeting-compatible (AIPerception + Target APawn, same
// shape as the 12 -- SDK-verified); its kill of a REMOTE peer is coop-driven: the
// v1 per-victim relay (1ae92a1a, probe-verified 2026-07-03) + the v2 grab
// choreography + aggro selector (wisp_attack_sync + wisp_grab_hold). ventCrawler
// wall-crawl pitch/roll is smoke-pending (yaw-only stream).
inline constexpr const wchar_t* NpcClass_Zombie       = L"npc_zombie_C";
inline constexpr const wchar_t* NpcClass_KerfurOmega  = L"kerfurOmega_C";
inline constexpr const wchar_t* NpcClass_Krampus      = L"npc_krampus_C";
inline constexpr const wchar_t* NpcClass_Funguy       = L"npc_funguy_C";
inline constexpr const wchar_t* NpcClass_GoreSlither  = L"npc_goreSlither_C";
inline constexpr const wchar_t* NpcClass_Insomniac    = L"insomniac_C";
inline constexpr const wchar_t* NpcClass_Fossilhound  = L"fossilhound_C";
inline constexpr const wchar_t* NpcClass_Antibreather = L"antibreather_C";
inline constexpr const wchar_t* NpcClass_Orborb       = L"npc_orborb_C";
inline constexpr const wchar_t* NpcClass_ArirFollower = L"npc_arirFollower_C";
inline constexpr const wchar_t* NpcClass_AriralShooter   = L"npc_ariral_shooter_C";
inline constexpr const wchar_t* NpcClass_AriralPigBeater = L"npc_ariral_pigBeater_C";
// Event/late-game creatures brought into the host-authoritative entity layer
// (RULE-1: extend npc_sync, do NOT replay the trigger). VERIFIED against the SDK
// CXXHeaderDump (the no-assuming rule, [[feedback-verify-game-domain-facts]]):
//   * killerwisp_C (Akillerwisp_C : ACharacter) -- the lethal YELLOW "Killer Wisp"
//     (NOT the base wisp_C swarm, NOT the per-player-effect colored wisps wisp_o/b/
//     g/...; those are SIBLING ACharacter classes with per-player effects, not
//     simple mirrorables). Uses AIPerception + a Target APawn exactly like the 12
//     enemies -> targeting-compatible with our puppet. Has comp_radarPoint -> shows
//     on radar. Spawns via Aticker_yellowWispSpawner_C (late-game: post-Obelisk,
//     days 26-31 night), NOT the F1 `wisps` event -- test via that spawner / a dev
//     spawn, not the wisps button.
//   * ventCrawler_C (AventCrawler_C : ACharacter) -- F1 `ventCrawler` event.
// Both ACharacter -> ride the existing BeginDeferred interceptor + pose stream.
// SMOKE-PENDING fidelity: ventCrawler wall-crawls (yaw-only pose stream renders it
// upright -- a known pitch/roll gap to close after smoke).
inline constexpr const wchar_t* NpcClass_KillerWisp   = L"killerwisp_C";
inline constexpr const wchar_t* NpcClass_VentCrawler  = L"ventCrawler_C";
//   * wisp_C (Awisp_C : ACharacter, wisp.hpp) -- the plain wispSwarm-event wisp (2026-07-03).
//     SCOPE (the source-gate, coop/npc_world_enum): EVENT-SWARM wisps (trigger_wispSwarm_C's
//     EX_CallMath loop) AND the ambient sky wisps (ticker_wispSpawner_C -- REVERSED 2026-07-10:
//     the ticker spawns at ABSOLUTE map coords +-60-70k X/Y Z=80k, measured in the bytecode
//     dump; world-anchored, NOT player-proximity, so host-authoritative like everything else.
//     The earlier "stays per-peer" call assumed player-anchoring that is not there).
//     Both spawn paths are EX_CallMath (PE-interceptor-blind): the catch is the
//     ufunction_hook Func-thunk on BeginDeferred, source-gated by FFrame::Object's class.
//     Mirror fidelity: spawns invisible; fade-in fires at the tick-driven LANDING edge
//     (CMC CurrentFloor.bBlockingHit), so the mirror keeps ACTOR tick (CMC still parked)
//     and the pose lane drives the landing edge (landed=true + dir(true)) on inAir 1->0
//     (ue_wrap::wisp::DriveWispLanding). RE: votv-event-trigger-graph-RE-2026-07-03.md.
//   * the 8 COLOR variants (ticker map, weights 0.1-5 vs wisp_C's 100): direct ACharacter
//     subclasses (SDK-verified 2026-07-10, wisp_b.hpp -- NOT wisp_C children, so
//     IsDescendantOfAny needs each listed).
inline constexpr const wchar_t* NpcClass_Wisp         = L"wisp_C";
inline constexpr const wchar_t* NpcClass_WispB        = L"wisp_b_C";
inline constexpr const wchar_t* NpcClass_WispRed      = L"wisp_red_C";
inline constexpr const wchar_t* NpcClass_WispBl       = L"wisp_bl_C";
inline constexpr const wchar_t* NpcClass_WispW        = L"wisp_w_C";
inline constexpr const wchar_t* NpcClass_WispG        = L"wisp_g_C";
inline constexpr const wchar_t* NpcClass_WispO        = L"wisp_o_C";
inline constexpr const wchar_t* NpcClass_WispP        = L"wisp_p_C";
inline constexpr const wchar_t* NpcClass_WispBlu      = L"wisp_blu_C";

// Compact array for the allowlist resolver (Inc1 of Phase 5N1). Iterated
// once at install time; each name resolved via R::FindClass + cached. The
// classes are loaded on first gameplay-level transition (NOT at menu)
// per VOTV's content-cooking pattern -- so the install retry path must
// re-attempt until all entries resolve (or log per-class failures). The
// array is sizeof-derived (kNpcAllowlistSize), so adding an entry binds
// everywhere automatically -- no count to update in npc_sync.
inline constexpr const wchar_t* kNpcAllowlist[] = {
    NpcClass_Zombie,
    NpcClass_KerfurOmega,
    NpcClass_Krampus,
    NpcClass_Funguy,
    NpcClass_GoreSlither,
    NpcClass_Insomniac,
    NpcClass_Fossilhound,
    NpcClass_Antibreather,
    NpcClass_Orborb,
    NpcClass_ArirFollower,
    NpcClass_AriralShooter,
    NpcClass_AriralPigBeater,
    NpcClass_KillerWisp,   // yellow Killer Wisp (late-game; AIPerception like the 12)
    NpcClass_VentCrawler,  // event `ventCrawler`  (smoke-pending: wall-crawl pose)
    NpcClass_Wisp,         // wispSwarm-event + ambient sky wisp (both sources host-auth since 2026-07-10)
    NpcClass_WispB,        // sky-wisp color variants (ticker_wispSpawner map; direct ACharacter
    NpcClass_WispRed,      //   subclasses, NOT wisp_C children -- each needs its own row)
    NpcClass_WispBl,
    NpcClass_WispW,
    NpcClass_WispG,
    NpcClass_WispO,
    NpcClass_WispP,
    NpcClass_WispBlu,
};
inline constexpr size_t kNpcAllowlistSize = sizeof(kNpcAllowlist) / sizeof(kNpcAllowlist[0]);

// B3b (2026-06-17): the NON-Character event actors the Character-only NPC mirror cannot replicate.
// npc_pose_drive drives pos + YAW-ONLY rotation + DriveCharacterMovement (CMC@0x288, ACharacter-only),
// so these AActor/APawn ships/UFOs/saucers need a SEPARATE transform-only FULL-rotation mirror
// (coop::element::WorldActor + coop/world_actor_sync). A SECOND BeginDeferred interceptor consumes this
// list; it is DISJOINT from kNpcAllowlist (a class is one or the other), so the two interceptors are
// conflict-free (game_thread.h multi-interceptor support). VERIFIED : AActor/APawn in the SDK
// CXXHeaderDump ([[feedback-verify-game-domain-facts]]) -- agent ac176fda + the design doc
// research/findings/votv-b3b-worldactor-mirror-design-2026-06-17.md. Matched by name (no host-allocated
// wire id), but kept curated. v1 ships the high-value subset live + the rest behind smoke confirmation
// of their spawn path (kavotia/piramidTest possession-risk classes are NOT here -- they go to B3a/verify).
inline constexpr const wchar_t* kWorldActorAllowlist[] = {
    L"ufoDropper_body_C",          // ufoDropper delivery saucer body (the gray "drop pod")
    L"ufoDropper_car_C",           // ufoDropper car variant
    L"ufoDropper_tank_C",          // ufoDropper tank variant
    L"rozitBorg_C",                // Rozital mothership (borgRozital event)
    L"arirShip_C",                 // ariral ship
    L"skyUfo_C",                   // high-altitude sky UFO
    L"jellyfish_C",                // space jellyfish
    L"morningUfo_C",               // morningGay event UFO
    L"tentacleBallsFollower_C",    // tentacle-balls follower
    L"soltomiaCleaning_C",         // soltomia cleaning craft
    L"kocker_C",                   // kocker
    L"skyFallingEvent_C",          // "Sky Falling" event body (AskyFallingEvent_C; distinct from skyFallingEvent's emitter cue)
    L"superEgger_C",               // super egger
    L"igetis_C",                   // igetis
    L"firetank_C",                 // firetank (APawn -- transform-only mirror is layout-agnostic)
    L"piramidSubpawn_C",           // pyramid sub-pawn (APawn; the SANDBOX piramidTest_C child, not the event)
    L"piramid2_C",                 // walking pyramid EVENT actor (plain AActor, host-random path; mirror
                                   // brain suppression + gather relay = coop/creatures/piramid_sync, v97)
};
inline constexpr size_t kWorldActorAllowlistSize =
    sizeof(kWorldActorAllowlist) / sizeof(kWorldActorAllowlist[0]);

// kAmbientPropSpawnMirrorClasses (M2 HostSpawnWatcher, 2026-06-11): the transient
// physics props the host's ambient spawners materialize via
// BeginDeferredActorSpawnFromClass whose own Init is BP-internal/unobservable
// (the pinecone-scare finding -- the spawner's Init dispatches via
// EX_LocalVirtualFunction -> ProcessInternal, bypassing our ProcessEvent detour;
// empirically confirmed by the pinecone_probe). The SPAWN CALL is observable, so
// coop/host_spawn_watcher catches it at the BeginDeferred POST seam and mirrors
// the prop host->client via the PropSpawn pipeline (key=None, eid-routed -- the
// trash-clump precedent), then death-watches its SetLifeSpan(600) expiry ->
// PropDestroy. These are the pineconeSpawner_C outputs (ambient_spawner_suppress.cpp:14):
// keyless + transient -- never the keyed/saved path, so the keyless-eid identity
// is correct. Local fall physics diverges per peer and that is FINE (user
// 2026-06-11: "local physics of falling objects doesn't need syncing"). Exact-
// pointer matched (no hierarchy walk) -- leaf classes with no in-scope subclasses.
inline constexpr const wchar_t* kAmbientPropSpawnMirrorClasses[] = {
    L"prop_food_pinecone_C",  // the scare: spawns high, drops, bounces, rolls / lies
    L"prop_stick_C",          // pineconeSpawner forage branch
    L"prop_crystal_C",        // pineconeSpawner forage branch
};
inline constexpr size_t kAmbientPropSpawnMirrorClassesSize =
    sizeof(kAmbientPropSpawnMirrorClasses) / sizeof(kAmbientPropSpawnMirrorClasses[0]);

inline constexpr const wchar_t* BeginDeferredSpawnFn = L"BeginDeferredActorSpawnFromClass";
inline constexpr const wchar_t* FinishSpawningActorFn = L"FinishSpawningActor";

// Story-save load: VOTV's own entry path (GameplayStatics::LoadGameFromSlot ->
// mainGameInstance_C::setSaveSlotObject + loadObjects=true -> open untitled_1).
// The gameplay map is untitled_1 for ALL modes; the SAVE selects story vs sandbox.
inline constexpr const wchar_t* LoadGameFromSlotFn = L"LoadGameFromSlot";
inline constexpr const wchar_t* SetSaveSlotObjectFn = L"setSaveSlotObject";

// Live host-world capture (save_capture.cpp): repopulate the world save from LIVE
// actors, then serialize it to a SCRATCH slot for a joiner -- the proper fix for
// save-transfer staleness (a kerfur the host turned on after its last autosave is
// captured live as an NPC, so the joiner's native load needs no reconcile).
// All on AmainGamemode_C except SaveGameToSlot (UGameplayStatics, the serializer).
inline constexpr const wchar_t* MainGamemodeSaveObjectsFn  = L"saveObjects";   // saveObjects(bool quicksave)
inline constexpr const wchar_t* MainGamemodeSaveTriggersFn = L"saveTriggers";  // saveTriggers()
inline constexpr const wchar_t* SaveGameToSlotFn           = L"SaveGameToSlot";  // (USaveGame*, FString slot, int32 idx) -> bool
inline constexpr const wchar_t* ActorClassName = L"Actor";  // owns K2_Get/SetActorLocation
inline constexpr const wchar_t* GetActorLocationFn = L"K2_GetActorLocation";
inline constexpr const wchar_t* GetActorRotationFn = L"K2_GetActorRotation";
inline constexpr const wchar_t* GetActorVelocityFn = L"GetVelocity";  // AActor::GetVelocity -> FVector (cm/s)
inline constexpr const wchar_t* GetActorScale3DFn = L"GetActorScale3D";  // AActor -> FVector (v54 PropSpawn real-scale stamp)
inline constexpr const wchar_t* CollectGarbageFn = L"CollectGarbage";  // KismetSystemLibrary static; schedules a full GC purge end-of-frame (post-sweep plateau collapse). Class constant @ the existing KismetSystemLibraryClass above.
inline constexpr const wchar_t* PropInventoryContainerPlayerClass = L"prop_inventoryContainer_player_C";  // PER-PLAYER inventory container -- never snapshot-expressed/broadcast/swept (prop_lifecycle::IsPerPlayerPropClass)
inline constexpr const wchar_t* GetActorForwardVectorFn = L"GetActorForwardVector";
inline constexpr const wchar_t* SetActorRotationFn = L"K2_SetActorRotation";
inline constexpr const wchar_t* SetActorScale3DFn = L"SetActorScale3D";  // AActor::SetActorScale3D(FVector NewScale3D) -> void (v83 trash-proxy per-form scale)
inline constexpr const wchar_t* SetActorTickEnabledFn = L"SetActorTickEnabled";
inline constexpr const wchar_t* SetActorHiddenInGameFn = L"SetActorHiddenInGame";        // AActor::SetActorHiddenInGame(bool bNewHidden) -- visual-only (instant-world deferred-hide)
inline constexpr const wchar_t* SetActorEnableCollisionFn = L"SetActorEnableCollision";  // AActor::SetActorEnableCollision(bool bNewActorEnableCollision) -- paired with hide so a hidden mirror is not grabbable
inline constexpr const wchar_t* DestroyActorFn = L"K2_DestroyActor";
inline constexpr const wchar_t* TeleportToFn = L"K2_TeleportTo";  // bool(FVector, FRotator); large-distance teleport that survives Character/CMC constraints
inline constexpr const wchar_t* GetActorBoundsFn = L"GetActorBounds";   // (bool,FVector&,FVector&,bool); world-space AABB of the actor's mesh

// Controller removal -- a remote pawn auto-acquires its OWN PlayerController on
// spawn (mainPlayer_C auto-possesses Player0), and that 2nd controller fights the
// LOCAL player's input/view. Strip it: GetController -> Detach -> destroy.
inline constexpr const wchar_t* PawnClassName = L"Pawn";
inline constexpr const wchar_t* AIControllerClassName = L"AIController";  // /Script/AIModule.AIController
inline constexpr const wchar_t* GetControllerFn = L"GetController";
inline constexpr const wchar_t* DetachFromControllerFn = L"DetachFromControllerPendingDestroy";
// Give the remote pawn an AIController (poses the body via the possession drive
// path) WITHOUT a PlayerController's viewport/input/camera -> no local hijack.
inline constexpr const wchar_t* SpawnDefaultControllerFn = L"SpawnDefaultController";

// SetSkeletalMesh is owned by USkinnedMeshComponent; SetAnimClass by
// USkeletalMeshComponent (FindFunction does NOT climb to super, so each is
// resolved from its OWN class). The SkeletalMeshActorClass name constant
// was retired with the H9 puppet-kind cleanup (2026-05-27).
inline constexpr const wchar_t* SkeletalMeshComponentClass = L"SkeletalMeshComponent";
inline constexpr const wchar_t* SkinnedMeshComponentClass = L"SkinnedMeshComponent";
inline constexpr const wchar_t* SetSkeletalMeshFn = L"SetSkeletalMesh";   // USkinnedMeshComponent
inline constexpr const wchar_t* SetAnimClassFn = L"SetAnimClass";         // USkeletalMeshComponent

// UStaticMeshComponent::SetStaticMesh -- the trash-proxy mirror's mesh swap (the
// host-authoritative AStaticMeshActor re-skins pile<->clump by setting the mesh
// the client resolves from chipType via getChipPileType). FindFunction does NOT
// climb to super, so SetStaticMesh resolves from its OWN owning class.
inline constexpr const wchar_t* StaticMeshComponentClass = L"StaticMeshComponent";
inline constexpr const wchar_t* SetStaticMeshFn = L"SetStaticMesh";       // UStaticMeshComponent

// Bug 2 Plan B1: BUA-interceptor target. The kerfur AnimBP class (its
// AnimBlueprintGeneratedClass leaf name; the AnimBP source asset is
// 'AnimBlueprint_kerfurOmega_regular' but the generated UClass adds the '_C'
// suffix UE4 uses for BP-generated classes). BlueprintUpdateAnimation is the
// per-frame BP event whose body writes spd=0 on null-Pawn puppets.
inline constexpr const wchar_t* AnimBPKerfurRegularClass = L"AnimBlueprint_kerfurOmega_regular_C";
inline constexpr const wchar_t* BlueprintUpdateAnimationFn = L"BlueprintUpdateAnimation";

// Dev freecam: a spawned ACameraActor we point the view at (SetViewTargetWithBlend),
// look driven by the game's own control rotation (smooth), moved by WASD per frame.
inline constexpr const wchar_t* CameraActorClass = L"CameraActor";
inline constexpr const wchar_t* ControllerClassName = L"Controller";
inline constexpr const wchar_t* GetControlRotationFn = L"GetControlRotation";
// Audit H4 (2026-05-27): use the BlueprintCallable setter, not a direct field
// write. UE4's K2_SetControlRotation runs ProcessViewRotation +
// UpdateRotation in addition to the field assignment -- skipping those caused
// per-tick view jitter on the puppet controller cache wired in commit
// 0a44b06.
inline constexpr const wchar_t* SetControlRotationFn = L"K2_SetControlRotation";
inline constexpr const wchar_t* PlayerControllerClassName = L"PlayerController";
// APlayerController::ProjectWorldLocationToScreen(FVector World, FVector2D& Screen,
// bool bPlayerViewportRelative)->bool. World->viewport-pixel projection for the
// ImGui screen-space nameplates (returns false when the point is behind the camera).
inline constexpr const wchar_t* ProjectWorldToScreenFn = L"ProjectWorldLocationToScreen";
inline constexpr const wchar_t* SetViewTargetWithBlendFn = L"SetViewTargetWithBlend";
inline constexpr const wchar_t* PlayerCameraManagerClass = L"PlayerCameraManager";
inline constexpr const wchar_t* GetCameraLocationFn = L"GetCameraLocation";
inline constexpr const wchar_t* GetCameraRotationFn = L"GetCameraRotation";

// Component visibility (USceneComponent BlueprintCallable) -- to force the
// third-person body meshes visible on an unpossessed remote pawn.
inline constexpr const wchar_t* SceneComponentClass = L"SceneComponent";
inline constexpr const wchar_t* SetVisibilityFn = L"SetVisibility";
// Audit H7 (2026-05-27): K2_SetRelativeRotation runs the full transform
// propagation pipeline (UpdateComponentToWorld -> child re-eval). The
// alternative -- a direct write to RelativeRotation -- skips the pipeline;
// the spring arm's parent re-eval IS driven by its TickComponent so the
// direct write isn't a functional bug, but it bypasses any future child
// dependency on the engine's rotation-changed delegate.
inline constexpr const wchar_t* SetRelativeRotationFn = L"K2_SetRelativeRotation";
inline constexpr const wchar_t* SetHiddenInGameFn = L"SetHiddenInGame";
inline constexpr const wchar_t* GetComponentLocationFn = L"K2_GetComponentLocation";
inline constexpr const wchar_t* GetComponentForwardFn = L"GetForwardVector";
// v40 kerfur BODY-facing sync: the kerfur actor BP rotates its ACharacter::Mesh component's
// WORLD rotation each tick to face the LOCAL player (ExecuteUbergraph_kerfurOmega -> floor_lerp);
// the mirror's actor tick is OFF so that never runs there. We read the resolved component world
// rotation on the host (K2_GetComponentRotation) + drive it on the mirror (K2_SetWorldRotation).
// Both are USceneComponent BlueprintCallable (Engine.hpp:17941/17950).
inline constexpr const wchar_t* GetComponentRotationFn = L"K2_GetComponentRotation";
inline constexpr const wchar_t* SetWorldRotationFn = L"K2_SetWorldRotation";
inline constexpr const wchar_t* CameraComponentClass = L"CameraComponent";

// Component destruction (UActorComponent::K2_DestroyComponent(Object)) -- to
// remove the local-only systems a remote pawn must NOT own (its unbound
// PostProcessComponent stomps the local screen's gamma/exposure).
inline constexpr const wchar_t* ActorComponentClass = L"ActorComponent";
inline constexpr const wchar_t* DestroyComponentFn = L"K2_DestroyComponent";
inline constexpr const wchar_t* PostProcessComponentClass = L"PostProcessComponent";

// Nameplate via a world-space UWidgetComponent (TRANSLUCENT). TextRender can't do
// partial alpha (UnlitText is Masked). A WidgetComponent renders its UMG to a render
// target composited with /Engine/EngineMaterials/Widget3DPassThrough_Translucent
// (BlendMode must be set Transparent -- ctor defaults Masked). We build our OWN UMG
// (UUserWidget+UTextBlock via SpawnObject) -- no cooked-widget dependency.
inline constexpr const wchar_t* WidgetComponentClass = L"WidgetComponent";
inline constexpr const wchar_t* AddComponentByClassFn = L"AddComponentByClass";      // on Actor
inline constexpr const wchar_t* FinishAddComponentFn = L"FinishAddComponent";        // on Actor
inline constexpr const wchar_t* SetTintColorAndOpacityFn = L"SetTintColorAndOpacity";// FLinearColor
inline constexpr const wchar_t* RequestRedrawFn = L"RequestRedraw";                  // sets bRedrawRequested
inline constexpr const wchar_t* RequestRenderUpdateFn = L"RequestRenderUpdate";      // forces render-state/RT refresh
inline constexpr const wchar_t* SetComponentTickEnabledFn = L"SetComponentTickEnabled";  // on UActorComponent -- a runtime-added WidgetComponent doesn't tick -> never draws its RT
inline constexpr const wchar_t* NameplateSetTextFn = L"SetText";                     // UTextBlock::SetText(FText)
inline constexpr const wchar_t* TextBlockClass = L"TextBlock";
inline constexpr const wchar_t* KismetTextLibraryClass = L"KismetTextLibrary";
inline constexpr const wchar_t* ConvStringToTextFn = L"Conv_StringToText";           // FString -> FText
inline constexpr const wchar_t* WidgetBaseClass = L"Widget";                         // UWidget (owns SetVisibility; SetVisibilityFn defined above)
// World-space WidgetComponent translucent material (the slot a runtime-added
// component may have null -> blank quad). bIsTwoSided=true uses the two-sided slot.
inline constexpr const wchar_t* Widget3DTranslucentMatName = L"Widget3DPassThrough_Translucent";
inline constexpr const wchar_t* Widget3DTranslucentOneSidedMatName = L"Widget3DPassThrough_Translucent_OneSided";
inline constexpr const wchar_t* MaterialInstanceConstantClass = L"MaterialInstanceConstant";  // class of those MICs

// RULE-1 nameplate: build our OWN UMG (no cooked-widget reuse). NewObject is the
// reflected UFunction UGameplayStatics::SpawnObject(objectClass, Outer). We make a
// UUserWidget -> UWidgetTree -> UTextBlock (root), then UWidgetComponent::SetWidget.
inline constexpr const wchar_t* SpawnObjectFn = L"SpawnObject";        // on GameplayStatics: (objectClass, Outer)->UObject*
inline constexpr const wchar_t* UserWidgetClass = L"UserWidget";
inline constexpr const wchar_t* WidgetTreeClass = L"WidgetTree";
// Two-line nameplate (nick + a separately-coloured health bar): a UVerticalBox
// root stacks two UTextBlocks so each can carry its OWN ColorAndOpacity (a single
// UTextBlock is one colour). AddChildToVerticalBox(UWidget*) is the BP-callable
// add (returns the slot); default slot HAlign=Fill + per-block centre
// justification centres each line, so no slot-offset writes are needed.
inline constexpr const wchar_t* VerticalBoxClass = L"VerticalBox";
inline constexpr const wchar_t* AddChildToVerticalBoxFn = L"AddChildToVerticalBox";  // UVerticalBox::AddChildToVerticalBox(UWidget*)->UVerticalBoxSlot*
inline constexpr const wchar_t* SetWidgetFn = L"SetWidget";            // UWidgetComponent::SetWidget(UUserWidget*)
inline constexpr const wchar_t* FontName = L"Roboto";                  // /Engine/EngineFonts/Roboto.Roboto
inline constexpr const wchar_t* FontClassName = L"Font";
// Screen-space HUD feed (UUserWidget added to the viewport, not a world WidgetComponent).
inline constexpr const wchar_t* AddToViewportFn = L"AddToViewport";        // UUserWidget::AddToViewport(int32 ZOrder)
inline constexpr const wchar_t* RemoveFromViewportFn = L"RemoveFromViewport";  // UUserWidget::RemoveFromViewport()
inline constexpr const wchar_t* WidgetClass = L"Widget";                   // owns SetVisibility (FindFunction = owning class, no super walk)
inline constexpr const wchar_t* WidgetSetVisibilityFn = L"SetVisibility";  // UWidget::SetVisibility(ESlateVisibility); HitTestInvisible=3
inline constexpr const wchar_t* SetPositionInViewportFn = L"SetPositionInViewport";    // UUserWidget(FVector2D Position, bool bRemoveDPIScale)
inline constexpr const wchar_t* SetAlignmentInViewportFn = L"SetAlignmentInViewport";  // UUserWidget(FVector2D Alignment)
// PR-FOUNDATION-2 Inc B part 2: client pause-menu "Save Game" button grey-out.
// All owned by UWidget -> resolve on WidgetClass ("Widget"), FindFunction = owning class.
inline constexpr const wchar_t* WidgetSetIsEnabledFn = L"SetIsEnabled";       // UWidget::SetIsEnabled(bool bInIsEnabled) -- blocks input + semantic disable (NOT a raw 0xB4 write: packed bitfield)
inline constexpr const wchar_t* WidgetSetRenderOpacityFn = L"SetRenderOpacity";  // UWidget::SetRenderOpacity(float InOpacity) -- brush-independent visual dim
inline constexpr const wchar_t* WidgetGetIsEnabledFn = L"GetIsEnabled";       // UWidget::GetIsEnabled()->bool ReturnValue (read-back diagnostic)
inline constexpr const wchar_t* UiMenuClass = L"ui_menu_C";                   // VOTV unified main+pause menu widget (GameInstance-owned, one live instance)
inline constexpr const wchar_t* UiMenuTickFn = L"Tick";                       // ui_menu_C::Tick(FGeometry,float) -- engine ProcessEvent-dispatched while visible (self-heal anchor)
inline constexpr const wchar_t* UiMenuButtonSaveProp = L"button_Save";        // UButton* @ ui_menu_C+0x2D0 (the "Save Game" button)
inline constexpr const wchar_t* UiMenuIsPauseProp = L"isPause";               // bool @ ui_menu_C+0x4C0 (true => the in-game pause menu, not the main menu)

// MULTIPLAYER menu button (P1, 2026-06-04): UMG classes + UFunctions to construct a
// UButton at runtime inside the live menu. Class names are UE FNames (no U-prefix,
// like UserWidget/TextBlock/Widget above). Each UFunction resolves on its OWNING
// class (FindFunction does not super-walk).
inline constexpr const wchar_t* ButtonClass = L"Button";                      // UButton
inline constexpr const wchar_t* PanelWidgetClass = L"PanelWidget";            // UPanelWidget (owns Slots/ClearChildren; base of CanvasPanel/HBox/VBox)
inline constexpr const wchar_t* ContentWidgetClass = L"ContentWidget";        // UContentWidget (owns SetContent; UButton is-a ContentWidget)
inline constexpr const wchar_t* SetContentFn = L"SetContent";                 // UContentWidget::SetContent(UWidget*)->UPanelSlot*
inline constexpr const wchar_t* WidgetIsHoveredFn = L"IsHovered";             // UWidget::IsHovered()->bool ReturnValue
inline constexpr const wchar_t* ClearChildrenFn = L"ClearChildren";           // UPanelWidget::ClearChildren() -- detach all children (objects survive); for the insert-at-top reorder
// ui_menu_C fields for the inject (resolved by FindPropertyOffset -- recook-robust).
inline constexpr const wchar_t* UiMenuButtonStartProp = L"button_start";      // UButton*  @ +0x2E0 (NEW GAME -- the inject derives its VerticalBox + clones its style)
inline constexpr const wchar_t* UiMenuTexBtnStartProp = L"tex_btnStart";      // UTextBlock* @ +0x3A8 (NEW GAME label -- font/color template)
inline constexpr const wchar_t* MainPlayerEscapeFn = L"InpActEvt_Escape_K2Node_InputKeyEvent_0";  // engine input event that opens the pause menu (ProcessEvent-dispatched, same class as the flashlight InpActEvt_* we already observe)
// Head-bone anchoring (USceneComponent::GetSocketLocation world; enumerate bones to find head).
inline constexpr const wchar_t* GetSocketLocationFn = L"GetSocketLocation";  // (FName)->FVector (world)
inline constexpr const wchar_t* GetNumBonesFn = L"GetNumBones";
inline constexpr const wchar_t* GetBoneNameFn = L"GetBoneName";              // (int32)->FName

// Physics-prop pickup interaction. RIGHT HOOK SURFACE -- the engine-native
// UPhysicsHandleComponent UFunctions. Determined empirically (debug build
// 14d0787 hands-on test 2026-05-23): the original SDK-header guesses
// (smoothGrab, pickupObject, dropGrabObject, throwHoldingProp,
// switchToHeavyDrag, pickupObjectDirect, playerTryToGrab, canPickup) ARE
// BP-pure inline functions on mainPlayer_C -- they have valid UFunction
// pointers (FindFunction succeeds) but BP-compiled callers inline them, so
// they NEVER appear as ProcessEvent's `function` arg. Zero observer hits in
// 3 minutes of active grabbing. All deleted per RULE 2.
//
// What DOES dispatch through ProcessEvent during a grab:
//   * InpActEvt_use_K2Node_InputActionEvent_41 on mainPlayer_C  (E press)
//   * grab__UpdateFunc / grab__FinishedFunc on mainPlayer_C
//       (auto-generated by the `grab` UTimelineComponent; BP body calls
//        grabHandle->SetTargetLocation each tick)
//   * UPhysicsHandleComponent.GrabComponentAtLocation* (at pickup)
//   * UPhysicsHandleComponent.SetTargetLocation* (every tick during grab)
//   * UPhysicsHandleComponent.ReleaseComponent (at drop)
//
// We hook the engine-native UFunctions (universal -- captures light grab
// AND heavy drag), with the BP-Timeline ones as secondary diagnostic.
// IDA decompile + log analysis written up in
// research/findings/votv-physics-interaction-deep-re-2026-05-23.md.

// Engine UPhysicsHandleComponent UFunctions (light-grab path; ProcessEvent-
// dispatched -- the exec thunks at rva 0x1430C64B0 / 0x1430C65D0 / 0x1430C6AD0 /
// 0x1430C6B60 / 0x142FEA9B0 are UFunction.Func pointers; ProcessEvent's
// `function` arg IS the UFunction* we match on, looked up by FName via
// reflection::FindFunction(UPhysicsHandleComponent::StaticClass(), name)).
inline constexpr const wchar_t* PhysicsHandleComponentClass            = L"PhysicsHandleComponent";
inline constexpr const wchar_t* GrabComponentAtLocationFn              = L"GrabComponentAtLocation";
inline constexpr const wchar_t* GrabComponentAtLocationWithRotationFn  = L"GrabComponentAtLocationWithRotation";
inline constexpr const wchar_t* SetTargetLocationFn                    = L"SetTargetLocation";
inline constexpr const wchar_t* SetTargetLocationAndRotationFn         = L"SetTargetLocationAndRotation";
inline constexpr const wchar_t* ReleaseComponentFn                     = L"ReleaseComponent";

// Engine UPhysicsConstraintComponent UFunctions (HEAVY drag path -- VOTV uses
// a physics CONSTRAINT for heavy drag, not a handle; SDK dump shows
// mainPlayer_C.heavyGrab is UPhysicsConstraintComponent*, distinct from
// mainPlayer_C.grabHandle which is UPhysicsHandleComponent*). These ARE
// ProcessEvent-dispatched too -- BP calls them via CallFunction nodes.
inline constexpr const wchar_t* PhysicsConstraintComponentClass        = L"PhysicsConstraintComponent";
inline constexpr const wchar_t* SetConstrainedComponentsFn             = L"SetConstrainedComponents";
inline constexpr const wchar_t* BreakConstraintFn                      = L"BreakConstraint";
inline constexpr const wchar_t* SetDisableCollisionFn                  = L"SetDisableCollision";

// Engine UPrimitiveComponent.AddImpulse (THROW path -- VOTV's throwHoldingProp
// is BP-pure inline, but it MUST call AddImpulse on the released prop's
// component to send it flying. AddImpulse IS ProcessEvent-dispatched). Same
// signature for primary owner UE4 4.27: (FVector impulse, FName BoneName,
// bool bVelChange). Used as the throw signal.
inline constexpr const wchar_t* PrimitiveComponentClass                = L"PrimitiveComponent";
inline constexpr const wchar_t* AddImpulseFn                           = L"AddImpulse";

// Secondary -- BP-Timeline / input level. Useful for triangulation and to
// learn the grabbed prop's identity (read mainPlayer_C.grabbing_actor from
// inside the InpActEvt observer just after the BP graph has decided what to
// grab). The Timeline name "grab" is content; the "_41" K2Node ordinal is
// version-fragile (a BP graph re-cook can renumber it).
inline constexpr const wchar_t* MainPlayerGrabUpdateFn       = L"grab__UpdateFunc";
inline constexpr const wchar_t* MainPlayerGrabFinishedFn     = L"grab__FinishedFunc";
inline constexpr const wchar_t* MainPlayerUseInputEventFn    = L"InpActEvt_use_K2Node_InputActionEvent_41";
// The "use" action has THREE delegate bindings (InputActionDelegateBinding, mainPlayer.json Export 483): _41 =
// IE_Pressed (the grab press we intercept), _38 = a SECOND IE_Pressed binding, _42 = IE_Released. All three run
// the use flow that can reach useAction's `use_deny` "EHHH". Hooking only _41 left _38 firing on every E-press
// (playing the deny in parallel with our cancelled grab -- the 19:06 regression). The client suppresses the deny
// on these two extra seams too (side-effect-free cancel; _41 stays the sole grab/throw-intent sender). Ordinals
// are BP-recook-fragile (sdk_check flags it). RE 2026-07-01.
inline constexpr const wchar_t* MainPlayerUseInputEventFn38  = L"InpActEvt_use_K2Node_InputActionEvent_38";
inline constexpr const wchar_t* MainPlayerUseInputEventFn42R = L"InpActEvt_use_K2Node_InputActionEvent_42";
// LMB (fire) input action -- the NATIVE throw-when-holding seam (InpActEvt_fire -> throwHoldingProp; SEPARATE
// from use/E). Two handlers (_58/_59, press/release); the client registers BOTH + gates on ClientCarryEid so
// only the press edge that finds a live carry acts (SendThrowIntent clears the carry -> the other no-ops).
inline constexpr const wchar_t* MainPlayerFireInputEventFn58 = L"InpActEvt_fire_K2Node_InputActionEvent_58";
inline constexpr const wchar_t* MainPlayerFireInputEventFn59 = L"InpActEvt_fire_K2Node_InputActionEvent_59";
// Phase 5F flashlight (item activation sync). Two UFunctions in the chain;
// hands-on testing 2026-05-25 NIGHT-3 showed `updateFlashlight` is BP-INLINED
// into `Flashlight Update` and never dispatches via ProcessEvent (the inner
// observer never fired across a session of F-presses), so we register a
// POST observer on BOTH and whichever actually dispatches wins. The OUTER
// function name has a LITERAL SPACE -- mainPlayer.hpp:488 emitted
// `void Flashlight Update();`, and the FName lookup matches that exact
// string verbatim.
//   InpActEvt_flashlight_K2Node_InputActionEvent_13/14  (F press/release)
//   -> Flashlight Update()    <- dispatched via ProcessEvent
//   -> updateFlashlight()     <- INLINED; never reaches ProcessEvent
inline constexpr const wchar_t* MainPlayerUpdateFlashlightFn = L"updateFlashlight";
inline constexpr const wchar_t* MainPlayerFlashlightUpdateFn = L"Flashlight Update";

// USpotLightComponent inherits ULightComponent::SetIntensity(float)
// (Engine.hpp:13551). The receiver calls this on the puppet's light_R
// to mirror the local sender's intensity -- bVisible alone is
// insufficient (VOTV's BP toggles via Intensity, not bVisible).
// SetIntensityUnits(ELightUnits) on ULocalLightComponent (Engine.hpp:13641)
// is the matching unit-scale setter. Both go through ProcessEvent and
// internally MarkRenderStateDirty on the light proxy -- direct field
// writes do NOT mark dirty so the renderer won't pick up changes.
inline constexpr const wchar_t* SetIntensityFn      = L"SetIntensity";
inline constexpr const wchar_t* SetIntensityUnitsFn = L"SetIntensityUnits";

// USpotLightComponent cone-angle setter UFunctions (Engine.hpp:14080-ish in
// stock UE4.27). Both call MarkRenderStateDirty internally -- the receiver
// calls them to mirror sender's cone shape so the proxy refreshes. Direct
// field writes to OuterConeAngle/InnerConeAngle would NOT update the visible
// cone (no virtual dispatch -> no MarkRenderStateDirty -> proxy keeps old
// values until next unrelated recreate). v6 wire-format requirement.
inline constexpr const wchar_t* SetOuterConeAngleFn = L"SetOuterConeAngle";
inline constexpr const wchar_t* SetInnerConeAngleFn = L"SetInnerConeAngle";

// SpotLight component class for FindFunction on the cone setters (the
// setters are declared on USpotLightComponent itself; ULightComponent
// has SetIntensity, USceneComponent has SetVisibility). FindFunction
// does NOT walk to super, so each setter is resolved from its OWN class.
// SpotLightComponentClass already declared further down (re-used here).

// Phase 5F retest fallback (2026-05-25 NIGHT-3): hands-on showed that
// BOTH updateFlashlight AND 'Flashlight Update' are BP-inlined into
// their callers and never dispatch via ProcessEvent. The remaining
// candidate is the input event itself, which the engine input system
// always dispatches as a UFunction (the grab_observer relies on the
// same shape for InpActEvt_use). The K2Node ordinal (_13 / _14) is
// version-fragile -- a BP recook can renumber it; we follow the same
// brittleness the grab observer accepted with _use_..._41. Two events
// per input (press + release); we register on both and dedup at sender
// via last-sent-state (release fires but the bool didn't change ->
// no-op).
inline constexpr const wchar_t* MainPlayerFlashlightInput13Fn = L"InpActEvt_flashlight_K2Node_InputActionEvent_13";
inline constexpr const wchar_t* MainPlayerFlashlightInput14Fn = L"InpActEvt_flashlight_K2Node_InputActionEvent_14";

// 2026-05-26 PM (v6 cone-shape sync): timerHoldFlashlight fires when the
// player HOLDS F long enough to switch beam mode (default spread <->
// focused). The BP body mutates `flashlightMode` byte + `light_R`'s
// OuterConeAngle/InnerConeAngle/Intensity. POST observer reads the
// post-mutation light state and sends a v6 ItemActivate packet so the
// receiver mirrors the new cone shape on the puppet. Same hook style as
// updateFlashlight / Flashlight Update / InpActEvt_13/14 (mainPlayer.hpp:655).
inline constexpr const wchar_t* MainPlayerTimerHoldFlashlightFn = L"timerHoldFlashlight";

// 2026-05-26 PM (v6 click-sound at puppet): VOTV plays the flashlight
// click as USoundWave `/Game/audio/effects/flashlight.flashlight`
// (confirmed via UE4SS_ObjectDump_GAMEPLAY_SAVE.txt:149975). The
// receiver plays it at the puppet's world location via
// UGameplayStatics::PlaySoundAtLocation so the local player hears the
// click come from the remote player's direction with full 3D
// attenuation. FindObject resolves by leaf name `flashlight` filtered
// by className `SoundWave` (linear GUObjectArray walk; done once at
// first apply and cached).
inline constexpr const wchar_t* FlashlightClickSoundName = L"flashlight";
inline constexpr const wchar_t* SoundWaveClass           = L"SoundWave";
inline constexpr const wchar_t* PlaySoundAtLocationFn    = L"PlaySoundAtLocation";

// USoundWave alone has no spatialization -- PlaySoundAtLocation needs an
// AttenuationSettings override or it plays 2D (same volume everywhere).
// RULE 1 native path 2026-05-26: instead of borrowing VOTV's content
// `att_*` assets (which would couple our code to specific cooked content
// names + their author-chosen radii), we CONSTRUCT a USoundAttenuation
// at runtime via UGameplayStatics::SpawnObject. Field offsets live in
// the `off::att` namespace (block at the top of this file).
inline constexpr const wchar_t* SoundAttenuationClass = L"SoundAttenuation";

// 2026-05-26 deep-RE breakthrough: BP graphs for input handlers + the
// updateFlashlight / 'Flashlight Update' functions all compile to
// tiny bytecode STUBS that call ExecuteUbergraph_mainPlayer via the
// VM's EX_LocalFinalFunction opcode (which bypasses ProcessEvent's
// observer hook, hence our trace caught nothing). So none of these
// stubs are useful to invoke via reflection -- their "real" body is
// in the ubergraph, locked behind controller / inMenu / inventory
// guards that fail for the controllerless puppet.
//
// THE NATURAL MECHANISM (per architectural-agent verdict + Engine.hpp:
// mainPlayer.hpp line 184-185): the BP fires the multicast delegate
// `flashlightStateChanged(USpotLightComponent* Light, bool Visible)`
// after toggling local state. Subscribers (cooked equipment-flashlight
// actor + map-side props + global lighting helpers) listen and DO
// the actual SetVisibility / SetIntensity / MarkRenderStateDirty
// work on the passed `Light`. Broadcasting this delegate FROM the
// puppet's mainPlayer_C with the puppet's `light_R` + new state
// causes the subscribers to apply the SAME work they do on the local
// peer -- including any side-effects (sound, particles, save-state
// updates) that match the user-observed local rendering.
inline constexpr const wchar_t* MainPlayerFlashlightStateChangedFn = L"flashlightStateChanged";

// Vitals pillar Inc2b (2026-05-31, ragdoll/faint DISPLAY sync). mainPlayer_C
// drives every ragdoll cause (manual C-key, exhaustion faint, KO) through ONE
// UFunction `ragdollMode(bool ragdoll, bool passOut, bool death)` (mainPlayer.hpp:
// 484) and recovers via `forceGetUp()` (line 641). MUST-VERIFY #8 proved both
// drive an UNPOSSESSED puppet (commit 4d52d40): ragdollMode(true,true,false)
// flips isRagdoll 0->1 + spawns the ragdoll actor; forceGetUp() clears the
// AnimBP gate. The receiver reconcile applies exactly those calls.
inline constexpr const wchar_t* MainPlayerRagdollModeFn = L"ragdollMode";
inline constexpr const wchar_t* MainPlayerForceGetUpFn  = L"forceGetUp";

// vitals Inc3-WIRE: the primary player-damage entry. The OWNER peer invokes this
// on its OWN possessed mainPlayer_C when the host relays an enemy hit, so the
// damage runs through that peer's private armor/inventory BP (per-peer-authoritative)
// -> its saveSlot.health drops -> the existing Inc1 stream + Inc3 flash fire. The #6
// probe (VOTVCOOP_RUN_DMGHAZARD_TEST) proved Add Player Damage(5) on the possessed
// player drops health 100->95.05 (and early-outs harmlessly on an unpossessed puppet).
// FName has spaces (a BP "Add Player Damage" event keeps them).
inline constexpr const wchar_t* MainPlayerAddPlayerDamageFn = L"Add Player Damage";

// Inc3 damage body-pulse: the puppet's skin (inst_kel4_*) exposes no drivable
// tint param (probe 2026-05-31), so the hurt-flash SWAPS the body mesh materials
// to an EXISTING pak material (no asset edit -- Principle 1), then restores the
// originals. The swap is the Minecraft-style "whole body flashes red" effect.
// UMeshComponent material UFunctions live on UPrimitiveComponent.
//
// MUST be a SKELETAL-character material: static-mesh/particle materials (e.g.
// inst_color_red, inst_redDwarf) have no GPUSkinVertexFactory shader permutation,
// so UE substitutes its DEFAULT GREY material on the skinned kerfur (confirmed
// 2026-05-31: user saw "grey solid", not red). inst_goregibs_organsSK is a gore
// SKIN material ("SK" == skeletal) -> it renders its real bloody-red texture on
// the kerfur. Swapped onto BOTH visible body meshes (Mesh@0x280 +
// mesh_playerVisible) since the puppet renders both.
inline constexpr const wchar_t* PlayerHurtFlashMaterialName = L"inst_goregibs_organsSK";
inline constexpr const wchar_t* MaterialInstanceConstantClassName = L"MaterialInstanceConstant";

// 2026-05-26 deep-RE puppet light fix: reuse AddComponentByClassFn +
// FinishAddComponentFn (already defined above for nameplate WidgetComponent
// spawning). We spawn a FRESH USpotLightComponent on the puppet via
// this path -- the only reflection-callable mechanism that internally
// runs UActorComponent::RegisterComponent (which creates the
// FSceneProxy). The existing class-default light_R has a permanently-
// null SceneProxy that no reflection-only write can revive.
inline constexpr const wchar_t* SpotLightComponentClass = L"SpotLightComponent";

// Phase 5F autotest helpers (2026-05-26): give/equip/battery setup.
// AmainPlayer_C::addPropToPlayer(FName prop) -- cheat-menu-equivalent
// path. Spawns the actor + adds to player inventory + (per F-INV-2
// open flag) MAY auto-equip when it's an equipment item.
// See research/findings/votv-inventory-equip-battery-RE-2026-05-26.md.
inline constexpr const wchar_t* MainPlayerAddPropToPlayerFn = L"addPropToPlayer";

// Battery item class for the standard flashlight (Aprop_batts_C).
// We write a TSubclassOf<Aprop_batts_C> pointer into saveSlot.flashlightBattery
// to mark "a battery is inserted"; the class CDO is fine (it's a class ref,
// not an instance).
inline constexpr const wchar_t* BattsClass = L"prop_batts_C";

// Flashlight equipment class (the FName the BP uses to identify the item).
inline constexpr const wchar_t* FlashlightEquipmentClass = L"prop_equipment_flashlight_C";

// UTimelineComponent BP-callable methods (used to force the `grab` Timeline
// from the autonomous test, so the 3 BP-Timeline observers fire without a
// real E-press). These ARE ProcessEvent-dispatched (BP CallFunction nodes).
inline constexpr const wchar_t* TimelineComponentClass       = L"TimelineComponent";
inline constexpr const wchar_t* TimelinePlayFromStartFn      = L"PlayFromStart";
inline constexpr const wchar_t* TimelineStopFn               = L"Stop";

// Aprop_C BP class name (for PropKeyRegistry GUObjectArray scan, Stage 4).
inline constexpr const wchar_t* PropClass             = L"prop_C";

// `Aprop_C.thrown(AmainPlayer_C* Player)` -- the natural throw event the
// prop's BP wires to throw-whoosh sound + particle-trail activation.
// Calling this on the receiver as part of OnRelease (when impulse non-zero)
// makes the sound/effects fire identically to a local throw -- RULE 1
// natural dispatch, NOT a separate sound-trigger crutch. Per prop.hpp:166.
inline constexpr const wchar_t* PropThrownFn          = L"thrown";

// Phase 5S0 Inc2: Aprop_C.Init is the BP UserConstructionScript that runs
// on every Aprop spawn (FinishSpawningActor path). POST-hooking it on the
// PropClass base gives us a single observer that catches EVERY Aprop
// derivative spawn -- mushroom growth, save-load (filtered by
// !g_session.connected()), inventory drops on the spawning peer, etc.
// HOST broadcasts; CLIENT skips (host-authoritative).
inline constexpr const wchar_t* PropInitFn            = L"Init";

// Phase 5N Stream B (host-authoritative spawners 2026-05-24): classes that
// represent INTERMEDIATE STATE for a host-authoritative pipeline. Mushrooms
// have a 2-class growth state machine: mushroom7_C (growing, hidden,
// collision-off) -> mushroom_C (mature) via timer-driven Transform.
// Per mushroom-state RE: growth is encoded by CLASS IDENTITY, not a field.
//
// On CLIENT: any local spawn of these classes (from per-peer spawners) is
// DESTROYED on Init POST. Any wire-received PropSpawn for these classes is
// DROPPED in event_feed. The mature variant flows through normally via
// Inc2 broadcast when host's growth timer transforms mushroom7_C -> mushroom_C.
//
// Extensible -- add other intermediate-variant classes as identified in
// future RE (e.g. growable plants, food spoilage transitions). Each entry
// is the BP class FName (no A- prefix; matches sdk_profile convention).
inline constexpr const wchar_t* PropMushroomGrowingClass = L"prop_food_mushroom7_C";

// v5 Bug C (inventory drop): the BP function that all 4 drop paths funnel
// through (per research/findings/votv-inventory-drop-spawn-RE-2026-05-24.md).
// We POST-hook this on UpropInventory_C and read the out-params to broadcast
// PropSpawn. ON the receiver, Aprop_C.setKey is dispatched between Begin and
// Finish spawning so the prop's Init() doesn't overwrite Key with NewGuid.
inline constexpr const wchar_t* PropInventoryClass      = L"propInventory_C";
inline constexpr const wchar_t* PropInventoryTakeObjFn  = L"takeObj";
inline constexpr const wchar_t* PropSetKeyFn            = L"setKey";

// UKismetStringLibrary::Conv_StringToName(const FString&) -> FName.
// BlueprintPure, ProcessEvent-dispatched on the KismetStringLibrary CDO.
// Used by remote_prop::OnSpawn to convert the wire Key string into a live
// FName (registers in the engine's FName pool with a fresh local index),
// then passed to Aprop_C.setKey on the just-spawned actor BEFORE
// FinishSpawningActor runs Init() so the Init's NewGuid branch is skipped.
inline constexpr const wchar_t* KismetStringLibraryClass = L"KismetStringLibrary";
inline constexpr const wchar_t* ConvStringToNameFn       = L"Conv_StringToName";

// Native UPrimitiveComponent UFunctions used by the receiver-side puppet
// path to put a held prop into kinematic mode + apply inherited physics
// velocity. These ARE ProcessEvent-dispatchable (native exec thunks already
// RVA'd in IDB by the IDA agent: SetPhysicsLinearVelocity @ 0x1430DFA40,
// SetPhysicsAngularVelocityInDegrees @ 0x1430DF7B0,
// GetPhysicsLinearVelocity @ 0x1430DC550,
// GetPhysicsAngularVelocityInDegrees @ 0x1430DC320 -- see
// research/findings/votv-throw-release-pipeline-RE-2026-05-24.md). Host reads
// GetPhysics* on the held mesh AT THE RELEASE EDGE to capture the body's
// inherited tracking velocity; receiver writes the matching SetPhysics*
// AFTER SetSimulatePhysics(true) so the body re-enters dynamic sim with the
// correct launch state.
inline constexpr const wchar_t* SetSimulatePhysicsFn                 = L"SetSimulatePhysics";
inline constexpr const wchar_t* GetPhysicsLinearVelocityFn           = L"GetPhysicsLinearVelocity";
inline constexpr const wchar_t* GetPhysicsAngularVelocityInDegreesFn = L"GetPhysicsAngularVelocityInDegrees";
inline constexpr const wchar_t* SetPhysicsLinearVelocityFn           = L"SetPhysicsLinearVelocity";
inline constexpr const wchar_t* SetPhysicsAngularVelocityInDegreesFn = L"SetPhysicsAngularVelocityInDegrees";

// Phase 5W (2026-05-26) weather. AdaynightCycle_C is the singleton weather
// authority -- owns all scheduler timers + state fields + mutator UFunctions.
// Resolved on the receiver via R::FindObjectByClass since it's the only
// instance per session. See research/findings/votv-weather-DESIGN-2026-05-26.md.
inline constexpr const wchar_t* DaynightCycleClass = L"daynightCycle_C";

// Phase 5W Inc2 (2026-05-27) lightning: AlightningStrike_C is the discrete
// one-shot strike actor (size 0x280 per the RE doc; 3 audio components +
// point light + sphere collider + timeline self-destruct). Its location is
// the actor's own world transform -- there's no separate Location field.
// Receiver spawns via BeginDeferredActorSpawnFromClass + FinishSpawningActor
// (same pattern as remote_prop.cpp for inventory drops).
inline constexpr const wchar_t* LightningStrikeClass = L"lightningStrike_C";

// Phase 5W fog (2026-06-01): AsuperFog_C is the dense "super fog" event actor.
// Unlike the rolling fog (held in fogEventObject@0x0338), the super-fog actor is
// NOT stored on the cycle -- its existence IS its active state, so the receiver
// finds + destroys live instances via FindObjectByClass / CountObjectsByClass.
inline constexpr const wchar_t* SuperFogClass = L"superFog_C";

// AdirectionalWind_C -- the wind actor (singleton). Resolved by class like the cycle
// (FindObjectByClass) for the host-authoritative wind sync (coop/weather_sync via
// ue_wrap/directionalwind). RE: research/findings/votv-wind-basefog-RE-2026-06-08.md.
inline constexpr const wchar_t* DirectionalWindClass = L"directionalWind_C";

// Scheduler UFunctions -- client INTERCEPTS these (PRE-cancel via the
// multi-slot interceptor) so the client side never decides "rain now" / etc.
// Host's POST observer on the same UFunctions reads the post-mutation state
// off the cycle and broadcasts the WeatherState packet.
inline constexpr const wchar_t* DaynightCycle_timerRainFn       = L"timerRain";
inline constexpr const wchar_t* DaynightCycle_timerLightningFn  = L"timerLightning";
inline constexpr const wchar_t* DaynightCycle_fogEventFn        = L"fogEvent";
inline constexpr const wchar_t* DaynightCycle_superFogEventFn   = L"superFogEvent";
inline constexpr const wchar_t* DaynightCycle_permaRainTimerFn  = L"permaRain_timer";

// Mutator UFunctions -- receiver invokes these to apply the host's state.
// causeRain(bool) starts/stops the visible rain (particle + audio + bool flip
// on the cycle). setRainProperties(bool, 4 floats) writes the scalar state
// block + drives setRainParameters internally (RE doc). intComs_triggerSnow
// fans out to 53 BP listeners -- direct field write would miss them.
// setWindParameters() is no-arg; reads cycle state, propagates to
// AdirectionalWind_C. spawnFog spawns the rolling-fog actor (used by
// coop/weather_fog for the host-authoritative mirror-spawn + suppressed on the
// client). SetFogDensity is NOT used by the shipping fog path (the clear is a
// plain K2_DestroyActor; density settles to the shared ambient via the cycle's
// own ReceiveTick) -- the constant remains only for the dev fogprobe.
inline constexpr const wchar_t* DaynightCycle_causeRainFn          = L"causeRain";
inline constexpr const wchar_t* DaynightCycle_setRainPropertiesFn  = L"setRainProperties";
inline constexpr const wchar_t* DaynightCycle_setWindParametersFn  = L"setWindParameters";
inline constexpr const wchar_t* DaynightCycle_intComsTriggerSnowFn = L"intComs_triggerSnow";
inline constexpr const wchar_t* DaynightCycle_spawnFogFn           = L"spawnFog";
inline constexpr const wchar_t* DaynightCycle_setFogDensityFn      = L"SetFogDensity";  // dev fogprobe only
// 2026-05-27 Inc1 fix: setRainParticles is the BP function that DIRECTLY
// activates / deactivates the UParticleSystemComponent rainEffect on the
// cycle. causeRain triggers the BP transition (audio + ambient + bool flip)
// but does NOT always activate the particle system reliably -- the RE doc
// flagged this as a fallback path. Receiver calls this after causeRain to
// guarantee the visible particle component matches state.
inline constexpr const wchar_t* DaynightCycle_setRainParticlesFn   = L"setRainParticles";

// Phase 5W Inc-fix-1 (2026-05-27): direct UParticleSystemComponent::Activate
// path on the cycle's eff_rain component. Per
// research/findings/votv-weather-RE-rendering-2026-05-27.md the previous
// receiver chain (causeRain UFunction) does NOT reliably activate eff_rain
// on the client because causeRain's BP body is a Random-roll function, not
// a particle-Activate dispatcher. Bypass it entirely: direct memory writes
// for the bools + scalars, then call eff_rain->Activate() directly.
// Activate is the engine-inherited UActorComponent UFunction; deterministic.
inline constexpr const wchar_t* ParticleSystemComponentClass    = L"ParticleSystemComponent";
inline constexpr const wchar_t* ActorComponent_ActivateFn       = L"Activate";
inline constexpr const wchar_t* ActorComponent_DeactivateFn     = L"Deactivate";

// Phase 5W Inc-fix-2 (2026-05-27): red sky path on AmainGamemode_C.
// mainGamemode.spawnRedSky() spawns the redSkyEvent_C actor and stashes
// the pointer at mainGamemode.redSky @0x0888 (per mainGamemode.hpp:150).
// Subsequent toggles call redSky.set(bool isred) which swaps the 4
// color-curve assets on the daynightCycle. set has 19 locals, all
// deterministic curve-pointer selects (no Random; verified IDA RE
// 2026-05-27).
inline constexpr const wchar_t* MainGamemode_SpawnRedSkyFn      = L"spawnRedSky";
inline constexpr const wchar_t* RedSkyEventClass               = L"redSkyEvent_C";
inline constexpr const wchar_t* RedSkyEvent_SetFn              = L"set";
// SetCollisionEnabled lives on UPrimitiveComponent; used by remote_prop::OnSpawn
// to restore default collision (QueryAndPhysics=3) on wire-converged props
// whose local copy had collision disabled by a natural-spawn pipeline (e.g.
// Aprop_food_mushroom_C goes through spawnedNaturally() -> NoCollision on the
// spawning peer). Param is `NewType` (ECollisionEnabled::Type, uint8 enum).
inline constexpr const wchar_t* SetCollisionEnabledFn                = L"SetCollisionEnabled";
// The class that overrides Aprop_C::Init AND triggers spawnedNaturally() ->
// NoCollision on its StaticMesh in the natural-spawn path
// (AmushroomSpawner_C::Spawn). 2026-05-25 RE
// (research/findings/votv-mushroom-fall-through-RE-2026-05-25.md): this is
// the actual cap-mushroom class. mushroom7_C (PropMushroomGrowingClass) grows
// into prop_puffballMature_C, NOT this -- separate species.
inline constexpr const wchar_t* PropFoodMushroomClass                = L"prop_food_mushroom_C";
}  // namespace name

}  // namespace ue_wrap::profile
