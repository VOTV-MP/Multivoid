# hook — the grappling hook players climb the map with   (STATUS: RE)

The native traversal tool: fire the hook, it attaches to geometry, scroll the wheel to
winch yourself up the cable, release, fire again higher — repeat to reach anywhere.
Also doubles as a tie-two-things-together tool (second end can be anchored to world/props).

RE pass 2026-07-06, fully static (kismet-analyzer bytecode; no live probe yet). Sources:
`research/bp_reflection/hook.json` + `prop_hook.json` (dumped this pass from
`research/pak_re/extracted/VotV/Content/objects/prop_hook.uasset`), disasm scratch at
`research/bp_reflection/_hook_uber_full.txt` + `_hook_fn_*.txt`. Every claim below is
[bytecode] unless tagged otherwise; uber offsets cited as `@N`.

## 1. Native behavior (ground truth)

### Classes

- **`prop_hook_C` : `prop_C`** — the inventory ITEM ("Hook"). Stays in hand across uses;
  it is a factory. Props: `single` (bool variant flag), `hookActor` (class = `hook_C`).
  Handlers: `playerHandUse_LMB` (uber @451), `playerHandUse_RMB` (@2887).
- **`hook_C` : `actor_save_C`** — the DEPLOYED hook actor (head + cable + constraint).
  NOT a prop (prop_C is cast separately on hit actors). Saves natively via
  getData/loadData (objectsData family — stable-ID relevant).
  Components: `A` (hook-head end), `B` (rope end), `Cable` (UE CableComponent, visual),
  `PhysicsConstraint` (A↔B when anchored), `throwConstraint` (player↔phys during
  flight), `phys` (runtime sphere, flight ballistics), `empty` (player-root anchor
  helper), `hook_single`/`hook_single1` (static-side meshes used when an end is
  Landscape), `Sphere`/`Sphere1` (collision), `hovertext` (nametag comp showing dist),
  `Audio`, `h`.
  Key vars: `actor_a/b`, `component_A/B`, `attached_a/b`, `attachLoc_A/B` (component-
  local), `attachKey_A/B` + `attachComponentName_A/B` (save identity via `getKey`),
  `isProp_A/B`, `dist` (cable length), `maxDist`, `scroll` (reel step), `throwSpeed`,
  `playerVelDiv`, `isThrown`, `playerHooked`, `dontDrop`, `lastLoc`, `addVel`,
  `hookObject` (back-ref to the item), delegates `hooked_A/B`.
