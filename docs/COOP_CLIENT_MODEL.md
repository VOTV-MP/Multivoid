# COOP client model — custom per-client skeletal mesh (full handoff doc)

**Read top-to-bottom.** Goal: give every remote CLIENT puppet a custom character
model (the user's own mesh) while the HOST stays Dr. Kel. First model = a Half-Life 1
GoldSrc scientist. The pipeline generalizes to any HL1 humanoid `.mdl`.

**STATUS (2026-07-02, geometry VERIFIED in-game -- textures are the active axis):**
- **RUNTIME = DONE + PROVEN WORKING (§3), commit `320c0ab4`.** Autonomous 2-peer LAN test
  (host s_1234 + fresh client): pak auto-mounts, `URyRuntimeObjectHelpers::LoadObject` returns
  OUR package's SkeletalMesh, `RemotePlayer::Spawn` applies it to the client puppet (role-gated:
  slot 0 host = kel, slots >= 1 clients = custom), the puppet animates + pose-tracks (trail 0cm).
  Files: `ue_wrap/asset_load.{h,cpp}`, `coop/player/client_model.{h,cpp}`, `RemotePlayer::Spawn`,
  `net_pump` role gate, `deploy-all.ps1` pak ship. The doc's open question #2 (peer role at Spawn)
  = the SLOT, resolved.
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
- **TEXTURES = THE ACTIVE AXIS (§7).** Confirmed in-game: scientist shape with garbled kel
  material over scientist UVs (expected-wrong). The pixels exist (19 extracted GoldSrc PNGs);
  delivery is missing. Plan: cook ONLY a `UTexture2D` (atlas) offline -- a cooked UMaterial is
  NOT python-cookable (shader maps) -- and bind it at runtime via CreateDynamicMaterialInstance +
  SetTextureParameterValue on the existing slot material (the hurt-flash material-swap infra
  proves runtime material ops). Probe-first ladder in §7.
