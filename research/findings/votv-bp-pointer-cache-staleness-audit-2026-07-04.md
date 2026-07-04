# BP-pointer cache staleness audit (2026-07-04) — the re-host dangling-pointer CLASS

Status: AUDIT COMPLETE (agent sweep of src/votv-coop/src/ue_wrap, 2026-07-04, run as part of
the re-host crash fix). The PROVEN crash instance (g_storySave -> gameInstance.saveSlotObject)
is FIXED (campaign-scoped cache, engine.cpp, DLL `D43455FC4787FFE4`). Everything below is the
LATENT remainder of the same class: fix queued as its own thread (design choice: world-boundary
latch reset vs per-call re-resolution).

> [SECOND LIVE KILL + 4 sites closed, 2026-07-04 ~15:00. The class killed a client session
> live: quit-to-menu via the game's OWN menu ran a flee path with NO subsystem teardown
> (fixed: TearDownCoopStateForSessionEnd, net_pump, `99253486`), and a queued weather apply
> then hit the old daynightCycle's RECYCLED GUObjectArray slot -- the cycle BP body executed
> with a foreign `self`, and its by-name EX_LocalVirtualFunction("setRainProperties") lookup
> landed on an ArrowComponent -> LowLevelFatalError (FindFunctionChecked, ScriptCore:1334).
> KEY AMENDMENT to "Verified-SAFE families": plain-IsLive instance caches are NOT safe in a
> TEARDOWN WINDOW -- IsLive is recycled-slot-BLIND (a foreign live occupant at the same
> address/index passes; reflection.cpp:166-170 documents the caveat). IsLiveByIndex
> (serial slot-compare) is the required shape for any cached instance used across a session
> end. Hardened by-index in `1e6c86ea`: ue_wrap/daynightcycle.cpp Cycle() (the seam all
> time users share), npc_mirror.cpp DrainClientMirrors, remote_prop.cpp ForceRelease +
> OnDisconnectForSlot (ActiveDrive now carries grab-time actorIdx). weather_sync's own
> g_cycleCache remains plain-IsLive but is now protected by the teardown running
> OnDisconnect (clears the cache) at every session-end path.]

## The class

A BP-owned pointer (UFunction* / UClass* / BP CDO / asset) latched into a process-lifetime
global on first resolve and never revalidated. BP classes and their functions CAN be GC'd on a
world unload (gameplay -> menu -> gameplay = re-host / rejoin) and reload at a different
address; the cached pointer then dangles. Native classes/CDOs/UFunctions (GameplayStatics,
KismetSystemLibrary, AActor, UPrimitiveComponent...) are rooted -- caching those is SAFE.

Aggravator (call.cpp:15-16, 36-81): Call() dispatches the cached UFunction* directly, and
GetOrBuildMetadata caches ParamFrame metadata keyed by the fn pointer for the process lifetime
("UFunctions are never GC'd during a running game" -- the exact assumption re-host violates).
A reused GUObjectArray slot would apply stale frameSize/offsets to a foreign object.

## Clean reference implementations (adopt one of these shapes)

- economy.cpp / order_economy.cpp / inventory.cpp: instance IsLive-revalidated, BP UFunction
  RE-RESOLVED PER CALL via FindFunction(ClassOf(liveInstance)).
- directionalwind.cpp: instance cache nulled in OnDisconnect.
- engine.cpp ResolveSavePrefixFn (2026-07-04): cached BP CDO + IsLiveByIndex revalidation,
  fn re-resolved when the CDO died.

## FLAG-HIGH (cached BP fn/CDO/asset DISPATCHED without revalidation -> crash if class GC'd+reloaded)

- door.cpp:34-37 g_doorOpenFn/g_doorCloseFn/g_moveFinishFn/g_moveUpdateFn (door_C)
- base_window.cpp:30 g_setCleanFn (baseWindow_C)
- garage.cpp:24 g_acivaeFn (garage_C)
- passwordlock.cpp:33-35 g_inputNumFn/g_updFn/g_openFn
- lightswitch.cpp:24,34 g_setActiveFn (trigger_lightRoot_C) / g_useFn (lightswitch_C)
- power_control.cpp:56 g_moveLeversFn (powerControl_C)
- swinger.cpp:24,25 g_openFn/g_closeFn (prop_swinger_C)
- skysphere.cpp:26 g_setMoonPhaseFn (newsky_C)
- grime.cpp:27 g_applyMaterialFn (grime_C)
- door_box.cpp:34,35 + ClassDesc.updateFn/finishedFn:26-27 (locker_C / droneConsole_C;
  OnDisconnect exists but clears ONLY the verify queue)
