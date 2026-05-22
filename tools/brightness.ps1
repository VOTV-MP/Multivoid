param([Parameter(Mandatory=$true)][string]$Path)
Add-Type -AssemblyName System.Drawing
$b = New-Object System.Drawing.Bitmap($Path)
$s = 0.0; $n = 0
for ($x = 0; $x -lt $b.Width; $x += 32) {
  for ($y = 0; $y -lt $b.Height; $y += 32) {
    $c = $b.GetPixel($x, $y); $s += ($c.R + $c.G + $c.B); $n++
  }
}
$b.Dispose()
"{0}: avg-brightness = {1:N1} (0-255)" -f (Split-Path $Path -Leaf), ($s / $n / 3)
