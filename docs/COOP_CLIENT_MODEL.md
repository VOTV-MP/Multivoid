# COOP client model — custom per-client skeletal mesh (full handoff doc)

**Read top-to-bottom.** Goal: give every remote CLIENT puppet a custom character
model (the user's own mesh) while the HOST stays Dr. Kel. First model = a Half-Life 1
GoldSrc scientist. The pipeline generalizes to any HL1 humanoid `.mdl`.

**STATUS (2026-07-02 evening take 2: FEATURE COMPLETE [V x4 incl. coop visual "Работает
amazing"]; the v2 "wide" profile look was REJECTED in-game the same evening ("переделай
обратно под v1") -- the scientist is re-cooked on v1 "narrow" (pak `AE49002C`, deployed
8/8).** LATE-NIGHT ADDENDUM (the 2nd-model postmortem, commit `41c3f41b`): the rvi
model converted with the einstein profile shipped BROKEN with zero warnings -- the
converter now MEASURES fit (profile auto-select scoring + coverage report + the
ancestor bone resolver + learn-from-manual-PSK; §4/§5/§6e); rvi re-cooked from the
user's own pose = pak `ED666BE5` 4/4, verdict pending. There is no DEFAULT_PROFILE
anymore -- auto-select or a learned per-model profile decides (v1 stays the einstein
pick by rest-fit). The model + pak + packages are `hl_einstein_v1sc`
(the "scientist" naming is retired, rename `6f4d41d1` stands); the repose profile comes
from the `tools/client_model/profiles/` LIBRARY -- v1 narrow = DEFAULT (in-game look
preferred), v2 wide (format-2 R+t local deltas, learned from the user's
`hl_einstein_v1sc_new_profile.psk` at residual 0.00005) kept in the library. The two
pipeline root-fixes from the v2 work STAND regardless of the verdict: learn's up-axis is
a CONSTANT of the target space (argmax-bbox broke when the wide arm span exceeded the
height), and format-2 profiles carry JOINT TRANSLATIONS that rotation-only silently
dropped. Probe client_model_probe retained per the probes-exempt rule, flag off.)**
- **RUNTIME = DONE + PROVEN WORKING (§3), commit `320c0ab4`.** [historical record -- the
  role-gate shape described here was PROVEN, then SUPERSEDED 2026-07-02 late evening by the
  v93 per-player SKINS system (see the RUNTIME block below + §3): the gate + the one-mesh
  API (`GetClientPuppetMesh`/`ApplyClientPuppetTexture`) were replaced by name-keyed
  `GetSkinMesh`/`ApplySkinToBody` + `SkinForSlot`, RULE 2.] Autonomous 2-peer LAN test
  (host s_1234 + fresh client): pak auto-mounts, `URyRuntimeObjectHelpers::LoadObject` returns
  OUR package's SkeletalMesh, `RemotePlayer::Spawn` applies it to the client puppet (then
  role-gated: slot 0 host = kel, slots >= 1 clients = custom), the puppet animates +
  pose-tracks (trail 0cm). Files: `ue_wrap/asset_load.{h,cpp}`,
  `coop/player/client_model.{h,cpp}`, `RemotePlayer::Spawn`, `deploy-all.ps1` pak ship.
- **GEOMETRY COOK = VERIFIED IN-GAME [V hands-on 2026-07-02]:** the scientist SHAPE renders and
  animates on the anthro rig ("Работает" -- probe take 3, kel control vs scientist subject side by
  side). The earlier "renders as kel" was a probe-design CONFOUND, not a cook bug: mainPlayer_C
  draws TWO overlapping bodies -- `mesh_playerVisible` AND its AttachParent `ACharacter::Mesh`
  (class-default kel) -- and the native slot's kel MASKED the custom mesh (take 1). §6d is CLOSED
  (byte-parse: our cooked render region = 1 section/1062 verts/52KB, kel's 17132 verts physically
  absent; sha256 chain cook output == staging == deployed pak).
  - **Engine lesson (take 2, MEASURED):** hiding the AttachParent (`SetVisibility` +
    `SetHiddenInGame`, both propagate=false) makes the CHILD vanish too -- UE4 implicitly gates a
    child's rendering by its AttachParent's visibility regardless of the propagate flag. Never
    hide the native body slot.
  - **Two-body invariant (the feature mechanism, take 3):** the given skin goes into BOTH slots
    (`SpawnPuppetMainPlayer` now does this for every puppet; stock kel = engine early-out no-op).
    Probe: `coop/dev/client_model_probe` ([dev] client_model_probe=1) -- kept as the visual
    harness for the texture iterations.
