# capture-kerfur-repro.ps1 -- snapshot BOTH peers' live multivoid.log the moment a kerfur repro
# symptom is on screen, BEFORE any relaunch clobbers them (multivoid.log truncates on launch).
#
# Usage (run at the symptom moment, games can still be running):
#   pwsh -File tools/capture-kerfur-repro.ps1
#   pwsh -File tools/capture-kerfur-repro.ps1 -Tag reverse_turnoff   # optional label
#
# Copies host (Game_0.9.0n_HOST) + client (Game_0.9.0n_CLIENT_1) logs into
#   research/kerfur_repro_<yyyymmdd_HHmmss><_tag>/{host,client}.log
# and prints the turn_off / KerfurConvert lines it found so you see immediately whether
# the conversion was logged (the diagnosis hinges on that).

param([string]$Tag = "")

$root = Split-Path -Parent $PSScriptRoot
$hostLog   = Join-Path $root "Game_0.9.0n_HOST\WindowsNoEditor\VotV\Binaries\Win64\multivoid.log"
$clientLog = Join-Path $root "Game_0.9.0n_CLIENT_1\WindowsNoEditor\VotV\Binaries\Win64\multivoid.log"

$stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$name  = if ($Tag) { "kerfur_repro_${stamp}_$Tag" } else { "kerfur_repro_$stamp" }
$outDir = Join-Path $root "research\$name"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

function Copy-Live([string]$src, [string]$dstName) {
    if (-not (Test-Path $src)) { Write-Host "  MISSING: $src" -ForegroundColor Red; return $null }
    $dst = Join-Path $outDir $dstName
    # FileShare ReadWrite so we can copy while the game still holds the handle open.
    $fs = [System.IO.File]::Open($src, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
    try {
        $out = [System.IO.File]::Create($dst)
        try { $fs.CopyTo($out) } finally { $out.Close() }
    } finally { $fs.Close() }
    return $dst
}

Write-Host "Capturing kerfur repro logs -> $outDir"
$h = Copy-Live $hostLog   "host.log"
$c = Copy-Live $clientLog "client.log"

# Surface the decisive lines immediately (the turn_off conversion signature + retire/materialize).
$pat = "POLL turn_off|POLL turn-on|BindFormActor|KerfurConvert|applied KerfurConvert|npc-adopt: bound LOCAL save NPC|SpawnFreshNpcMirror|follow|idle|mode"
foreach ($pair in @(@("HOST", $h), @("CLIENT", $c))) {
    $label = $pair[0]; $file = $pair[1]
    if ($file) {
        Write-Host "`n==== $label conversion/mode lines ($file) ====" -ForegroundColor Cyan
        $hits = Select-String -Path $file -Pattern $pat -CaseSensitive:$false
        if ($hits) { $hits | ForEach-Object { $_.Line } } else { Write-Host "  (no conversion/mode lines matched)" -ForegroundColor Yellow }
    }
}
Write-Host "`nDone. Hand these two files to Claude for the R-host / R-client-mode / R-client-retire split." -ForegroundColor Green
