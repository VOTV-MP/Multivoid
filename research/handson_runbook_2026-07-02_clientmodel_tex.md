# Hands-on runbook — client-model TEXTURE rung 4 (atlas, 2026-07-02, take 3)

> **OUTCOME (take 3, same day): VERIFIED — user "теперь нормально".** The scientist renders
> solid from outside with the real 19-tile look: texture axis CLOSED, winding axis CLOSED.
> Audit on the shipped diff: all touched functions COLD-path, zero findings >= 80 confidence;
> flagged remote_player.cpp 913 LOC > 800 soft cap -> queued extraction (ragdoll subsystem ->
> remote_player_ragdoll.{h,cpp}). Remaining: the coop two-peer visual (section below).

> **OUTCOME (coop visual, same day): VERIFIED — user "Работает amazing".** Host+client facing
> each other: the client's puppet = textured scientist on the host's screen, the host's puppet
> = kel on the client's (role gate). FEATURE COMPLETE. (Launch was blocked first by a
> triple-masked launcher failure — pak locked by the running host + mp.py UnicodeEncodeError +
> instantly-closing .bat — root-fixed in `c1403fd7`.)

> **Take-2 look verdict: INSIDE-OUT** ("смотрю спереди — вижу спину... как будто изнутри") — all
> front faces culled: the cook's winding heuristic ("geometric-outward = front") was an ASSUMPTION
> and it was backwards vs the engine. Measured (signed volume, divergence theorem): the game's own
> kerfurOmega_KelSkin stores **CW-outward** (signedVol=-161625); our cook emitted CCW-outward
> (+102223). Present in the geometry-verified take too — a closed mesh's silhouette is
> winding-invariant and the rung-3 garble hid the facing. Root fix: ue_cook now MEASURES the
> template's winding side and matches it (never assumes); shading normals decoupled (stay
> geometrically outward). Recooked: ours now **-102223 = template side**; UVs re-verified
> 1062/1062 in-tile.

> Rung 3 VERIFIED earlier ("текстура растянута") = texture cook + MID binding chain PROVEN.

**Deployed (take 3):** DLL `fff53e04672daa9d` (unchanged) + `scientist.pak` `5edabac7c5096960`
(winding-fixed mesh + atlas), all 4 install folders hash-verified 8/8.
`[dev] client_model_probe=1` still set in the HOST ini.

## What changed (rung 4)
1. **Atlas** (`tools/client_model/atlas.py`, NEW): all 19 extracted scientist PNGs shelf-packed
   into ONE 512x256 canvas (41% filled, 1px duplicated-edge gutter against bilinear bleed; no mips
   are cooked so 1px suffices). Visually verified: face/coat/arms/legs/shoes all present, upright.
2. **Per-face UV remap** (`ue_cook.py`): the OBJ's `usemtl` per-face material now drives each
   corner's UV into its atlas tile. Includes the V-orientation fix: cooked v = 1 - obj_v (GoldSrc
   t=0 = texture top; OBJ carries v-up; UE samples v-DOWN with mip row 0 = PNG row 0) — rung 3 was
   rendering each tile vertically flipped (invisible in the garble).
   Byte-verified: re-parsed the cooked .uexp — **1062/1062 UVs land inside atlas tile rects**
   (u[0.002,0.984] v[0.004,0.629]); 153 verts in the Face1 tile.
3. **Texture cook**: `ue_tex.py cook atlas.png` -> tex_scientist 512x256 PF_B8G8R8A8, 1 inline mip
   (524288B pixels), self re-parse OK.
4. **FEATURE wiring** (the coop path, not just the probe): `client_model::GetClientPuppetTexture()`
   (lazy one-shot cache) + `ApplyClientPuppetTexture()` (slot-0 MID + 'tex' param on BOTH body
   components); `RemotePlayer::Spawn(useClientModel)` binds it right after SpawnPuppet succeeds.
   Client puppets now get mesh + texture; host puppet stays kel.

## The 30-second look (solo host — probe pair)
1. Launch the host as usual; load in; stand still ~10 s.
2. The pair spawns ~3 m ahead: LEFT = kel control, RIGHT = scientist.
3. **Verdicts (RIGHT puppet):**
   - **Actual HL scientist look** — white lab coat, tie, face with mustache in the right places:
     **rung 4 GREEN, texture axis CLOSED.** Next = coop visual (host+client facing each other).
   - **Right textures but scrambled placement** (coat pixels on face etc.): tile mapping bug —
     send the log + a screenshot.
   - **Per-part upside-down textures**: my V-orientation derivation is wrong — one-line fix
     (drop the 1-v flip), recook, redeploy.
   - **Unchanged single-texture stretch**: stale pak (hash-check) or the mesh didn't recook.
4. Log (host `votv-coop.log`):
   - `asset_load: LoadObject('/Game/Mods/VOTVCoop/tex_scientist.tex_scientist') -> <ptr> [... class='Texture2D' ...]`
   - `[CLIENTMODEL-PROBE] RIGHT tex bind: comp=<ptr> mid=<ptr> tex=<ptr>` (x2) + `bound on 2/2 slots`

## The coop look (when both PCs are free — the real feature)
Host + client join; stand FACING EACH OTHER:
- On the HOST's screen the client's puppet = scientist (mesh + atlas texture; log:
  `RemotePlayer::Spawn: CLIENT peer -> custom client mesh` + `client_model: custom texture ... bound on 2/2`).
- On the CLIENT's screen the host's puppet = stock kel (role gate: slot 0 = host = kel).

## Honest status
Rung 4 AS-BUILT + deployed (hash-verified 8/8); look NOT yet done. Feature texture-bind wired but
exercised only by the coop path (solo probe exercises the probe-side bind). Audit agent running on
the shipped diff. 20 commits held local (unpushed); this build's diff uncommitted until the look.
