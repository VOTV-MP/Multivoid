# Multivoid branding assets

The project's visual identity. Consumed by the Thunderstore package (WP-9), and available to the
GitHub social preview and the site. **Not** a build input for the DLL — the DLL's own embedded
resources live in `src/votv-coop/resources/` and `src/votv-coop/assets/fonts/`.

| file | tracked | what it is |
|---|---|---|
| `icon.psd` | **NO — see below** | The editable source, 4.8 MB. |
| `icon-512.png` | yes | **The flattened master.** 512x512, PNG, 32bpp with alpha. Supplied by the user 2026-08-25 (second revision, same day), stored verbatim. This is what the 256 is cut from. |
| `icon.png` | yes | **Generated, never hand-edited.** Exactly 256x256 — the size Thunderstore requires at the zip root. |

### Why `icon.psd` is not committed (yet)

It is the only truly irreplaceable file here — flattening is one-way — so it is *not* a file to lose.
But it is 4.8 MB of binary heading into a **public** repository, and a PSD carries more than pixels
(layer names, hidden layers, embedded originals). That is a call for the user, not a default:

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

The art is a Multivoid screenshot: the HL-derived client model in the base airlock, under the
Multivoid diamond. That is the same asset class as the shipped scientist skins, which
`docs/UE4SS_ARC.md` §7.6 settled as a USER DECISION on 2026-08-23 — it ships. Recorded here so the
question is not re-opened at package time.
