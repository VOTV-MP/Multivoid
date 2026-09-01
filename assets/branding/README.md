# Multivoid branding assets

The project's visual identity. Consumed by the Thunderstore package (WP-9), and available to the
GitHub social preview and the site. **Not** a build input for the DLL — the DLL's own embedded
resources live in `src/votv-coop/resources/` and `src/votv-coop/assets/fonts/`.

| file | tracked | what it is |
|---|---|---|
| `icon.psd` | **NO — see below** | The editable source **of the RETIRED art**, 4.8 MB. It is NOT the source of the current master (see Provenance) — do not open it expecting to edit what ships. |
| `icon-512.png` | yes | **The flattened master.** 512x512, PNG, 32bpp with alpha. **Replaced 2026-09-01** by the user's third revision, stored verbatim (`md5 5fdbfb20d50a6883e9f054d40bc01d38`); the 2026-08-25 art it supersedes is in git history. This is what the 256 is cut from. |
| `icon.png` | yes | **Generated, never hand-edited.** Exactly 256x256 — the size Thunderstore requires at the zip root. |

### Why `icon.psd` is not committed (yet)

**Re-stated 2026-09-01:** it used to be the one irreplaceable file here. It is not any more — it is
the editable source of art that no longer ships, and the art that DOES ship has no editable source in
this tree at all. So the case for committing it is weaker than it was, and the case against is
unchanged: 4.8 MB of binary heading into a **public** repository, and a PSD carries more than pixels
(layer names, hidden layers, embedded originals). Still a call for the user, not a default:

- **to commit it:** `git add -f assets/branding/icon.psd` (`.gitignore` does not currently block it;
  it is simply not staged) — and it should get a leak pass first, like anything else going public.
- **while it stays untracked:** it is at risk from `git clean -xd`. Keep a copy outside the tree.

## Regenerating `icon.png`

Any change to the master must be followed by this, from the repo root. It is a one-liner rather than
a script on purpose: an icon does not rot, so there is nothing here for CI to police.

```powershell
Add-Type -AssemblyName System.Drawing
$d='assets\branding'; $in=[System.Drawing.Image]::FromFile("$PWD\$d\icon-512.png")
$b=New-Object System.Drawing.Bitmap 256,256,([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$g=[System.Drawing.Graphics]::FromImage($b)
$g.CompositingMode='SourceCopy'; $g.InterpolationMode='HighQualityBicubic'; $g.PixelOffsetMode='HighQuality'
$g.DrawImage($in,(New-Object System.Drawing.Rectangle 0,0,256,256),0,0,$in.Width,$in.Height,'Pixel')
$g.Dispose(); $in.Dispose(); $b.Save("$PWD\$d\icon.png",[System.Drawing.Imaging.ImageFormat]::Png); $b.Dispose()
```

Then check what you actually wrote — Thunderstore rejects anything that is not exactly 256x256, and
it rejects it at upload time, which is the worst moment to find out:

```powershell
Add-Type -AssemblyName System.Drawing
$v=[System.Drawing.Image]::FromFile("$PWD\assets\branding\icon.png"); "$($v.Width)x$($v.Height)"; $v.Dispose()
```

## Provenance

**Current art (2026-09-01).** Five HL-derived scientist models posed around the workstation room,
each tagged with a nickname and a ping in the coop nameplate style, under a pixel-font `MultiVoid`
wordmark. Same asset class as the shipped scientist skins, which `docs/UE4SS_ARC.md` §7.6 settled as
a USER DECISION on 2026-08-23 — it ships. Recorded here so the question is not re-opened at package
time; the extra models change nothing about that decision, they are more of the same class.

**Superseded art (2026-08-25).** The HL-derived client model alone in the base airlock, under the
Multivoid diamond. `icon.psd` on disk is that image's editable source. The 2026-09-01 master arrived
as a flattened PNG with **no editable source in the tree**, so flattening is not the only one-way
step here any more — whatever produced it lives outside this repo.

**Alpha, measured not assumed (2026-09-01).** The current master has genuinely transparent corners:
`[0,0,0,0]` at the four corner pixels, and the generated 256 carries 3,991 fully-transparent plus
1,753 partial-alpha pixels. **The superseded master did not** — its corners measured alpha `255`
(the rounded corners were painted dark, not cut out), so the older claim that "alpha survives" was
describing a channel that was uniformly opaque. Thunderstore supports transparency, so the new
behaviour is the wanted one; it is written down because the downscale above is the step that could
silently destroy it.
