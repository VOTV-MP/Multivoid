<#
.SYNOPSIS
    Two-instance LAN coop test framework (RULE 1: a real cross-process test, not the
    single-process loopback). Launches TWO VotV instances -- one host, one client --
    each in its own net role, and verifies they discover each other and exchange
    state over real UDP (separate sockets, real handshake, no self-send).

.DESCRIPTION
    Both instances load the SAME mod DLL from the SAME game dir, so they cannot be
    configured by per-file scenario.txt / votv-coop.ini. Instead each is given its
    role/peer/port/nickname AND its own log file via ENVIRONMENT VARIABLES the harness
    reads (VOTVCOOP_SCENARIO / _NET_ROLE / _NET_PEER / _NET_PORT / _NET_NICK / _LOG).

    PASS criteria (parsed from the two logs -- the deterministic proof):
      host   log: "CONNECTED (host)"   + "Client joined the game" + net stats recv>0, puppet=1
      client log: "CONNECTED (client)" + "Host joined the game"   + net stats recv>0, puppet=1
    The DISTINCT nicknames crossing over (host sees "Client", client sees "Host") prove
    real cross-process exchange -- impossible in the self-loopback.

    Screenshots of both windows are captured best-effort as artifacts (sequential
    foreground BitBlt; 3D frames are black under PrintWindow).

.PARAMETER Port        UDP port the host binds / the client targets (default 47621).
.PARAMETER TimeoutSec  Max wait for both to connect + exchange (default 240; two
                       instances boot + load story under CPU/GPU contention).
.PARAMETER NoBuild     Skip the cmake build + deploy (use the already-deployed mod).
.PARAMETER Keep        Leave both instances running after the verdict (for inspection).
.PARAMETER ResX/-ResY  Per-window resolution (default 1280x720 so two fit on screen).
#>
[CmdletBinding()]
param(
    [int]$Port = 47621,
    [int]$TimeoutSec = 240,
    [switch]$NoBuild,
    [switch]$Keep,
    [int]$ResX = 1280,
    [int]$ResY = 720,
    # Run the autonomous host-side grab test (VOTVCOOP_RUN_GRAB_TEST=1) -- finds
    # nearest physics prop, drives a forced grab via the engine PhysicsHandle
    # UFunctions to verify the Stage-1 observer pipeline end-to-end without an
    # E-press. Adds ~25 s to the test (10 s settle + 5 s drive + 10 s tail).
    [switch]$GrabTest
)
$ErrorActionPreference = "Stop"
$root  = Split-Path -Parent $PSScriptRoot
# 2026-05-25: the LAN test now runs against Game_0.9.0n_dev/ (the Claude-
# owned dev copy with UE4SS installed) -- ISOLATED from the user's
# hands-on play copies (Game_0.9.0n/ = host, Game_0.9.0n_copy/ = client).
# Both host + client instances in the autonomous test launch from the
# SAME _dev/ exe with role/peer/port env vars distinguishing them. This
# keeps the test's logs, screenshots, and any save-state mutations OUT
# of the user's play directories. See docs/RE_WORKFLOW.md.
$win64 = Join-Path $root "Game_0.9.0n_dev\WindowsNoEditor\VotV\Binaries\Win64"
$exe   = Join-Path $win64 "VotV-Win64-Shipping.exe"
$hostLogName   = "votv-coop-host.log"
$clientLogName = "votv-coop-client.log"
$hostLog   = Join-Path $win64 $hostLogName
$clientLog = Join-Path $win64 $clientLogName

function Step($m) { Write-Host "[lan-test] $m" -ForegroundColor Cyan }

# --- build + deploy ---------------------------------------------------------
if (-not $NoBuild) {
    Step "building mod (Release)..."
    cmake --build (Join-Path $root "build\votv-coop") --config Release | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
}
Step "deploying standalone loader (UE4SS disabled)..."
& (Join-Path $PSScriptRoot "deploy-loader.ps1") -Standalone | Out-Null

# Fresh logs so we only parse THIS run.
Remove-Item $hostLog, $clientLog -ErrorAction SilentlyContinue

# Verified autotest spawn poses (per [[project-autotest-spawn-pose]] memo,
# user-confirmed 2026-05-23): host + client face each other ~14 m apart along Y
# at ~64 m elevation, so each instance's screenshot sees the OTHER's puppet in
# full frame. Replaces the prior OMEGA-interior-stuck screenshots that showed
# nothing. The harness reads VOTVCOOP_AUTOTEST_X/Y/Z/YAW/PITCH and teleports
# the local pawn + sets controller rotation post-load (env unset = no
# teleport, prod default).
$hostPose   = @{ X = "-37730"; Y = "69943";  Z = "6420"; Yaw = "-86.8"; Pitch = "-5.9" }
$clientPose = @{ X = "-37640"; Y = "68532";  Z = "6399"; Yaw = "88.2";  Pitch = "-4.7" }