- Runtime + probe + two-slot invariant built/deployed/committed; the pak ships to all peers.

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
   → mdl_extract.py   → model.obj + model.bones.json(+bone WORLD mats) + tex/*.png
   → repose.py apply  → scientist_tpose.obj        (auto A-pose→VOTV T-pose + scale, §5)
   → ue_cook.py       → scientist.uasset/.uexp      (Y-mirror to cooked space, HL→anthro
                                                     rigid bone remap, splice into template)
   → repak pack       → scientist.pak               (VotV/Content/Mods/VOTVCoop/scientist, V11)
SHIP: scientist.pak deployed to EVERY peer (client-side visual asset).
RUNTIME (mod):   [TODO — §3]
  mount scientist.pak → LoadObject the mesh → on each puppet spawn, by REMOTE ROLE:
    client → SetSkeletalMesh(scientist) + SetAnimClass(anthro AnimBP)
    host   → keep current mesh (today's behaviour)
```

Two halves: the offline cook (done, §4/§5) and the runtime load+apply+test (§3, the task).

The repose (§5) is learned ONCE from a manual example and is now a reusable profile;
adding a NEW model is just `mdl_extract → repose.py apply → ue_cook → repak` (no Blender).

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

## 3. RUNTIME: load the pak + apply to clients + TEST  ← THE OTHER AGENT'S TASK

Prior RE: `research/findings/votv-mp-pak-mount-feasibility-2026-05-25.md`.

### 3A. Load the cooked mesh (reflection only, no UE4SS)
VOTV ships reflection-callable pak plugins (call via the mod's `R::FindClass` /
`R::FindFunction` / `ParamFrame` on the game thread):
- **`URyRuntimeObjectHelpers::LoadObject(FString fullObjectPath) → UObject*`** —
  synchronous load by `/Game/...` path. **Use:**
  `LoadObject("/Game/Mods/VOTVCoop/scientist.kerfurOmega_KelSkin")` → the `USkeletalMesh*`.
  (The OBJECT name is `kerfurOmega_KelSkin`, not `scientist` — §6a; rename is §8.)
- `URyRuntimePakHelpers::MountPakFile(FString path) → bool` — mount from an explicit path.
- `UPakLoaderLibrary::{MountPakFile, GetPakFileObject, GetPakFileClass}` — 2nd plugin (fallback).
- **UE4 auto-mounts any `.pak` under `Content/Paks/` at startup.** Simplest deploy:
  drop `scientist.pak` at `…/VotV/Content/Paks/LogicMods/votv-coop/scientist.pak`; it's
  mounted before the mod boots → the mesh is resident → `LoadObject` returns it. Mount
  root `/Game/Mods/VOTVCoop/`.
- Call context: resolve the plugin CDO/target + `FindFunction` + `ParamFrame` (mirror
  the feasibility doc's ~40-LOC `LoadCoopAssetClass` POC). Game thread, after world-ready.
  Graceful-degrade if the pak/asset is missing (keep the default mesh).
- **Deploy:** `scientist.pak` must ship to EVERY peer (client-side visual) — extend
  `tools/deploy-all.ps1` to copy the pak into each peer's `Content/Paks/LogicMods/…`.

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
Preconditions: build+deploy the 3A/3B runtime change; deploy `scientist.pak` to BOTH peers.
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
4. **EXPECTED-WRONG (not a failure):** the scientist's **texture is garbled** — it's
   currently the kel material sampled through the scientist's UVs (§7). Shape/rig/anim
   are the real test; textures are the next feature.
5. If load CRASHES or the asset won't resolve by name → do the name-map rename (§8) and
   retest. If arms over-rotate → the repose/AnimBP retarget needs a look (§5).
6. Screenshots are for autonomous runs only (per project rule); for a hands-on test the
   user observes directly.

Open questions: (1) auto-mount `Content/Paks/LogicMods/…` vs mod `MountPakFile`; auto
first. (2) confirm each `RemotePlayer` carries host/client role at `Spawn()` time.
(3) client seeing its OWN body as scientist (local pawn) — separate, later.

---

## 4. Offline pipeline — BUILT (tools/client_model/) + how to reproduce

Reproduce the pak from the source mdl (all pure Python; `python`, not `python3`, has numpy):
```
# 1. extract HL model -> geometry + per-vertex bone + bone WORLD matrices + textures
python tools/client_model/mdl_extract.py tools/hl_einstein_v1sc/hl_einstein_v1sc.mdl \
       research/pak_re/mesh_out/hl_einstein
# 2. AUTO repose A-pose -> VOTV T-pose (+ scale to dr_kel height) via the learned profile (§5)
python tools/client_model/repose.py apply research/pak_re/mesh_out/hl_einstein \
       tools/client_model/votv_tpose_profile.json tools/hl_einstein_v1sc/scientist_tpose.obj
# 3. COOK: Y-mirror to cooked space + HL->anthro rigid remap + splice into kerfurOmega template
python tools/client_model/ue_cook.py    # -> research/pak_re/mesh_out/hl_einstein/scientist.uasset/.uexp
# 4. PACK (copy cook output into staging, then repak)
cp research/pak_re/mesh_out/hl_einstein/scientist.uasset \
   research/pak_re/mesh_out/hl_einstein/scientist.uexp \
   research/pak_re/modpak/VotV/Content/Mods/VOTVCoop/
research/pak_re/tools/repak.exe pack --version V11 research/pak_re/modpak research/pak_re/scientist.pak
```

Tools (all in `tools/client_model/`, dev-only, RULE 3):
- `mdl_extract.py` — GoldSrc/HL1 `.mdl` → OBJ + per-vertex bone + HL bone WORLD matrices
  (in `model.bones.json`) + textures. DONE.
- `repose.py` — **THE REPOSE AUTOMATION (§5).** `learn` extracts the VOTV T-pose profile
  from a manual example; `apply` auto-reposes any HL Bip01 model. DONE, validated (residual 0).
- `ue_cook.py` — **THE COOK.** Sources the reposed T-pose OBJ, applies the exact Y-mirror
  (§2), auto-corrects winding, HL→anthro rigid bone remap, decomposes the template into
  keep/replace segments (identity round-trip proven), encodes the scientist buffers (uint16
  indices, FVector positions, FPackedNormal tangents, half-UVs, rigid skin weights, white
  colors), rebuilds 1 section (BoneMap = all 101 bones), fixes SerialSize/BulkDataStartOffset.
  DONE; output re-parses valid.
- `ue_skelmesh.py` — general UE4.27 cooked-SkeletalMesh parser; round-trips kerfurOmega +
  kel_lmao byte-perfect. Validates the cook output.
- `ue_pkg.py` — UE4.27 package (de)serializer; round-trips byte-identical.
- `votv_tpose_profile.json` — the learned "VOTV T-pose standard" for HL Bip01 (22 per-bone
  local rotations + target height). Reusable across models.
- `skin_to_rig.py` — SUPERSEDED for the final asset (an early rigid-remap→glb Blender-view
  helper). Not in the cook path; kept as a viewing aid only.
- `mesh_extract/` (C#/CUE4Parse) — game-mesh study. `cue4parse_ref/` — CUE4Parse reader
  sources (MIT) = the byte-order spec. `SPEC.md` — byte-exact cook spec.

**The deliverable:** `research/pak_re/scientist.pak` (58 KB, V11) →
`VotV/Content/Mods/VOTVCoop/scientist.uasset/.uexp` → loads as
`/Game/Mods/VOTVCoop/scientist` (object `kerfurOmega_KelSkin`). Cooked mesh: 1 section,
1062 verts, 708 tris, 101-bone anthro rig, rigid (1 influence), 0 pelvis-fallbacks
(all verts mapped to real anthro bones), winding 0.76 outward.

Workspace: `research/pak_re/` (gitignored) — `mesh_out/` (mdl_extract outputs),
`modpak/` (pak staging), `extracted/` (repak-extracted game meshes = the cook template),
`tools/repak.exe`. Example model: `tools/hl_einstein_v1sc/` (Valve asset, local only).

---

## 5. Repose automation — A-pose → VOTV T-pose (SOLVED)

**Problem:** HL1 models are A-pose (arms ~45° down); the anthro rig binds T-pose (arms
out). Animations authored for the T-pose bind over-rotate an A-pose mesh's arms. The mesh
must be reposed to the T-pose bind (and scaled to dr_kel size) BEFORE cooking.

**Solution — learn it once, apply forever.** The user did it manually ONE time (SourceIO
imports the mdl on its HL Bip01 rig; import `dr_kel.psk` as the size/pose reference; pose +
scale the scientist to VOTV's T-pose; export `hl_einstein_v1sc.psk`). Key facts measured
from that example (`repose.py learn`):
- GoldSrc skinning is **rigid (1 bone/vertex)** → the manual repose is EXACTLY a per-bone
  rigid transform.
- **Uniform scale 2.580, identical across all 22 bones** (73.6 → 189.9 units = dr_kel height).
- Bone head offsets are rest-invariant under posing → the whole repose reduces to ONE
  transferable quantity per bone: a **LOCAL pose rotation** (in that bone's own frame) +
  a target height. That IS the VOTV T-pose standard → `votv_tpose_profile.json`.

**Validation:** applying the profile back to the A-pose (`repose.py apply`) reproduces the
manual PSK to **max residual 0.00009 / mean 0.00005** on a ~190-unit model (floating-point
zero — visually confirmed identical in Blender by the user). The profile stores only 22
rotations + a height, yet reconstructs all 379 verts exactly → the representation is lossless.

**Generalization:** the profile is model-INDEPENDENT (local bone rotations; bone lengths
come from each model's own skeleton; scale derives from target height). Any HL Bip01 model:
`mdl_extract → repose.py apply <profile>` → T-posed geometry, no Blender. (Not yet tested on
a 2nd model — expected to work for same-skeleton HL humanoids; a wildly different rig would
need a re-`learn` from a new manual example.)

**Re-learn (if the target/rig changes):**
```
python tools/client_model/repose.py learn <mdl_extract_dir> <manually_posed.psk> <profile.json>
```

---

## 6. Caveats — offline-valid, IN-GAME UNTESTED (verify on first load, §3C)
a. **Object name** is still `kerfurOmega_KelSkin` (packed at a new PACKAGE path, not
   renamed) → LoadObject path is `/Game/Mods/VOTVCoop/scientist.kerfurOmega_KelSkin`.
   Cosmetic; a clean rename is §8 if the load refuses on the name.
b. Tangents approximate (computed normal + a perpendicular); ImportedBounds = template's;
   vertex colors = white. Fine for "does it render / animate"; refine later.
c. **Winding 0.76 outward** — the Y-mirror winding was auto-kept (majority outward; the
   rest are genuine humanoid concavities). If the mesh shows inside-out in-game, force the
   opposite winding in `ue_cook` (the empirical flip test is one line).
d. **Untested:** whether UE loads a package whose name-map still references the template's
   paths. LoadObject resolves by file path + finds the export by name, so it likely works;
   if it REFUSES on first test, do the rename (§8).
e. **Pose generalization** unverified on a 2nd model (§5) — only the source model is proven.

---

## 7. Textures — THE ACTIVE AXIS (chosen plan + probe-first ladder, 2026-07-02)
The cook reuses the template's material array (kerfurOmega/kel materials, 3 slots; the
1 spliced section uses slot 0). So the scientist renders with a **kel texture sampled
through the scientist's UVs = garbled** — confirmed in-game 2026-07-02 (shape/rig/anim
verified correct; only the texture is wrong).

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

**Probe-first ladder (each rung verified before the next):**
1. **RE the kel material chain STATICALLY** (SDK dump / FModel): is slot 0's material (or its
   parent) parameterized with a TEXTURE parameter (name?), or does it sample a hardcoded
   TextureObject? MID `SetTextureParameterValue` only works on a parameter. If unparameterized,
   find a parameterized game material to use as the MID base instead.
2. **Cook ONE UTexture2D** (single dominant scientist texture, no atlas yet) into the pak;
   verify `LoadObject` returns class='Texture2D' (parse-level probe).
3. **Runtime render probe:** in `client_model_probe`, MID + SetTextureParameterValue on the
   RIGHT puppet's slot 0 → user looks: scientist-colored = texture cook + binding proven.
4. **Atlas bake:** composite the 19 PNGs into one atlas + remap the OBJ UVs per-face at cook
   (pure PIL/numpy); recook mesh+atlas → final look. Multi-section only if atlas quality fails.

---

## 8. Name-map rename (fix for §6a/§6d, only if the in-game test needs it)
Repath the package `kerfurOmega_KelSkin` → `scientist` inside the `.uasset` name map so
LoadObject is clean (`/Game/Mods/VOTVCoop/scientist.scientist`). Rewrite the 2 name-map
entries, recompute their FName hashes, shift post-name-map summary offsets + export
SerialOffset + BulkDataStartOffset by the length delta.
**FName hashes CRACKED (verified 6/6):** each entry stores 2 uint16 after the FString —
`CasePreservingHash = FCrc::StrCrc32(name) & 0xFFFF` (CRC32 poly 0xEDB88320, reflected),
`NonCasePreservingHash = StrCrc32(name.lower()) & 0xFFFF`. Safe to implement in
`ue_pkg`/`ue_cook`. Not done because the object-name path works for LoadObject; do it only
if the first in-game load fails on the name.

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
- `tools/client_model/repose.py` + `votv_tpose_profile.json` — repose automation (§5).
- `research/findings/votv-mp-pak-mount-feasibility-2026-05-25.md` — pak plugins, auto-mount,
  the LoadObject POC (basis for §3A).
- Code seams: `coop/player/remote_player.cpp:87/92/158`, `ue_wrap/puppet.cpp:341-680,525`,
  `ue_wrap/engine_component.cpp:212-222,241-250`.
- SourceIO (`tools/SourceIO/`) — the Blender addon that imports GoldSrc `.mdl` natively
  (used for the manual example); psk-psa (`reference/psk-psa-v9.1.2/`) — PSK import/export.
- Deliverable: `research/pak_re/scientist.pak`.
```