- email.cpp:40,41 g_addEmailFn/g_delEmailFn (instance IS IsLiveByIndex-guarded; fns are NOT)
- sleep.cpp:26,27 g_wakeupFn/g_sleepFn (mainGamemode_C)
- saved_signals.cpp:25,26 g_saveSignalFn/g_deleteSignalFn
- dish.cpp:26 g_startMovingToFn (dish_C)
- votv_lib.cpp:16,17 g_libCdo (BP CDO as Call TARGET) / g_stepFn (lib_C)
- wisp.cpp:394 g_fatalityMontage (ASSET passed as Set<void*> param), :43 g_releaseFn
  (killerwisp_C), :432 g_dirFn (wisp_C)

## FLAG-HIGH (second sweep: engine/UI modules)

- console_desk.cpp:75,76,100,101,102 g_updCompFn/g_writeToCoordLogFn/g_formDownloadFn/
  g_initDownloadSignalFn/g_playPingSoundFn (mainBP/desk fns), :132 g_updCursorLocationsFn
  (ui_coordinates_C); PLUS three SoundCue ASSETS :103,118,119 g_sndPingSuccess/g_sndWorking/
  g_sndWorkingEnd (fed as UFunction params / SetSound raw-stored into a live AudioComponent)
- engine_mainplayer.cpp:95,96,108 g_ragdollModeFn/g_forceGetUpFn/g_addPlayerDamageFn
  (mainPlayer_C; addPlayerDamage also handed out as a PE-interceptor KEY :508)
- device_screen.cpp:70 g_setActiveInterfaceFn (worst shape: ResolvePass hard-stops on
  g_coreResolved :94 -- the stale fn can never refresh even in principle)
- space_renderer.cpp:66,67,68 g_addSignalFn/g_deleteSignalFn/g_spawnSignalFn
  (spaceRenderer_C; instance IS IsLiveByIndex-guarded :246, fns are not)

## FLAG-HIGH (third sweep: coop layer -- these fire AUTOMATICALLY in gameplay post-re-host)

- weather_redsky.cpp:31,32 g_spawnRedSkyFn/g_redSkyEventSetFn (Call fn + observer anchor;
  OnDisconnect keeps them)
- weather_fog.cpp:29,30 g_spawnFogFn/g_setFogDensityFn (OnDisconnect DELIBERATELY keeps them
  -- ":314 'stable, same UClass'" is exactly the assumption under test)
- weather_lightning.cpp:36 g_lightningStrikeClass (BP class as Set<void*> ActorClass spawn param)
- firefly_sync.cpp:47 g_effFireflies (ParticleSystem asset as EmitterTemplate param)
- event_cue_sync.cpp:61 g_cueTemplates[] (assets; OnDisconnect clears only the PSC set)
- event_fire_sync.cpp:51,52 g_runEventFn/g_runSpecialEventFn (no class-compare fail-safe)

## FLAG-MEDIUM

- spawn_menu.cpp:23 g_spawnMenuCls (stale exact-class match -> menu silently dead after
  re-host; FindFunction/FindPropertyOffset also deref the stale class); :118,201
  sOpenedFn/sClosedFn (static-latched BP fns).
- weather_redsky.cpp:30 g_gamemodeCdo (rarely re-dereffed); kerfur_convert.cpp:86-91
  dropProp*/spawnKerfuro fns (fail-safe class-compare guard); prop_stick_sync.cpp:48
  g_forceStickFn (guarded by IsWallAttachable); remote_prop.cpp:127 g_propThrownFn +
  remote_prop_spawn.cpp:74 g_propSetKeyFn (Aprop_C base likely resident);
  teleport_client.cpp:43 g_teleportWObackroomsFn (dev/host-triggered).