- **POSE (A-pose→T-pose) = SOLVED + AUTOMATED (§5).**
- **TEXTURES = DONE + VERIFIED IN-GAME [V hands-on 2026-07-02 "теперь нормально"] -- full ladder:**
  Rung 1 [V static RE]: slot-0 material `inst_kel4_body` = MIC of `mat_object_sk` with ONE
  texture parameter **`tex`** (value tex_kel3_skin) -> MID binding viable, no cooked material.
  Rung 2 [V]: `tools/client_model/ue_tex.py` cooks a PNG into a cooked `UTexture2D` package
  (PF_B8G8R8A8, 1 mip, inline; full package rename incl. the REAL name-map hash recipe, §8).
  Rung 3 [V hands-on "текстура растянута" = exactly the expected single-tile stretch]: runtime
  `CreateDynamicMaterialInstance(slot0)` + `SetTextureParameterValue('tex')` chain PROVEN.
  Rung 4 [V hands-on]: `tools/client_model/atlas.py` shelf-packs the 19 PNGs into one 512x256
  atlas (1px duplicated-edge gutter; no mips so 1px suffices); ue_cook remaps every corner UV
  into its `usemtl` tile INCLUDING the V-orientation fix (cooked v = 1 - obj_v: GoldSrc t=0 =
  texture top, OBJ stores v-up, UE samples v-down with mip row 0 = PNG row 0 -- rung 3 was
  per-tile v-flipped, invisible in the garble). Byte-verified: 1062/1062 UVs inside tile rects.
- **WINDING root-fixed [V hands-on 2026-07-02, exposed by rung 4]:** the real textures revealed
  an INSIDE-OUT render (front faces culled -- "смотрю спереди, вижу спину... как будто изнутри")
  present since the FIRST cook: the old "geometric-outward = front" heuristic was an assumption,
  and it is BACKWARDS vs the engine -- cooked UE meshes store **CW-outward** winding (template
  kerfurOmega_KelSkin signed volume = **-161625**; our cook emitted +102223). The geometry pass
  could not catch it: a closed mesh's SILHOUETTE is winding-invariant and the garble hid the
  facing. Fix: ue_cook now MEASURES the template's signed-volume side (divergence theorem --
  exact, immune to the concavity noise that made a centroid-ray test read 0.469-vs-0.716) and
  matches it; shading normals are decoupled from index winding.
- **SHADING NORMALS = the MDL's AUTHORED ones [root-fixed 2026-07-03 `d3f43626`, offline
  render-proven]:** the original "recompute geometric-outward from windings" produced the
  DARK-SUIT bug (user: "костюм слишком тёмный под углами, у всех v1sc") -- the GoldSrc
  scientist coat is a DOUBLE-SIDED sheet (outer+inner copies), so per-position accumulation
  self-cancelled into near-zero normals (probe: scientist_body 27% of instances >90deg off
  authored; heads/hands ~10deg = why only suits looked broken). Now mdl_extract reads the
  per-trivert normindex (previously discarded) + the norm bone table, writes `vn` +
  `f p/t/n`; repose rotates normals with each bone's rigid part; ue_cook Y-mirrors and packs
  them PER FACE-CORNER (vertex split key includes the normal index). The recompute is
  deleted (RULE 2). In-game the backface cull (template-matched winding) hides the coat's
  inner copy -- verified in the offline proof render with the same cull.
- **FEATURE texture bind wired (not just the probe):** [the API names here are the pre-v93
  ones, superseded same day -- now `GetSkinTexture(name)` inside `ApplySkinToBody`] slot-0
  MID + 'tex' on BOTH body slots; `RemotePlayer::Spawn` binds it right after SpawnPuppet on
  custom-mesh puppets. COOP VISUAL VERIFIED [V hands-on 2026-07-02 "Работает amazing"]:
  host+client facing each other, both looks correct (pre-skins role-gate build).
- Deployed state (2026-07-03 evening): DLL `1CDD6079A5241162` 4/4; ALL 15 shipped model paks
  rebuilt with authored normals on their same library profiles (einstein v1_narrow, rest
  rvi38) and redeployed (user source folder + models/ + installs, hash-verified per pak).
  Audit (2026-07-02): all functions COLD-path; remote_player.cpp past the 800 soft cap ->
  queued extraction: ragdoll subsystem -> remote_player_ragdoll.{h,cpp}.

**Locked decisions (RULE 1 no-crutch, RULE 3 no-editor-at-runtime):**
- A REAL cooked `USkeletalMesh` the engine skins natively (NOT a ProceduralMesh +
  CPU-skin — rejected, §9).
- Target rig = the **ANTHRO `kerfurOmegaV1_Skeleton`** (~101 bones) — the rig the real
  client skin `kerfurOmega_KelSkin` uses. (`kel_lmao` 6-bone is a useless stick — ignore.)
- Cooked in pure Python → packed with repak → loaded from a `.pak` at runtime.
- Source = GoldSrc/HL1 `.mdl`.

---

## 1. Architecture (the whole flow)

