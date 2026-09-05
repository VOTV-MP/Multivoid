# Multivoid branding assets

The project's visual identity: the icon the Thunderstore package carries, also available to the
GitHub social preview and the site. Not a build input for the DLL, whose own embedded resources
live in `src/votv-coop/resources/` and `src/votv-coop/assets/fonts/`.

| file | tracked | what it is |
|---|---|---|
| `icon-512.png` | yes | The flattened master: 512x512 PNG, 32bpp with alpha (md5 `5fdbfb20d50a6883e9f054d40bc01d38`). The 256 is cut from it. The art it replaced is in git history. |
| `icon.png` | yes | Generated, never hand-edited. Exactly 256x256, the size Thunderstore requires at the zip root. |
| `icon.psd` | no | The editable source of the superseded art, 4.8 MB. It is not the source of the current master; do not open it expecting to edit what ships. |

## Why `icon.psd` is not committed

It is the editable source of art that no longer ships, and the art that does ship has no editable
source in this tree at all, so the case for committing it is weak. The case against is the usual
one: 4.8 MB of binary in a public repository, and a PSD carries more than pixels (layer names,
hidden layers, embedded originals). If it is ever committed, it gets a leak pass first, like
anything else going public. While it stays untracked it is at risk from `git clean -xd`; keep a
copy outside the tree.

## Regenerating `icon.png`

Any change to the master must be followed by this, from the repo root. It is a one-liner rather
than a script on purpose: an icon does not rot, so there is nothing here for CI to police.

```powershell
Add-Type -AssemblyName System.Drawing
$d='assets\branding'; $in=[System.Drawing.Image]::FromFile("$PWD\$d\icon-512.png")
$b=New-Object System.Drawing.Bitmap 256,256,([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$g=[System.Drawing.Graphics]::FromImage($b)
$g.CompositingMode='SourceCopy'; $g.InterpolationMode='HighQualityBicubic'; $g.PixelOffsetMode='HighQuality'
$g.DrawImage($in,(New-Object System.Drawing.Rectangle 0,0,256,256),0,0,$in.Width,$in.Height,'Pixel')
$g.Dispose(); $in.Dispose(); $b.Save("$PWD\$d\icon.png",[System.Drawing.Imaging.ImageFormat]::Png); $b.Dispose()
```

Then check what you actually wrote. Thunderstore rejects anything that is not exactly 256x256,
and it rejects it at upload time, which is the worst moment to find out:

```powershell
Add-Type -AssemblyName System.Drawing
$v=[System.Drawing.Image]::FromFile("$PWD\assets\branding\icon.png"); "$($v.Width)x$($v.Height)"; $v.Dispose()
```

## Provenance

**The current art.** Five Half-Life-derived scientist models posed around the workstation room,
each tagged with a nickname and a ping in the coop nameplate style, under a pixel-font `MultiVoid`
wordmark. The same asset class as the shipped scientist skins, which ship by decision; recorded
here so the question is not reopened at package time. The master arrived as a flattened PNG with
no editable source in the tree, so whatever produced it lives outside this repo.

**The superseded art.** The Half-Life-derived client model alone in the base airlock, under the
Multivoid diamond. `icon.psd` on disk is that image's editable source.

**Alpha, measured not assumed.** The current master has genuinely transparent corners: `[0,0,0,0]`
at the four corner pixels, and the generated 256 carries 3,991 fully transparent plus 1,753
partial-alpha pixels. The superseded master did not: its corners measured alpha `255` (the rounded
corners were painted dark, not cut out). Thunderstore supports transparency, so the current
behaviour is the wanted one; it is written down because the downscale above is the step that
could silently destroy it.