- Coop-layer FLAG-LOW (compare-only/anchors -- silent feature death, no crash):
  item_activate flashlight fns; inventory_pickup_sync g_inventoryCue;
  save_button_disable g_escFn/g_tickFn; multiplayer_menu g_tickFn; garbage_sync
  g_garbageContainerCls; save_block g_saveSlotClass (fails OPEN); npc_sync g_npcAllowlist;
  host_spawn_watcher g_propClasses; firefly class+tick fn; event_fire_sync cached classes.

## NEXT STEP GATE (probe-don't-guess, decided 2026-07-04)

The only PROVEN dangling pointer was an INSTANCE (the save object -- per-load transient,
guaranteed purged). Whether BP UClasses/UFunctions/assets are ACTUALLY GC-purged+relocated
across gameplay->menu->gameplay in VOTV is UNMEASURED -- and client rejoin (same world
round-trip) has been exercised many times with weather/doors/events still working afterward,
which argues classes may persist or reload at identical addresses. BEFORE churning ~30 files:
arm a probe that records InternalIndexOf+pointer of a handful of latched BP fns/classes/assets
(fog spawnFogFn, door openFn, eff_fireflies asset, ui_saveSlots CDO) at first resolve and
IsLiveByIndex-checks + logs them at every StartCoopSession / world change. One re-host session
answers which tiers actually dangle; fix per evidence (economy.cpp per-call re-resolution for
the tiers that do).
- engine_widget.cpp:51 g_npFont (Font ASSET raw-written into UTextBlock.Font; mitigated:
  targets are GameInstance-outered and the font is likely menu-resident).
- device_screen.cpp:43 g_widgets[] / :56 g_devices[] + console_desk.cpp:123 g_uiCoordsCls
  (compare-only BP classes -- silent feature death after re-host, no crash).

## Verified-SAFE families (do NOT churn these)

- Native classes/CDOs/UFunctions (GameplayStatics, KismetSystemLibrary, UMG widgets,
  Actor/Scene/Primitive/SkinnedMesh components, TimelineComponent...) -- rooted for process
  life. engine_attach.cpp:220-223 documents the rule.
- Instance caches with IsLive/IsLiveByIndex + re-resolve: skysphere g_skyCache,
  directionalwind, economy/order_economy/inventory g_gm, email/saved_signals/dish/sleep
  g_gamemode, daynightcycle, console_desk g_instance/g_uiCoordsInst, space_renderer
  g_instance, drone g_cache, engine_pawn CamMgr, engine_playerragdoll g_ragdollClass,
  engine_mainplayer g_hurtMat/g_phcClsCache, save_browser (per-call widget/fn), scs_rig,
  engine_component, engine_bones (stale map keys never deref'd).
- BP UClass* latched but used ONLY to resolve offsets at latch time (console_desk g_cls/
  g_atlasCls, drone g_cls, space_renderer g_cls/g_uiSignalCls) -- inert after latch.

## FLAG-LOW (BP UClass* used ONLY in pointer-equality/descendant compares -- stale value
silently misclassifies = feature stops syncing after re-host, no crash)

door.cpp:30; base_window.cpp:27; garage.cpp:21; passwordlock.cpp:27; lightswitch.cpp:21,31;
power_control.cpp:54; swinger.cpp:22; windturbine.cpp:17; grime.cpp:24; atv.cpp:24;
door_box.cpp:32,33; wisp.cpp:32,429; resolve-time-only: email.cpp:32,38,39; sleep.cpp:19,29,36;
saved_signals.cpp:21; dish.cpp:18,23; daynightcycle.cpp:22,137,139.

## No module resets its latch on world change

`grep g_resolved.store(false` -> zero hits. Only door_box::OnDisconnect (verify queue only)
and directionalwind::OnDisconnect (instance -- the correct pattern) exist.

## Recommended systemic fix (to design in the follow-up thread)

Either (a) ONE world-boundary reset hook that clears every BP-pointer latch (extend the
OnDisconnect/world-change seam; one owner per [[feedback-one-owner-order-axis]]), or
(b) migrate FLAG-HIGH sites to the economy.cpp per-call re-resolution model. (b) is
allocation-light (FindFunction on a live class is a walk of that class's function list) and
removes the latch concept entirely; (a) is a smaller diff but keeps the latch pattern alive.
Also revisit call.cpp's fn-pointer-keyed metadata cache under whichever model wins.