# --- launch the two instances with per-process env --------------------------
function Launch-Instance($role, $nick, $logName, $extraEnv, $pose, $grabTest) {
    $env:VOTVCOOP_SCENARIO = "play"
    $env:VOTVCOOP_NET_ROLE = $role
    $env:VOTVCOOP_NET_PORT = "$Port"
    $env:VOTVCOOP_NET_NICK = $nick
    $env:VOTVCOOP_LOG      = $logName
    $env:VOTVCOOP_NET_PEER = $extraEnv
    $env:VOTVCOOP_AUTOTEST_X     = $pose.X
    $env:VOTVCOOP_AUTOTEST_Y     = $pose.Y
    $env:VOTVCOOP_AUTOTEST_Z     = $pose.Z
    $env:VOTVCOOP_AUTOTEST_YAW   = $pose.Yaw
    $env:VOTVCOOP_AUTOTEST_PITCH = $pose.Pitch
    if ($grabTest) {
        $env:VOTVCOOP_RUN_GRAB_TEST = "1"
        # Cross-peer scan anchor: both peers scan around the HOST's autotest
        # pose so they find the SAME nearest prop and we can directly verify
        # Key string match (UUID stable cross-peer; ComparisonIndex per-process).
        $env:VOTVCOOP_GRAB_TEST_ANCHOR_X = $hostPose.X
        $env:VOTVCOOP_GRAB_TEST_ANCHOR_Y = $hostPose.Y
        $env:VOTVCOOP_GRAB_TEST_ANCHOR_Z = $hostPose.Z
    }
    $p = Start-Process -FilePath $exe `
            -ArgumentList "-windowed","-ResX=$ResX","-ResY=$ResY" -PassThru
    # Clear so the next launch / this shell isn't polluted.
    Remove-Item Env:VOTVCOOP_SCENARIO,Env:VOTVCOOP_NET_ROLE,Env:VOTVCOOP_NET_PORT,`
                Env:VOTVCOOP_NET_NICK,Env:VOTVCOOP_LOG,Env:VOTVCOOP_NET_PEER,`
                Env:VOTVCOOP_AUTOTEST_X,Env:VOTVCOOP_AUTOTEST_Y,Env:VOTVCOOP_AUTOTEST_Z,`
                Env:VOTVCOOP_AUTOTEST_YAW,Env:VOTVCOOP_AUTOTEST_PITCH,Env:VOTVCOOP_RUN_GRAB_TEST,`
                Env:VOTVCOOP_GRAB_TEST_ANCHOR_X,Env:VOTVCOOP_GRAB_TEST_ANCHOR_Y,Env:VOTVCOOP_GRAB_TEST_ANCHOR_Z `
                -ErrorAction SilentlyContinue
    return $p
}

Step "launching HOST (nick=Host, binds port $Port, autotest pose @ host$(if($GrabTest){', GRAB TEST armed (full)'}))..."
$hostProc = Launch-Instance "host" "Host" $hostLogName "" $hostPose $GrabTest
Start-Sleep -Seconds 4   # let the host process come up first
Step "launching CLIENT (nick=Client, peer=127.0.0.1:$Port, autotest pose @ client$(if($GrabTest){', SCAN ONLY'}))..."
# Client also gets VOTVCOOP_RUN_GRAB_TEST=1 -- it runs the prop scan + logs
# fields for cross-peer Key comparison, but does NOT drive any UFunctions.
$clientProc = Launch-Instance "client" "Client" $clientLogName "127.0.0.1" $clientPose $GrabTest
Step "host pid=$($hostProc.Id)  client pid=$($clientProc.Id)"