```
OFFLINE (dev machine, tools/client_model/):   [DONE, automated]
  hl_einstein_v1sc.mdl
   → mdl_extract.py   → model.obj (v/vt/vn + f p/t/n -- AUTHORED normals) +
                        model.bones.json(+bone WORLD mats, vert+norm bone tables) + tex/*.png
   → repose.py apply  → hl_einstein_v1sc_tpose.obj  (auto A-pose→VOTV T-pose + scale, §5;
                                                     profiles/ LIBRARY; normals rotated per-bone)
   → atlas.py         → atlas.png + atlas.json      (19 tiles → 512x256, 1px gutter)
   → ue_cook.py       → hl_einstein_v1sc.uasset/.uexp (Y-mirror to cooked space, HL→anthro
                                                     rigid bone remap, per-face atlas UV remap,
                                                     TEMPLATE-MATCHED winding, AUTHORED normals
                                                     per face-corner, splice into template)
   → ue_tex.py        → tex_hl_einstein_v1sc.uasset/.uexp (atlas.png → cooked UTexture2D, §8 rename)
   → repak pack       → hl_einstein_v1sc.pak        (VotV/Content/Mods/VOTVCoop/*, V11, 4 files)
SHIP: hl_einstein_v1sc.pak deployed to EVERY peer by tools/deploy-all.ps1
      (Content/Paks/LogicMods/votv-coop/; the pre-rename scientist.pak is auto-removed).
RUNTIME (mod):   [v93 SKINS, AS-BUILT 2026-07-02 late evening -- REPLACED the role gate (RULE 2)]
  UE auto-mounts every pak in LogicMods/votv-coop → each PLAYER carries a skin NAME
  (votv-coop.ini player_skin=, next to player_guid; a NEW identity rolls a RANDOM starter
  from the curated 6-list ∩ present paks -- walter/sci/rvi_scientist/luther/twhl2/twhl3,
  fallback hl_einstein_v1sc; `4570180e` -- persisted once like the guid; picked in
  F1 > Cosmetics > Skins) → the name rides the Join payload (after the guid) + PlayerJoined
  (after the nick) + live SkinChange (kind 82, host-relayed) →
  RemotePlayer::Spawn(skinName)/ApplySkin + local_body::Tick (the LOCAL first-person body)
  all route through client_model::ApplySkinToBody: SetSkeletalMesh on BOTH body slots
  (two-body invariant) + slot-0 MID 'tex' on both; "dr_kel" = the pristine native mesh
  (local_body's pre-swap capture) + SetMaterial(0,null) override clear.
  skin_registry scans the pak folder for the F1 browser (name = pak stem; preview =
  sibling <name>.png/.bmp -- WIC-decoded into ImGui tiles).
```

Two halves: the offline cook (§4/§5, geometry+texture+winding VERIFIED in-game) and the
runtime (pre-skins coop visual VERIFIED 2026-07-02 morning; the v93 skins generalization +
the v1-profile re-cook are AS-BUILT, hands-on pending -- runbook
`research/handson_runbook_2026-07-02_evening_fixes.md`).

The repose (§5) is learned ONCE from a manual example and is now a reusable profile;
adding a NEW model is `mdl_extract → repose.py apply → ue_cook → repak` (no Blender) --
or ONE run of the portable converter (§4). Drop the pak (+ preview png/bmp) into
LogicMods/votv-coop on EVERY peer and it appears in everyone's F1 browser.

---

## 2. Verified game facts (rig + seams + the coordinate transform)

- The real client puppet mesh (`mainPlayer_C.mesh_playerVisible`, offset `0x04F8`) is
  the anthro skin `kerfurOmega_KelSkin` on **`kerfurOmegaV1_Skeleton`** (~101 bones:
  pelvis, belly, chest, neck, head, upperarm/forearm/hand L+R + fingers, thigh/lowerLeg/
  foot L+R + IK/FK). VOTV ships many skins on this rig — anthro-skin-swap is native.
- Puppet spawn: `ue_wrap::puppet::SpawnPuppetMainPlayer()`
  (`src/votv-coop/src/ue_wrap/puppet.cpp:341-680`); mesh+anim applied at `:525`.
- Appearance seam: `RemotePlayer::Spawn()`
  (`src/votv-coop/src/coop/player/remote_player.cpp`): `:87` reads local skin
  (`Pup::GetMeshPlayerVisibleAsset`), `:92` AnimBP class
  (`GetMeshPlayerVisibleAnimClass`), `:158` `Pup::SpawnPuppet(spawnLoc, skin, animClass)`.
- Apply primitives (exist): `SetSkeletalMesh`
  (`src/votv-coop/src/ue_wrap/engine_component.cpp:212-222`), `SetAnimClass` (`:241-250`).
