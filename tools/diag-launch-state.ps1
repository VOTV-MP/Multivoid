$root = "d:\Projects\Programming\VOTV_MP"
$folders = @(
    "$root\Game_0.9.0n_HOST\WindowsNoEditor\VotV\Binaries\Win64",
    "$root\Game_0.9.0n_CLIENT_1\WindowsNoEditor\VotV\Binaries\Win64",
    "$root\Game_0.9.0n_CLIENT_3\WindowsNoEditor\VotV\Binaries\Win64"
)
foreach ($f in $folders) {
    if (-not (Test-Path $f)) { Write-Output "MISSING: $f"; continue }
    Write-Output "=== $f ==="
    Get-ChildItem $f -Filter "*.dll" | Where-Object { $_.Name -match 'xinput|votv-coop|UE4SS|dwmapi' } | Select-Object Name, Length, LastWriteTime | Format-Table -AutoSize | Out-String | Write-Output
    Get-ChildItem $f -Filter "votv-coop*.log" -ErrorAction SilentlyContinue | Select-Object Name, Length, LastWriteTime | Format-Table -AutoSize | Out-String | Write-Output
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
    foreach ($dn in @("xinput1_3.dll", "votv-coop.dll")) {
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