- Variant assets next door (not yet RE'd): `hook_Child`, `hook_flesh`, `prop_hook_erie`.

### Lifecycle state machine

1. **FIRE (LMB, no active hook)** — `prop_hook` uber @881:
   - Crosshair on blocking hit @1089: deferred-spawn `hookActor` AT the hit point,
     `hookObject=self`, `player.activeHook = it`, direct `attach_a(hitResult, player,
     checkLen=true)` — point-blank plant, no flight.
   - No blocking hit @1890: deferred-spawn at CAMERA location, `addVel = player.
     fallVeloc`, `activeHook = it`, **`throw(player)`**.
2. **FLIGHT** — `hook_C::throw` → uber @7011: `isThrown=true`; runtime `phys` sphere
   added; `empty` attaches to player root; `throwConstraint` constrains player-root ↔
   `phys`; `B` attaches to player root, `A` attaches to `phys`;
   `phys.velocity = cameraForward × throwSpeed + addVel`; `lastLoc` seeded.
   - Flight tick @3782: sphere-trace `lastLoc → phys` (radius 3, statDynPhysVeh object
     set); on hit: `phys` teleported to hit, **`attach_a(hit, GetPlayerCharacter(0),
     true)`**, `isThrown=false`, `phys` destroyed, **`playerHooked=true`**.
   - Tick @3495 while thrown: `throwConstraint` linear drive on when |A−B| ≥ `maxDist`
     (the cable "runs out" — hook yanks back).
3. **attach_a(hit | actorReplace…, ActorAttach, checkLen, …)** — the hook-head anchor:
   - REJECTS (destroys the whole hook actor): hit actor implements `int_objects_C.
     ignoreHook`=true @315; hit actor is a `Character` @390 (**can't hook people**);
     `IsChildActor` @484; hit actor's class is another `hook_C` @545.
   - Else: `actor_a=ac`, `attached_a=true`, head placed at `loc+norm` rotated to
     `-norm`; constraint side = hit component (Landscape → `hook_single` static mesh
     side) @2484; `safeAsProp → isProp_A`; **if prop and frozen: `setPropProps(false×4)`
     — hooking UNFREEZES a frozen prop** @1430; hovertext bound to `Sphere`.
   - `attach_a`/`attach_b` take `actorReplace/componentReplace/locationReplace/
     normalReplace` params that BYPASS the hit result — a programmatic attach API
     (the save-restore path uses it; perfect seam for coop mirrors).
4. **HOOKED-TO-PLAYER (the climb phase)** — `playerHooked=true`, `B` on player root:
   - Tick @4491: skip if player grabs the hook's root actor, or `isThrown`, or
     MovementMode==None. Else **the PULL**: `vel += SetVectorLength(A−B,
     max((|A−B|−dist)/playerVelDiv, 0))` with ×0.99 damping when taut, written
     STRAIGHT into `mainPlayer.CharacterMovement.Velocity` @5797; if mode==Walking and
     pull ≥ 100 → `SetMovementMode(Falling)` @6517. **All via `lib_C::getMainPlayer` —
     hard SP assumption, always the LOCAL player.**
   - **REEL**: `mainPlayer` uber scroll handlers (`_mainplayer_uber_full.txt` @44763 down
     / @45744 up): `activeHook.dist ∓= activeHook.scroll`, clamp [20, maxDist],
     `activeHook.setLength()`. setLength: constraint XYZ limits = dist,
     `Cable.CableLength = dist/1.5`.
5. **ANCHOR SECOND END (LMB with active hook, non-`single`)** — prop_hook @2457: blocking
   hit → **`attach_b(hitResult, resetLen=true)`**, then `activeHook := null` (+item
   updateHold @10); no hit → hook DESTROYED. `single=true` variant: LMB just destroys
   (re-fire style). RMB @2887: cancel — destroy active hook.
   - `attach_b`: same reject set; `PhysicsConstraint.BreakConstraint()` first @1480;
     `attached_b=true`, B placed at hit; `dist = min(|A−B|, maxDist)` when resetLen/0
     @2023; constraint limits set; **`assign()`**; `Audio.Activate`; notifies
     `int_objects.hooked(self)` + `BROADCAST hooked_B`; hovertext dest + `empty`
     detach @2915.
   - **`assign()`** (uber @489): `Cable.CableLength = dist/1.5`; `attachKey_A/B :=
     getKey()` of both actors (save identity); `setLocs` (attachLoc_A/B in component
     space); `setLength`; `makeAttachments` (PhysicsConstraint component_A ↔
     component_B, Landscape sides swapped for hook_single meshes); **`playerHooked=
     false`**; binds `OnDestroyed` of both actors → `unhook_(false)` @200, prop
     `unhooked` delegates, and re-checks `ignoreHook` @984.
6. **ANCHORED (A+B both world/prop)** — passive furniture: tick @2189 keeps constraint
   linear/angular drives on only when taut (|A−B| ≥ dist); `tensionLinear = max(|A−B|−
   dist, 0)` @3233; 1 Hz `tension` timer notifies `int_objects.hookTension` on both
   ends @8044; hovertext shows live dist @2802. Collision re-enabled when both
   attached @79.
7. **RELEASE / DESTROY** — `unhook_(spawnHook)`: if `attached_a && attached_b &&
   spawnHook` → `drophook(self)` spawns a **`prop_hook_C` pickup** at the hook (unless
   `dontDrop`) @6879, then `K2_DestroyActor`. `kicked` event (@1838): `drophook` →
   try `putObjectInventory2(player)`, destroy pickup if it fit. Either attached actor
   destroyed → `unhook_(false)`.
8. **SAVE / LOAD** — `getData`: A/B world rotations + locations, `dist`,
   `attachKey_A/B`, `attachComponentName_A/B` via lib setSaveDataBySlots. `loadData` +
   `processKeys`: re-resolve actors from keys via `gamemode.getObjectFromKey`,
   `getComponentObjectByName`, retry via `frameDelay` loop until valid, re-attach.

## 2. Sync-axis table

| axis | owner (who simulates) | peers need | carried by |
|---|---|---|---|
| item pickup/inventory/hand (`prop_hook_C`) | host (prop lane) | prop mirror as usual | prop lane (existing; hand-visibility = the known hold-state gap) |
| hook actor EXISTS (spawn/destroy of `hook_C`) | firing player (today: LOCAL-ONLY — in NO lane: not prop_C, not NPC, not WA-allowlisted) | see it appear/disappear | **GAP — nothing today** |
| flight ballistics (phys sphere, ~0.5-2 s) | firing player | A-end position during flight (cosmetic; short) | GAP |
| A-end anchor (loc/rot, what it hit) | firing player at attach moment | exact planted head pose | GAP |
| prop-unfreeze side effect (attach_a → setPropProps) | MUST be host (prop authority) | prop wakes on all peers | GAP — client-local today = divergence |
| climb pull on the player (tick @4491 velocity writes) | inherently OWNER-LOCAL (getMainPlayer) | nothing — result rides the existing player pose stream | player pose lane (existing, free) |
| reel `dist` (scroll) | owning player | cable length on the mirror | GAP (1 float, event-ish) |
| cable visual (CableComponent A↔B) | per-viewer sim (UE cable is cosmetic client physics) | endpoints + CableLength; sim runs locally | derived — free once endpoints exist |
| anchored A↔B constraint pulling PROPS | MUST be host (prop authority) | prop motion via prop lane | GAP — client-anchored hook can't drag host props |
| tension notifies (`hooked`/`hookTension` on int_objects) | side of the authoritative constraint | consequences ride the target's own lane | follows constraint owner |
| persistence (save) | host save (`actor_save_C` native getData) | joiner gets anchored hooks from save | native for HOST-owned hooks only |
| hovertext dist readout | per-viewer | optional (native shows to whoever looks) | local; fine either way |

## 3. Coop design (DESIGN — ratified with user 2026-07-06, not built)

Architecture decisions (the smart-item pattern, hook = case #1):

- **Home**: `coop/items/` — the folder already exists (inventory/order sync); a deployed
  item actor is the world-side continuation of the ITEM concept, same domain. New files
  `hook_sync.cpp` (+ `hook_mirror.cpp` at >800), npc_sync/npc_mirror shape. No separate
  "deployables/" folder (one concept = one folder).
- **Authority is phase-split, not uniformly host** (MTA precedent: client-simulated
  `CClientProjectile` + server-owned pickups —
  `reference/mtasa-blue/Client/mods/deathmatch/logic/CClientProjectile.h`): authority
  follows whoever the game physically welds the simulation to.
- **Transitions are POLLED, not hooked**: throw/attach_a/attach_b are BP→BP direct calls
  (prop_hook uber → `activeHook.attach_a`) — per COOP_DISPATCH_VISIBILITY presumed
  PE-INVISIBLE. The whole state machine is plain properties (`isThrown`, `attached_a/b`,
  `playerHooked`, `dist`, A pose) → owner polls at snapshot cadence, diffs become
  reliable transition events (alarm-lane precedent). No Func-thunk patches needed.
- **No framework at N=1** ([[feedback-fix-then-generalize-mirror-identity]]): hook ships
  working first, rope second on the same pattern; shared smart-item primitives
  (phase enum, owner slot, anchor-handoff handshake) get extracted at case #3.

Ownership per lifecycle phase:

1. **Fired/flying/player-hooked** (phases 1-4): the hook is an EXTENSION OF THE OWNING
   PLAYER, exactly like their pose. Owner simulates natively (their own machine already
   does everything — pull, reel, physics). Peers get a **display-only mirror**: spawn
   `hook_C` deferred with **actor tick DISABLED before FinishSpawning** + collision off,
   drive A-end from the owner's stream, B-end follows the owner's puppet root, set
   `Cable.CableLength = dist/1.5` on reel changes. The cable component sims per-viewer
   for free once endpoints are right.
   - Wire shape: a small per-player hook state in the existing player snapshot cadence
     (active flag + phase + A loc/rot + dist), NOT a new heavy lane. Flight can be
     smoothed client-side; it's sub-2-seconds cosmetic.
2. **Anchored** (phase 6, player released): the hook graduates to WORLD FURNITURE →
   **authority transfers to host**. Owner sends an anchor-commit (actors/components by
   eid + attach locs); host spawns/adopts the authoritative `hook_C` using the native
   programmatic-attach seam (`attach_a`/`attach_b` `actorReplace/componentReplace/
   locationReplace/normalReplace` params — no synthetic HitResult needed), then it
   persists in the host save natively (`getData` + attachKeys) and late-joiners get it
   from the save like any actor_save_C. Client's local instance is destroyed on commit
   (identity handoff, no dupe — the join-window/dup discipline applies).
3. **Prop interactions must be host-side**: the unfreeze side effect and any constraint
   end on a prop belong to the host instance. For a CLIENT climbing a hook planted in a
   host prop: pull-on-player stays owner-local (harmless), but the reaction force on the
   prop only exists where the authoritative constraint lives. V1 scope decision to make:
   host re-plants a client's prop-anchored hook immediately (not just at release), OR
   prop-anchored client hooks are transferred at attach_a time. Defer until the basic
   geometry-anchored flow works.

**CRITICAL mirror rule**: a mirrored `hook_C` with a LIVE tick is catastrophic — its tick
calls `getMainPlayer` and would (a) yank the VIEWER's own player toward someone else's
hook (@5797 velocity write) and (b) flip them to Falling (@6517). Tick-off is not an
optimization, it is correctness. (Same class of trap as npc_mirror tick-off.)

**Native guards that help us**: attach_a destroys the hook on Character hits — a thrown
hook meeting a puppet self-destructs natively (no player-hooking exploit to close);
`ignoreHook` interface is respected everywhere; `hook_C`-on-`hook_C` rejected.

## 4. Caveats / known quirks

- `attach_a` on a frozen prop UNFREEZES it (`setPropProps(false,false,false,false)`) —
  a world mutation hidden inside a "cosmetic" attach; must not run client-local in coop.
- The flight-hit path attaches to `GetPlayerCharacter(0)` as ActorAttach — index-0
  assumption, same SP family as getMainPlayer.
- `drophook` spawns a fresh `prop_hook_C` pickup — an item DUPLICATION seam if both host
  and client run release logic (classic double-spawn; the anchored-authority handoff in
  §3 exists partly to keep release single-owner).
- Landscape anchoring swaps the constraint side to internal static meshes
  (`hook_single`/`hook_single1`) — mirrors must reproduce via the same native functions,
  not hand-built constraints (template-faithful rule).
- `loadData/processKeys` retries via `frameDelay` until components resolve — load-order
  tolerant natively; our join snapshot can lean on the same retry.
- Scroll reel writes `dist` from the PLAYER BP (mainplayer uber), not through a hook
  UFunction — `setLength` is the visible seam (PE-dispatched), `dist` mutation itself is
  a raw property write (EX_Let, PE-invisible). A reel observer hooks `setLength`.
- `rope_C` is the dumb sibling (constraint + break handler only) — same two-end shape,
  candidate to ride the same sync once hook ships.

## 5. Verification

- 2026-07-06: full static bytecode RE (this doc) — prop_hook uber 57 stmts, hook uber
  292 stmts + all 14 named fns read. NO live probe yet: `single`-variant behavior,
  throwSpeed/scroll magnitudes (CDO defaults not read), and the exact feel of the pull
  are unverified in-game.
- Sync: NOT BUILT. Next per THE METHOD: (a) read CDO defaults for the tunables,
  (b) wire-design review vs COOP_SYNC_MAP (which snapshot carries the per-player hook
  state), (c) build phase-1 mirror (fire/fly/climb display), (d) anchored handoff,
  (e) e2e + hands-on.