- Game pak: UE4.27, pak v11, unencrypted.
- **COORDINATE TRANSFORM — PSK space → cooked-UE space is EXACTLY a Y-negation**
  `(x, y, z) → (x, −y, z)`, measured from the dr_kel pair (dr_kel.psk points → cooked
  kerfurOmega positions, affine residual **0.0**, det −1). The cook applies this + a
  winding reversal (Y-mirror flips handedness). This is why repose outputs in PSK space
  (matching the user's Blender/psk-psa round-trip) and the cook just mirrors Y.

---

## 3. RUNTIME: load the pak + apply + TEST  [pak/load/apply mechanics PROVEN 320c0ab4 + 8df26e05; the delivery POLICY is now the v93 SKINS system, AS-BUILT 2026-07-02 late evening]

> **v93 SKINS (the current policy layer, replaced the role gate -- RULE 2):** every player
> carries a persisted skin NAME (`votv-coop.ini player_skin=`, next to `player_guid=`;
> default `hl_einstein_v1sc`; F1 > Cosmetics > Skins to change, gmod-style tiles). Name =
> pak stem in `LogicMods/votv-coop/` ("dr_kel" = native). Announce: Join field (after
> guid) + PlayerJoined field (after nick) + live `SkinChange` (kind 82, host-relayed,
> forgery-guarded). Apply: `client_model::ApplySkinToBody` (both body slots + slot-0 MID
> 'tex'; dr_kel = pristine native mesh from `local_body::NativeBodyMesh()` + material-
> override clear) -- used by `RemotePlayer::Spawn(skinName)/ApplySkin` (puppets) AND
> `local_body::Tick` (the LOCAL first-person body -- the immersion fix). Catalog + F1
> browser: `skin_registry` + `ui/skins_panel` (preview = sibling `<name>.png/.bmp`,
> WIC-decoded). Peer without the pak: kel fallback, logged. AS-BUILT; hands-on pending
> (runbook 2026-07-02 evening take 3).
>
> **SKIN EFFECTS (2026-07-03, take-3):** builtin kerfur skins additionally rebuild the
> variant ACTOR's cosmetic rig on the dressed body — `coop/player/skin_effects` +
> `ue_wrap/scs_rig` (runtime SCS read). Template flags are honored BIT-EXACTLY via
> `reflection::FindBoolProperty` (take-3; the take-2 byte-XOR guess instanced the dormant
> violet lifeLight on every skin): dormant nodes (sentient-only light/sparks) stay OFF,
> mynet's absolute-rotation grid decals + never-ticking emitters + att_small zapp loops
> reproduce the native look. RT face (4 omega bodies, kerfusFace_C scene-capture), step
> modes per bytecode — REPLACE (mynet: default step muted at volume 0, boltrix@1 at actor
> loc) / ADDITIVE (keljoy squeak, scaled/4 vol + scaled/2+1 pitch); own-body REPLACE sound
> skipped (native local step unmutable — would double). Receiver-local, nothing on the
> wire. RE: research/findings/votv-kerfur-variant-effects-RE-2026-07-03.md; smoke PASS
> (omega rig 0 comps + face, mynet 31); hands-on pending (runbook 2026-07-03 take 3,
> DLL `6646128B7172620C`).
>
> The pre-skins AS-BUILT below stays as the PROVEN mechanics record: the skin is applied to
> BOTH body slots (SpawnPuppetMainPlayer writes it to mesh_playerVisible AND the native
> ACharacter::Mesh AttachParent) -- see the STATUS block's two-body invariant. The 3C coop test
> PASSED [V hands-on 2026-07-02 "Работает amazing"]: host+client facing each other, client
> puppet = textured scientist on the host's screen, host puppet = kel on the client's.

Prior RE: `research/findings/votv-mp-pak-mount-feasibility-2026-05-25.md`.

### 3A. Load the cooked mesh (reflection only, no UE4SS)
VOTV ships reflection-callable pak plugins (call via the mod's `R::FindClass` /
`R::FindFunction` / `ParamFrame` on the game thread):
- **`URyRuntimeObjectHelpers::LoadObject(FString fullObjectPath) → UObject*`** —
  synchronous load by `/Game/...` path. **Use:**
  `LoadObject("/Game/Mods/VOTVCoop/hl_einstein_v1sc.kerfurOmega_KelSkin")` → the
  `USkeletalMesh*`. (The OBJECT name is `kerfurOmega_KelSkin`, not the package name —
  §6a; rename is §8.)
- `URyRuntimePakHelpers::MountPakFile(FString path) → bool` — mount from an explicit path.
- `UPakLoaderLibrary::{MountPakFile, GetPakFileObject, GetPakFileClass}` — 2nd plugin (fallback).
- **UE4 auto-mounts any `.pak` under `Content/Paks/` at startup.** Simplest deploy:
  drop `hl_einstein_v1sc.pak` at `…/VotV/Content/Paks/LogicMods/votv-coop/`; it's
  mounted before the mod boots → the mesh is resident → `LoadObject` returns it. Mount
  root `/Game/Mods/VOTVCoop/`.
- Call context: resolve the plugin CDO/target + `FindFunction` + `ParamFrame` (mirror
  the feasibility doc's ~40-LOC `LoadCoopAssetClass` POC). Game thread, after world-ready.
  Graceful-degrade if the pak/asset is missing (keep the default mesh).
- **Deploy:** `hl_einstein_v1sc.pak` ships to EVERY peer (client-side visual) via
  `tools/deploy-all.ps1` (which also removes the pre-rename `scientist.pak` once).

### 3B. Apply to client puppets (the seam)
At the §2 seam (`RemotePlayer::Spawn`), pick the skin by the **remote peer's ROLE**:
- remote peer is the **host** → keep the current mesh/anim (today's behaviour).
- remote peer is a **client** → the loaded scientist `USkeletalMesh` + the anthro AnimBP
  (copy it from the local player's animClass; the scientist is on `kerfurOmegaV1_Skeleton`,
  so the anthro AnimBP drives it 1:1).
- Primitives already exist (`SetSkeletalMesh`, `SetAnimClass`, §2). **No per-frame code,
  no animation changes** — the engine skins natively (shared skeleton + AnimBP). The mod
  already knows peer role (session / `net.role`).

### 3C. IN-GAME TEST RECIPE (the gate) — how to test "напялить на клиента"
Preconditions: build+deploy the 3A/3B runtime change; deploy `hl_einstein_v1sc.pak` to BOTH peers.
1. Launch a LAN session (host + client) per the named-window launchers
   (`mp_host_game.bat` + `mp_client_connect.bat`). Fresh/New-Game save only.
2. Client joins. **On the HOST screen, look at the CLIENT's puppet** (and vice-versa the
   client looks at other clients).
3. **PASS criteria:**
   - The puppet is the **scientist shape** (humanoid, distinct from Dr. Kel), not kel.
   - **No crash / no hang** on load or spawn (confirms §6 — the spliced package loads).
   - **Animation is correct:** idle/walk drive the scientist with the anthro AnimBP;
     **arms do NOT over-rotate** (this is what the §5 repose fixed — verify it held).
   - The HOST's own puppet (seen by the client) stays Dr. Kel (role gating works).
4. ~~EXPECTED-WRONG: garbled texture~~ **RESOLVED** — the §7 atlas ladder shipped; the
   scientist renders with its real textures (rung-4 [V]).
5. If load CRASHES or the asset won't resolve by name → do the name-map rename (§8) and
   retest. If arms over-rotate → the repose/AnimBP retarget needs a look (§5).
6. Screenshots are for autonomous runs only (per project rule); for a hands-on test the
   user observes directly.

Former open questions — resolved: (1) **auto-mount wins** [V]: UE mounts
`Content/Paks/LogicMods/…` at startup, no MountPakFile call needed (320c0ab4 runtime proof).
(2) **role at Spawn = the peer SLOT** [V] -- superseded 2026-07-02 late evening by the v93
per-player skin name (net_pump passes player_handshake::SkinForSlot(slot); the role gate
is gone, RULE 2). (3) client seeing its OWN body in its skin: SHIPPED (coop/player/
local_body.cpp -- the first-person immersion fix; AS-BUILT, hands-on pending).

---

## 4. Offline pipeline — BUILT (tools/client_model/) + how to reproduce

**PORTABLE ONE-SHOT (2026-07-02 evening):** `tools/client_model/portable/make_portable.py
[--exe]` builds `portable/dist/` — `convert_model.pyz` (zipapp of the SAME live modules +
embedded cook templates + default profile) / `convert_model.exe` (PyInstaller onefile, no
python needed) + `repak.exe` + `convert.bat` + `README.txt`. Drop next to any `.mdl`, run,
the `.pak` appears in that folder (`--name`, `--profile`, `--keep-work` flags; the driver
warns that the DLL loads the fixed name `hl_einstein_v1sc`). Verified: pyz AND exe runs
reproduce the deployed pak content byte-identically (4/4 sha256). dist/ is gitignored
(game-derived template bytes); rebuild after any module change.

Manual step-by-step (same modules; `python`, not `python3`, has numpy).
2026-07-02: the model carries its ORIGINAL name `hl_einstein_v1sc` (the early "scientist"
naming is retired); the repose profile comes from the `tools/client_model/profiles/` LIBRARY
(`default` = v1 "narrow" — see profiles/README.md):
```
# 1. extract HL model -> geometry + per-vertex bone + bone WORLD matrices + textures
python tools/client_model/mdl_extract.py tools/hl_einstein_v1sc/hl_einstein_v1sc.mdl \
       research/pak_re/mesh_out/hl_einstein
# 2. AUTO repose A-pose -> VOTV T-pose (+ scale) via the DEFAULT library profile (§5)
python tools/client_model/repose.py apply research/pak_re/mesh_out/hl_einstein \
       default tools/hl_einstein_v1sc/hl_einstein_v1sc_tpose.obj
# 3. COOK: Y-mirror to cooked space + HL->anthro rigid remap + splice into kerfurOmega template
python tools/client_model/ue_cook.py    # -> research/pak_re/mesh_out/hl_einstein/hl_einstein_v1sc.uasset/.uexp
# 3b. TEXTURE: atlas -> cooked UTexture2D package tex_hl_einstein_v1sc
python tools/client_model/ue_tex.py cook research/pak_re/mesh_out/hl_einstein/atlas.png \
       research/pak_re/mesh_out/hl_einstein/tex_hl_einstein_v1sc
# 4. PACK (copy the 4 cook outputs into staging, then repak)
cp research/pak_re/mesh_out/hl_einstein/{hl_einstein_v1sc,tex_hl_einstein_v1sc}.{uasset,uexp} \
   research/pak_re/modpak/VotV/Content/Mods/VOTVCoop/
research/pak_re/tools/repak.exe pack --version V11 research/pak_re/modpak research/pak_re/hl_einstein_v1sc.pak
```

Tools (all in `tools/client_model/`, dev-only, RULE 3):
- `mdl_extract.py` — GoldSrc/HL1 `.mdl` → OBJ + per-vertex bone + HL bone WORLD matrices
  (in `model.bones.json`) + textures. DONE.
- `repose.py` — **THE REPOSE AUTOMATION (§5).** `learn` extracts a VOTV T-pose profile
  from a manual example; `apply` reposes with an explicit profile or `auto` (library
  scoring). DONE, validated (residual 0 on each example).
- `ue_cook.py` — **THE COOK.** Sources the reposed T-pose OBJ, applies the exact Y-mirror
  (§2), auto-corrects winding, HL→anthro bone resolution (`bone_keyword` +
  `resolve_bone_targets`: keyword table, else the nearest keyword-mapped ANCESTOR, else a
  COUNTED miss — the pre-2026-07-02 constant-pelvis fallback silently pinned rvi's
  toes/fingers/head-prop = 106/764 verts to the hip), decomposes the template into
  keep/replace segments (identity round-trip proven), encodes the scientist buffers (uint16
  indices, FVector positions, FPackedNormal tangents, half-UVs, rigid skin weights, white
  colors), rebuilds 1 section (BoneMap = all 101 bones), fixes SerialSize/BulkDataStartOffset.
  DONE; output re-parses valid.
- `ue_skelmesh.py` — general UE4.27 cooked-SkeletalMesh parser; round-trips kerfurOmega +
  kel_lmao byte-perfect. Validates the cook output.
- `ue_pkg.py` — UE4.27 package (de)serializer; round-trips byte-identical.
- `profiles/` — the repose profile LIBRARY (format 3; auto-select scores coverage +
  rest-pose similarity, `rejected` entries skipped; provenance + statuses in
  profiles/README.md). Replaced the old single root `votv_tpose_profile.json`.
- `portable/` — `driver.py` (the drop-in-folder orchestrator; auto-LEARNS the profile
  from a manual-pose PSK found next to the .mdl, else auto-selects from the bundled
  library) + `make_portable.py` (bundles the live modules into dist/: pyz + optional
  exe + repak.exe + bat + README).
- `skin_to_rig.py` — SUPERSEDED for the final asset (an early rigid-remap→glb Blender-view
  helper). Not in the cook path; kept as a viewing aid only.
- `mesh_extract/` (C#/CUE4Parse) — game-mesh study. `cue4parse_ref/` — CUE4Parse reader
  sources (MIT) = the byte-order spec. `SPEC.md` — byte-exact cook spec.

**The deliverable:** `research/pak_re/hl_einstein_v1sc.pak` (~570 KB, V11, 4 files) →
`VotV/Content/Mods/VOTVCoop/{hl_einstein_v1sc,tex_hl_einstein_v1sc}.uasset/.uexp` → load as
`/Game/Mods/VOTVCoop/hl_einstein_v1sc` (object `kerfurOmega_KelSkin`) +
`.../tex_hl_einstein_v1sc` (512x256 PF_B8G8R8A8 atlas). Cooked mesh (v1 narrow profile,
the deployed `AE49002C` re-cook -- the v2-profile cook was the same shape wider):
1 section, 1062 verts, 708 tris, 101-bone anthro rig, rigid (1 influence), all bones
keyword-resolved, atlas-remapped UVs (19 tiles), winding template-matched (same side as
the template's -161625 signed volume -- see STATUS). Second model: `rvi_scientist_v1sc`
(764 verts, 38-bone skeleton) cooked from the user's own pose, pak `ED666BE5`.

Workspace: `research/pak_re/` (gitignored) — `mesh_out/` (mdl_extract outputs),
`modpak/` (pak staging), `extracted/` (repak-extracted game meshes = the cook template),
`tools/repak.exe`. Example model: `tools/hl_einstein_v1sc/` (Valve asset, local only).

---

## 5. Repose automation — A-pose → VOTV T-pose (SOLVED)

**Problem:** HL1 models are A-pose (arms ~45° down); the anthro rig binds T-pose (arms
out). Animations authored for the T-pose bind over-rotate an A-pose mesh's arms. The mesh
must be reposed to the T-pose bind (and scaled to dr_kel size) BEFORE cooking.

**Solution — learn it once, apply forever.** The user poses ONE manual example in Blender
(SourceIO imports the mdl on its HL Bip01 rig; `dr_kel.psk` as the size/pose reference;
export a posed PSK); `repose.py learn` extracts a transferable PROFILE from it. Key facts:
- GoldSrc skinning is **rigid (1 bone/vertex)** → the manual repose is EXACTLY a per-bone
  rigid transform. **Uniform scale** (2.580 across all 22 bones on both examples).
- **Profile format 3 (2026-07-02 late, the rvi postmortem; library relearned): per-bone
  LOCAL pose = a FULL rigid delta (R + t) in the bone's rest frame + `rest_local` (the
  source skeleton's rest transforms = the auto-select fit metric) + `status` + placement.**
  History: format 1 stored rotation-only — enough for a pure re-pose (v1 narrow, residual
  0.00009) but it silently DROPPED JOINT TRANSLATIONS (the v2 wide example moves
  shoulder/arm joints outward; rotation-only left a 16-unit residual). Formats 1/2 retired
  (RULE 2); the relearn is drift-free vs the old outputs (einstein v1 max 5.4e-5, v2 0.0).
- **The `up` axis is a CONSTANT of the target space (UE Z-up; the cook is a pure
  Y-negation)** — never inferred from the mesh bbox: the v2 wide arm span (X=209.5)
  exceeds the height (Z=190.2), so the old argmax-bbox heuristic measured "height" along
  the arms and grounded the model sideways (residual 17.58 until fixed).

**The PROFILE LIBRARY (`tools/client_model/profiles/`, user 2026-07-02: keep a base of
profiles):** `tpose_v1_narrow_2026-07-01.json` (active; in-game look preferred),
`tpose_v2_wide_2026-07-02.json` (**rejected** in-game — auto-select skips it),
`tpose_rvi38_2026-07-02.json` (active; the user's manual rvi pose, 38-bone Sven Co-op
skeleton). Provenance + statuses: `profiles/README.md`.

**Validation:** each profile reproduces ITS example to float-zero — v1: max 0.00006;
v2: 0.00005; rvi38: 0.00009 (~190-unit models). Lossless representation.

**Generalization is MEASURED, never assumed (the 2nd-model verdict, rvi_scientist
2026-07-02):** transferring the einstein-learned profile to a DIFFERENT 38-bone skeleton
degraded two ways — 16 bones the profile never saw kept their A-pose (103/764 verts:
toes, fingers, a head-prop), and even covered bones drifted because deltas are
rest-relative (error compounds down the arm chain 2.6 → 12 units at the fingers). Hence:
- `repose.py apply <origDir> auto <out.obj>` — scores every library profile (vertex
  bone-name coverage, then rest-pose geodesic similarity), prints the table, skips
  `rejected`, and ALWAYS reports uncovered vert-carrying bones.
- A manual-pose PSK next to the `.mdl` beats the library: the portable driver
  auto-detects it (exact point/bone correspondence) and LEARNS the model's own exact
  profile (float-zero), saving `<name>.profile.json` next to the pak — the library grows
  from the user's manual poses (rename `tpose_<what>_<date>.json`, commit).

**Learn (a new skeleton family / look):**
```
python tools/client_model/repose.py learn <mdl_extract_dir> <manually_posed.psk> tools/client_model/profiles/<name>.json
# or just drop the posed PSK next to the .mdl and run the portable converter
```

---

## 6. Caveats — updated after the in-game runs (2026-07-02)
a. **CLOSED [V]:** object name `kerfurOmega_KelSkin` at OUR package path loads fine --
   proven pre-rename as `LoadObject('/Game/Mods/VOTVCoop/scientist.kerfurOmega_KelSkin')`
   (log: `outer='/Game/Mods/VOTVCoop/scientist'`); the 2026-07-02 rename moved only the
   PACKAGE name (file placement in the pak) to `hl_einstein_v1sc` -- same mechanism, mesh
   object unrenamed. (The texture package IS cleanly renamed -- ue_tex.py full rename, §8.)
b. **Still approximate (visually acceptable so far):** tangent-X (a synthesized
   perpendicular to the authored normal -- fine while the material has no normal map);
   ImportedBounds = template's; vertex colors = white. (Shading normals themselves are no
   longer approximate -- authored, see STATUS 2026-07-03.)
c. **REOPENED by rung 4, then ROOT-FIXED [V]:** the earlier "no inside-out look" call was
   premature -- the garbled texture masked the facing and a closed mesh's silhouette is
   winding-invariant, so the geometry look could not falsify winding at all. Rung-4 real
   textures exposed a full INSIDE-OUT render. Root fix: ue_cook matches the TEMPLATE's
   measured signed-volume winding side, never an assumed convention (STATUS block has the
   numbers). Re-verified hands-on same day ("теперь нормально").
d. **CLOSED [V]:** UE loads the package whose name-map still references the template's
   paths (mesh package loads + renders; imports resolve to the game's skeleton/materials
   as intended).
e. **CLOSED (measured) 2026-07-02:** pose generalization to a 2nd model — the einstein
   profile on rvi_scientist's 38-bone skeleton produced the BAD pak (A-posed chunks +
   pelvis-pinned toes/fingers). Root-fixed at the pipeline level: profile auto-select
   with printed coverage/fit scoring, learn-from-manual-PSK in the driver, and the
   ancestor-walking bone resolver (§4/§5). rvi re-cooked from the user's own pose:
   positions == manual PSK (max 9e-5), 2020/2020 verts on the resolver's targets.

---

## 7. Textures — DONE [V hands-on 2026-07-02] (the as-built ladder)
The cook reuses the template's material array (kerfurOmega/kel materials, 3 slots; the
1 spliced section uses slot 0). Without our texture the scientist renders with a **kel
texture sampled through the scientist's UVs = garbled** — that was the pre-ladder state;
the ladder below fixed it (STATUS block has the verified as-built summary).

The scientist's own textures ARE extracted: `research/pak_re/mesh_out/hl_einstein/tex/*.png`
(19 GoldSrc textures) named by the PSK materials (`th_einstein_Sci3(Chest).bmp`, …).

**CHOSEN: real textures via a texture-ONLY cook + runtime material binding.** Rationale:
- (i) flat/vertex-color = a crutch (RULE 1/2 — would ship then be ripped out). Rejected.
- A cooked **UMaterial is NOT python-cookable** (cooked materials embed platform shader maps).
  So "cook a material" is off the table entirely.
- A cooked **UTexture2D IS cookable** (pixel format + mips + bulk — CUE4Parse spec vendored),
  and the runtime can bind it WITHOUT any cooked material:
  `CreateDynamicMaterialInstance(slot0)` + `SetTextureParameterValue(<param>, ourTexture)` —
  the hurt-flash infra already proves runtime material swaps on these components.
- (iii) multi-section/multi-material: only if a single ATLAS proves insufficient for a 708-tri
  GoldSrc model (unlikely). Not now.

**Probe-first ladder — ALL RUNGS DONE [V]:**
1. **[V] RE the kel material chain STATICALLY:** slot-0 `inst_kel4_body` = MIC of
   `mat_object_sk` with ONE texture parameter **`tex`** → MID binding viable.
2. **[V] Cook ONE UTexture2D** (Sci3_Chest_ 64x88, no atlas): `LoadObject` returned
   class='Texture2D' in-game.
3. **[V hands-on "текстура растянута"] Runtime render probe:** MID + `SetTextureParameterValue`
   on the RIGHT puppet → the stretched chest texture = cook + binding chain proven.
4. **[V hands-on "теперь нормально"] Atlas bake:** `atlas.py` (19 PNGs → 512x256, 1px
   duplicated-edge gutter) + per-face UV remap in ue_cook (usemtl → tile rect, v-flip fix)
   → real scientist look. Multi-section never needed. Exposed + root-fixed the WINDING
   assumption on the way (STATUS). Feature bind wired into `RemotePlayer::Spawn` (since
   v93: inside `client_model::ApplySkinToBody`, the one apply path).

---

## 8. Name-map rename (fix for §6a/§6d, only if the in-game test needs it)
Repath a package/object inside the `.uasset` name map (e.g. mesh object →
`/Game/Mods/VOTVCoop/hl_einstein_v1sc.hl_einstein_v1sc` — NOT needed for the mesh, §6a).
Rewrite the name-map entries, recompute their FName hashes, shift post-name-map summary
offsets + export SerialOffset + BulkDataStartOffset by the length delta.
**FName hashes CRACKED for real (validated 19/19 BOTH slots vs tex_kel3_skin, 2026-07-02;
the earlier "verified 6/6" formula was WRONG on the NonCase slot):** each name-map entry
stores 2 uint16 after the FString, in this order —
`NonCasePreservingHash = Strihash_DEPRECATED(name) & 0xFFFF` (uppercase each char, process
ONLY the low byte per char, MSB-first CRC table poly 0x04C11DB7, init 0, no final invert),
then `CasePreservingHash = FCrc::StrCrc32(name) & 0xFFFF` (reflected 0xEDB88320, each char
processed as 4 LE bytes, init/final invert standard). Implemented + self-tested in
`tools/client_model/ue_tex.py` (`name_entry()` / `selftest`), which also does the full
package rename (name map + offset shifting + export table patch) that this section
originally deferred.

---

## 9. Rejected approaches (RULE 2 — don't revisit)
- **RealTimeImport → ProceduralMesh + per-frame CPU skinning:** static geometry; animating
  it in our tick = crutch (violates principle 7). Rejected.
- **UE 4.27 editor cook:** ~80 GB; user rejected. We cook in Python.
- **Runtime USkeletalMesh build from glTF (glTFRuntime-style):** fragile, 5000+ LOC.
- **kel_lmao (6-bone) as the target:** a stick placeholder, not the client skin.
- **Manual per-model Blender repose:** was v1's plan; now automated (§5). The manual step
  survives only as the ONE example the profile was learned from.

---

## 10. References
- `tools/client_model/README.md` + `SPEC.md` — pipeline/tools + byte-exact cook spec.
- `tools/client_model/repose.py` + `profiles/` (library; v2 wide = default) — repose automation (§5).
- `research/findings/votv-mp-pak-mount-feasibility-2026-05-25.md` — pak plugins, auto-mount,
  the LoadObject POC (basis for §3A).
- Code seams: `coop/player/remote_player.cpp:87/92/158`, `ue_wrap/puppet.cpp:341-680,525`,
  `ue_wrap/engine_component.cpp:212-222,241-250`.
- SourceIO (`tools/SourceIO/`) — the Blender addon that imports GoldSrc `.mdl` natively
  (used for the manual example); psk-psa (`reference/psk-psa-v9.1.2/`) — PSK import/export.
- Deliverable: `research/pak_re/hl_einstein_v1sc.pak`.
```
