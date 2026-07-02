# Repose profile LIBRARY — the "VOTV T-pose standard" collection

One json per learned example (user 2026-07-02: keep a base of profiles, not one file).
A profile is learned from ONE manually posed+scaled PSK (`repose.py learn`) and is
EXACT (float-zero) on the skeleton it was learned from; on a different skeleton it
degrades (uncovered bones keep A-pose, covered deltas drift — measured on
rvi_scientist: 103/764 verts A-posed, arm chain drift 2.6→12 units). So the fit is
MEASURED, never assumed:

- `repose.py apply <origDir> auto <out.obj>` / the portable converter **auto-select**:
  every profile is scored against the model — vertex-weighted bone-name **coverage**
  first, then **rest-pose similarity** (geodesic angle between the model's and the
  profile's recorded `rest_local` rotations; ~0° = "the skeleton I was learned on").
  `status: "rejected"` profiles are skipped. The table is printed on every run.
- A manual-pose PSK next to the .mdl beats the library: the converter LEARNS the
  model's own exact profile from it (portable/driver.py auto-detects it by exact
  point/bone correspondence). That learned `<name>.profile.json` is the candidate to
  add here (rename to `tpose_<what>_<date>.json`, commit) — the library GROWS from
  the user's manual poses; future models with a similar skeleton auto-pick it.

| file | learned from | look | status |
|---|---|---|---|
| `tpose_v1_narrow_2026-07-01.json` | `hl_einstein_v1sc.psk` (22-bone HL Bip01) | narrow arm span 177.1 (the first manual example) | **active** — in-game look preferred (hands-on 2026-07-02 evening) |
| `tpose_v2_wide_2026-07-02.json` | `hl_einstein_v1sc_new_profile.psk` | wide arm span 209.5, matches the anthro template proportions (215) | **rejected** — in-game look vetoed 2026-07-02 evening ("переделай обратно под v1"); auto-select skips it |
| `tpose_rvi38_2026-07-02.json` | `rvi_scientist_v1sc_my_pose_good.psk` (38-bone Sven Co-op Bip01: +toes/fingers/head-prop) | user's manual rvi pose, span 194.8, height 188 | **active** |

**Format 3 (the only supported format; library relearned 2026-07-02):** per-bone
`pose_local` (4x4 R+t delta in the bone's rest frame — the transferable re-pose;
translations carry joint moves like v2's widened shoulders) **+ `rest_local`** (the
source skeleton's rest local transforms — the auto-select fit metric) + `status` +
placement (`target_height`/`foot`/`center`). Formats 1/2 are retired (RULE 2): the
relearn was verified drift-free vs the old outputs (einstein v1 max 5.4e-5, v2 0.0).

The learn-time `up` axis is a CONSTANT of the target space (UE Z-up; the cook is a
pure Y-negation) — never inferred from the mesh bbox: a T-pose arm span wider than
the height flips the argmax and the model gets measured/grounded sideways (the
2026-07-02 17.58-residual bug).
