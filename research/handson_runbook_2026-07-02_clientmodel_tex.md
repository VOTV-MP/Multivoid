# Hands-on runbook — client-model TEXTURE rung 3 (2026-07-02)

**Deployed:** DLL `a142e2bc39cc58b5` + `scientist.pak` `319f39d650c4` (4 files: mesh + tex_scientist),
all 4 install folders hash-verified. `[dev] client_model_probe=1` set in the HOST ini. HEAD `72a21a42`.

**Context:** GEOMETRY is VERIFIED [V hands-on 2026-07-02 "Работает"] — probe take 3 (same mesh in BOTH
body slots) rendered the animated scientist next to the kel control; the garbled texture was the
expected §7 wrong. This build adds texture rungs 1-3: a cooked `UTexture2D`
(`/Game/Mods/VOTVCoop/tex_scientist`, Sci3_Chest_ 64x88, PF_B8G8R8A8, 1 inline mip) + runtime
`CreateDynamicMaterialInstance(slot0)` + `SetTextureParameterValue('tex', tex)` bound on the RIGHT
puppet's BOTH body slots by the probe.

## The 30-second look (solo host)
1. Launch the host as usual; load into the world; stand still ~10 s.
2. The pair spawns ~3 m in front of the camera: LEFT = kel control, RIGHT = scientist.
3. **Verdicts (what the RIGHT one looks like):**
   - **Torso/skin-colored body** (the chest texture stretched over the whole model — uniform flesh/
     shirt tone instead of the kel-garble): **texture cook + MID binding PROVEN** → rung 4 (atlas of
     all 19 textures + per-face UV remap) gives the real look.
   - **Unchanged kel-garble**: binding no-op → read the log lines below.
   - **RIGHT missing/black**: texture load or format problem → log below.
4. Log (host `votv-coop.log`):
   - `asset_load: LoadObject('/Game/Mods/VOTVCoop/tex_scientist.tex_scientist') -> <ptr> [obj=... class='Texture2D' ...]`
   - `[CLIENTMODEL-PROBE] RIGHT tex bind: comp=<ptr> mid=<ptr> tex=<ptr>` (x2, both slots)
   - `[CLIENTMODEL-PROBE] RIGHT texture bound on 2/2 slots`

## Honest status
Rungs 1-3 AS-BUILT (`72a21a42`); rung-3 look NOT yet done. Rung 4 (atlas + UV remap in ue_cook +
wiring the texture bind into the FEATURE apply path, not just the probe) starts after this look is
green. 19 commits held local (unpushed).