# --- verification helpers ---------------------------------------------------
function Log-Has($path, $pattern) {
    return (Test-Path $path) -and `
           [bool](Select-String -Path $path -Pattern $pattern -SimpleMatch -ErrorAction SilentlyContinue)
}
# Last "net stats:" line -> @{recv; puppet} (or $null).
function Net-Stats($path) {
    if (-not (Test-Path $path)) { return $null }
    $line = Select-String -Path $path -Pattern "net stats: " -ErrorAction SilentlyContinue |
            Select-Object -Last 1
    if (-not $line) { return $null }
    if ($line.Line -match "recv=(\d+).*puppet=(\d)") {
        return @{ recv = [int]$Matches[1]; puppet = [int]$Matches[2] }
    }
    return $null
}
function Side-Ok($log, $connStr, $joinStr) {
    if (-not (Log-Has $log $connStr)) { return $false }
    if (-not (Log-Has $log $joinStr)) { return $false }
    $s = Net-Stats $log
    return ($s -ne $null) -and ($s.recv -gt 0) -and ($s.puppet -eq 1)
}

# --- poll until both sides pass or timeout ----------------------------------
Step "waiting up to ${TimeoutSec}s for cross-process connect + state exchange..."
$deadline = (Get-Date).AddSeconds($TimeoutSec)
$pass = $false
while ((Get-Date) -lt $deadline) {
    if (($hostProc.HasExited) -or ($clientProc.HasExited)) {
        Step "an instance exited early (host exited=$($hostProc.HasExited) client exited=$($clientProc.HasExited))"
        break
    }
    # Prefix match: the connect line may be "CONNECTED (host)" or
    # "CONNECTED (host, via token'd msg)" depending on which path confirmed it.
    $hostOk   = Side-Ok $hostLog   "CONNECTED (host"   "Client joined the game"
    $clientOk = Side-Ok $clientLog "CONNECTED (client" "Host joined the game"
    if ($hostOk -and $clientOk) { $pass = $true; break }
    Start-Sleep -Seconds 3
}

# PASS only means "connected + first pose seen + puppet spawned". The puppet's
# first network pose SNAPS its position from the spawn placeholder (2.5 m in
# front of the local) to the source's actual world position. Give the stream
# 6 s to settle so screenshots show puppets at their TRUE pose locations,
# not at the placeholder. (Without this delay the screenshots can catch the
# puppet mid-snap or before the snap, making it look like the autotest poses
# don't apply -- they DO, but the screenshot timing is what was wrong.)
$shotDir = Join-Path $win64 "coop-screenshots"
New-Item -ItemType Directory -Force -Path $shotDir | Out-Null
$cap = Join-Path $PSScriptRoot "capture-window.ps1"

# Helper: capture both processes' windows with a label suffix.
function Capture-Pair($labelSuffix) {
    foreach ($pair in @(@($hostProc,"host"), @($clientProc,"client"))) {
        if ($pair[0].HasExited) { continue }
        $out = Join-Path $shotDir ("lan-" + $pair[1] + "-" + $labelSuffix + ".png")
        try { & powershell -NoProfile -ExecutionPolicy Bypass -File $cap -ProcessId $pair[0].Id -OutPath $out | Out-Null } catch {}
        if (Test-Path $out) { Step ("screenshot[" + $pair[1] + "] " + $labelSuffix + " -> " + $out) }
    }
}

if ($pass) {
    if ($GrabTest) {
        # Grab test timing (matches RunAutonomousGrabTest()):
        #   PASS + ~10 s    routine starts (post-stabilization)
        #   PASS + ~12 s    teleport + Grab fires; 30-tick hold loop begins
        #   PASS + ~15 s    host BEFORE-TILT screenshot (host tick 6)
        #   PASS + ~16 s    tilt phase begins (ticks 8-19)
        #   PASS + ~25 s    host AFTER-TILT screenshot (host tick 26)
        #   PASS + ~27 s    release; heavy + Timeline tail
        #   PASS + ~32 s    routine done
        Step "PASS detected; GRAB TEST armed -- capturing CROSS-PEER views during host's hold..."
        Start-Sleep -Seconds 15
        Capture-Pair "before-tilt"
        Start-Sleep -Seconds 10
        Capture-Pair "after-tilt"
        # Wait ~3 s for the host's grab routine to reach the post-throw
        # window (release+throw happens ~2 s after tick 26 / after-tilt
        # screenshot, then 700 ms of mid-flight). Capture mid-flight on
        # BOTH peers to prove the v4 wire propagates the throw impulse.
        Start-Sleep -Seconds 3
        Capture-Pair "post-throw"
        # Let the rest of the routine complete (heavy arm + Timeline tail).
        Start-Sleep -Seconds 6
    } else {
        Step "PASS detected; waiting 6 s for pose stream to settle before screenshot..."
        Start-Sleep -Seconds 6
    }
}

# Final capture (existing names so other tooling/docs that reference these still work).
foreach ($pair in @(@($hostProc,"host"), @($clientProc,"client"))) {
    if ($pair[0].HasExited) { continue }
    $out = Join-Path $shotDir ("lan-" + $pair[1] + ".png")
    try { & powershell -NoProfile -ExecutionPolicy Bypass -File $cap -ProcessId $pair[0].Id -OutPath $out | Out-Null } catch {}
    if (Test-Path $out) { Step ("screenshot[" + $pair[1] + "] -> " + $out) }
}

# --- report -----------------------------------------------------------------
Write-Host ""
Step "==== HOST log (key lines) ===="
if (Test-Path $hostLog) {
    Get-Content $hostLog | Where-Object { $_ -match "CONNECTED|joined the game|net stats:|locked peer|left the game|ERROR" } |
        Select-Object -Last 8 | ForEach-Object { Write-Host "  $_" }
}
Step "==== CLIENT log (key lines) ===="
if (Test-Path $clientLog) {
    Get-Content $clientLog | Where-Object { $_ -match "CONNECTED|joined the game|net stats:|left the game|ERROR" } |
        Select-Object -Last 8 | ForEach-Object { Write-Host "  $_" }
}
Write-Host ""
if ($pass) {
    Write-Host "[lan-test] RESULT: PASS -- two instances connected over UDP and exchanged state cross-process." -ForegroundColor Green
} else {
    Write-Host "[lan-test] RESULT: FAIL -- see the log lines above (timeout=${TimeoutSec}s)." -ForegroundColor Red
}

# --- teardown ---------------------------------------------------------------
if (-not $Keep) {
    Step "stopping both instances..."
    Stop-Process -Id $hostProc.Id, $clientProc.Id -Force -ErrorAction SilentlyContinue
} else {
    Step "leaving instances running (-Keep). host pid=$($hostProc.Id) client pid=$($clientProc.Id)"
}
if (-not $pass) { exit 1 }
