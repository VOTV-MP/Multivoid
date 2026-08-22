$root = "d:\Projects\Programming\VOTV_MP"
$folders = @(
    "$root\Game_0.9.0n_HOST\WindowsNoEditor\VotV\Binaries\Win64",
    "$root\Game_0.9.0n_CLIENT_1\WindowsNoEditor\VotV\Binaries\Win64",
    "$root\Game_0.9.0n_CLIENT_2\WindowsNoEditor\VotV\Binaries\Win64",
    "$root\Game_0.9.0n_CLIENT_3\WindowsNoEditor\VotV\Binaries\Win64"
)
foreach ($f in $folders) {
    if (-not (Test-Path $f)) { Write-Output "MISSING: $f"; continue }
    Write-Output "=== $f ==="
    Get-ChildItem $f -Filter "*.dll" | Where-Object { $_.Name -match 'xinput|multivoid|votv-coop|UE4SS|dwmapi' } | Select-Object Name, Length, LastWriteTime | Format-Table -AutoSize | Out-String | Write-Output
    # The UE4SS-lane mod folder (WP-2): main.dll + enabled.txt.
    $modMain = Join-Path $f "Mods\Multivoid\dlls\main.dll"
    if (Test-Path $modMain) {
        $mi = Get-Item $modMain
        $en = Test-Path (Join-Path $f "Mods\Multivoid\enabled.txt")
        Write-Output ("Mods\Multivoid\dlls\main.dll size=" + $mi.Length + " mtime=" + $mi.LastWriteTime + " enabled.txt=" + $en)
    } else {
        Write-Output "Mods\Multivoid\dlls\main.dll MISSING"
    }
    Get-ChildItem $f -ErrorAction SilentlyContinue | Where-Object { $_.Name -match '^(multivoid|votv-coop).*\.log$' } | Select-Object Name, Length, LastWriteTime | Format-Table -AutoSize | Out-String | Write-Output
    $sc = Join-Path $f "scenario.txt"
    if (Test-Path $sc) { Write-Output ("scenario.txt = '" + (Get-Content $sc -Raw) + "' (mtime " + (Get-Item $sc).LastWriteTime + ")") }
    $exe = Join-Path $f "VotV-Win64-Shipping.exe"
    if (Test-Path $exe) {
        $fi = Get-Item $exe
        Write-Output ("exe size=" + $fi.Length + " mtime=" + $fi.LastWriteTime)
        try {
            $s = [System.IO.File]::Open($exe, 'Open', 'Read', 'None')
            $s.Close()
            Write-Output "exe is not locked"
        } catch {
            Write-Output ("exe IS LOCKED: " + $_.Exception.Message)
        }
    }
    # Check if any of the dlls are locked (held by a running process)
    # main.dll is the live mod; a leftover xinput1_3.dll / multivoid-*.dll is
    # retired-lane residue (the predecessor guard REFUSES beside it).
    $modDlls = @("Mods\Multivoid\dlls\main.dll", "xinput1_3.dll") + @(Get-ChildItem (Join-Path $f "multivoid-*.dll") -ErrorAction SilentlyContinue | ForEach-Object Name)
    foreach ($dn in $modDlls) {
        $dp = Join-Path $f $dn
        if (Test-Path $dp) {
            try {
                $s = [System.IO.File]::Open($dp, 'Open', 'ReadWrite', 'None')
                $s.Close()
                Write-Output ("$dn writable (not held)")
            } catch {
                Write-Output ("$dn LOCKED: " + $_.Exception.Message)
            }
        }
    }
}
