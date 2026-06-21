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
    [switch]$GrabTest,
    # Phase 5F (2026-05-26): run the autonomous flashlight test on BOTH peers
    # (VOTVCOOP_RUN_FLASHLIGHT_TEST=1). Each peer calls AmainPlayer_C::
    # `Flashlight Update` 4 times via reflection; the POST observer
    # broadcasts each, and the OTHER peer should receive + apply to its
    # puppet. PASS criteria below check for "flashlight: sent" + "flashlight:
    # applied to puppet" log lines on both sides.
    [switch]$FlashlightTest,
    # Phase 5W (2026-05-27): autonomous weather test. Host calls
    # weather_sync::DebugForceRain ON/OFF/ON/OFF via reflection (proper
    # 4-step sequence per RULE 1: enable_rain precondition +
    # setRainProperties + causeRain + setWindParameters). Each toggle
    # broadcasts a WeatherState packet; client's local cycle applies via
    # mutator UFunctions (causeRain + setRainProperties). Verification
    # checks both host's `weather: host broadcast` and client's `weather:
    # applied flags` log lines.
    [switch]$WeatherTest,
    # Phase 5W Inc-fix-2 (2026-05-27): autonomous RED SKY sync test.
    # Host calls DebugForceRedSky(true) -- spawns AredSkyEvent_C on the
    # gamemode + swaps color curves to the red set. Host POST observer
    # broadcasts; client invokes same. Visual signal: entire sky/scene
    # goes RED on both peers. Unambiguous, unlike rain particles.
    [switch]$RedSkyTest,
    # 2026-06-21 (trash-proxy phase 1): autonomous chipPile GRAB + re-pile test
    # (VOTVCOOP_RUN_CHIPPILE_TEST=1). HOST waits 40 s for gameplay + client +
    # pile tracking, then drives a real grab (arm InpActEvt_use + run playerGrabbed)
    # and a Phase B re-pile. CLIENT is scan-only. The verification is in the LOGS
    # (the client's trash PROXY lifecycle): SPAWN -> OnConvert re-skin GRAB(pile->clump)
    # -> re-skin LAND(clump->pile), all IN PLACE on ONE actor (no dup), plus the
    # lerp drive + no leak. Needs a save with chipPiles (else it logs "NO chipPile").
    [switch]$ChipPileTest
)
$ErrorActionPreference = "Stop"
$root  = Split-Path -Parent $PSScriptRoot
# 2026-05-25: the LAN test runs against Game_0.9.0n/ (the user's host
# play copy). Both host + client instances launch from the SAME exe
# with role/peer/port env vars distinguishing them.
#
# We TRIED retargeting to Game_0.9.0n_dev/ (the new Claude-owned dev
# copy with UE4SS) but hit a regression: both instances reach
# "harness: target STORY save 's_may2026'" and then silently die before
# the save-load completes. Cause is unknown -- possibly a first-launch
# asset-cache initialization race when two fresh game instances boot
# simultaneously from a freshly-cloned game folder. The dev copy
# launches a SINGLE instance fine; the failure is specific to two
# concurrent fresh boots.
#
# Workaround: keep lan-test pointed at the well-warmed Game_0.9.0n/
# folder (which has run lan-tests many times this session without
# issue). The dev copy is still valuable for solo RE work (Live View,
# Lua probes, GUIUFunctionCaller hypothesis testing). When the two-
# concurrent-boot issue is solved, switch back. See
# docs/RE_WORKFLOW.md for the 3-copy convention.
$win64 = Join-Path $root "Game_0.9.0n\WindowsNoEditor\VotV\Binaries\Win64"
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
function Launch-Instance($role, $nick, $logName, $extraEnv, $pose, $grabTest, $flashlightTest, $weatherTest, $redSkyTest, $chipPileTest) {
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
    if ($flashlightTest) {
        $env:VOTVCOOP_RUN_FLASHLIGHT_TEST = "1"
    }
    if ($weatherTest) {
        $env:VOTVCOOP_RUN_WEATHER_TEST = "1"
    }
    if ($redSkyTest) {
        $env:VOTVCOOP_RUN_REDSKY_TEST = "1"
    }
    if ($chipPileTest) {
        $env:VOTVCOOP_RUN_CHIPPILE_TEST = "1"
    }
    $p = Start-Process -FilePath $exe `
            -ArgumentList "-windowed","-ResX=$ResX","-ResY=$ResY" -PassThru
    # Clear so the next launch / this shell isn't polluted.
    Remove-Item Env:VOTVCOOP_SCENARIO,Env:VOTVCOOP_NET_ROLE,Env:VOTVCOOP_NET_PORT,`
                Env:VOTVCOOP_NET_NICK,Env:VOTVCOOP_LOG,Env:VOTVCOOP_NET_PEER,`
                Env:VOTVCOOP_AUTOTEST_X,Env:VOTVCOOP_AUTOTEST_Y,Env:VOTVCOOP_AUTOTEST_Z,`
                Env:VOTVCOOP_AUTOTEST_YAW,Env:VOTVCOOP_AUTOTEST_PITCH,Env:VOTVCOOP_RUN_GRAB_TEST,`
                Env:VOTVCOOP_GRAB_TEST_ANCHOR_X,Env:VOTVCOOP_GRAB_TEST_ANCHOR_Y,Env:VOTVCOOP_GRAB_TEST_ANCHOR_Z,`
                Env:VOTVCOOP_RUN_FLASHLIGHT_TEST,Env:VOTVCOOP_RUN_WEATHER_TEST,Env:VOTVCOOP_RUN_REDSKY_TEST,`
                Env:VOTVCOOP_RUN_CHIPPILE_TEST `
                -ErrorAction SilentlyContinue
    return $p
}

Step "launching HOST (nick=Host, binds port $Port, autotest pose @ host$(if($GrabTest){', GRAB TEST armed (full)'})$(if($FlashlightTest){', FLASHLIGHT TEST armed'})$(if($WeatherTest){', WEATHER TEST armed (host drives, client observes)'})$(if($RedSkyTest){', RED SKY TEST armed (host drives)'}))..."
$hostProc = Launch-Instance "host" "Host" $hostLogName "" $hostPose $GrabTest $FlashlightTest $WeatherTest $RedSkyTest $ChipPileTest
# Wait for the HOST to be HOSTING (its first "net stats: state=" heartbeat) before launching
# the client, instead of a fixed 4 s head-start. Under boot contention the host's startup
# (incl. the ~10 s widget reflection dump) can push "hosting" past a minute, and a fixed sleep
# let the client's menu-join time out before the host was accepting -- the 2026-06-21 connect
# race (host reached hosting at 21:40:39, client gave up at 21:40:31). Poll the host log; the
# client still takes ~30-60 s to boot to its own join attempt, by which point the host is ready.
Step "waiting up to 180 s for HOST to start hosting (net stats heartbeat) before launching client..."
$hostReadyDeadline = (Get-Date).AddSeconds(180)
$hostReady = $false
while ((Get-Date) -lt $hostReadyDeadline) {
    if ($hostProc.HasExited) { throw "host instance exited ($($hostProc.ExitCode)) before it started hosting" }
    if ((Test-Path $hostLog) -and (Select-String -Path $hostLog -Pattern "net stats: state=" -SimpleMatch -Quiet)) { $hostReady = $true; break }
    Start-Sleep -Seconds 2
}
if ($hostReady) { Step "host is hosting -- launching client." }
else { Step "WARN: host hosting heartbeat not seen in 180 s; launching client anyway (connect may still race)." }
Step "launching CLIENT (nick=Client, peer=127.0.0.1:$Port, autotest pose @ client$(if($GrabTest){', SCAN ONLY'})$(if($FlashlightTest){', FLASHLIGHT TEST armed'})$(if($WeatherTest){', WEATHER TEST armed (observer-only)'})$(if($RedSkyTest){', RED SKY TEST armed (observer-only)'}))..."
# Client also gets VOTVCOOP_RUN_GRAB_TEST=1 -- it runs the prop scan + logs
# fields for cross-peer Key comparison, but does NOT drive any UFunctions.
# Flashlight test: BOTH peers drive toggles so we verify the wire goes
# both directions (host->client puppet AND client->host puppet).
# Weather test: BOTH peers get the env var so each spawns the routine, but
# the routine self-gates on role -- only the host actually forces rain;
# the client logs "not host" and exits. Setting the env on both keeps
# launcher symmetry simple.
$clientProc = Launch-Instance "client" "Client" $clientLogName "127.0.0.1" $clientPose $GrabTest $FlashlightTest $WeatherTest $RedSkyTest $ChipPileTest
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
# Client-side readiness, scenario-robust (2026-06-21 fix). The net-stats "puppet" counter is
# HOST-CENTRIC -- it reports net_pump::Puppet(1).valid() (slot 1), but on a CLIENT the host's
# puppet is at slot 0 (net_pump.cpp:60 "slot 0 = host ... on CLIENT processes"), so that counter
# is structurally ALWAYS 0 on a client and Side-Ok's puppet==1 can never hold client-side. The
# join message also differs by flow (a menu-mode save-transfer join logs "Joined <host>'s game",
# not "Host joined the game"). Verify the client is genuinely in the shared world by the robust
# signals: CONNECTED(client) + the host's puppet actually SPAWNED + inbound traffic.
function Client-Ok($log) {
    if (-not (Log-Has $log "CONNECTED (client")) { return $false }
    if (-not (Log-Has $log "RemotePlayer::Spawn: puppet=")) { return $false }
    $s = Net-Stats $log
    return ($s -ne $null) -and ($s.recv -gt 0)
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
    # Host join marker is "slot N joined the game" (player_handshake announces by slot, not nick),
    # so match the scenario-robust substring "joined the game" rather than "Client joined the game".
    $hostOk   = Side-Ok $hostLog   "CONNECTED (host"   "joined the game"
    $clientOk = Client-Ok $clientLog
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
    if ($RedSkyTest) {
        # Red sky test timing (matches RunAutonomousRedSkyTest):
        #   PASS + ~20 s    routine starts (stabilization wait)
        #   PASS + ~20 s    phase ON  (DebugForceRedSky(true) -- spawnRedSky)
        #   PASS + ~30 s    phase OFF (DebugForceRedSky(false) -- set false)
        #   PASS + ~36 s    routine done
        # Capture mid-ON (peak red), then post-OFF (reverted).
        Step "PASS detected; RED SKY TEST armed -- host fires spawnRedSky..."
        Start-Sleep -Seconds 25         # PASS+25, midway through 10 s ON dwell
        Capture-Pair "redsky-on"
        Start-Sleep -Seconds 10         # PASS+35, midway through 6 s OFF dwell
        Capture-Pair "redsky-off"
    } elseif ($WeatherTest) {
        # Weather test timing (matches RunAutonomousWeatherTest's
        # 4-phase routine, each phase 6s):
        #   PASS + ~20 s    routine starts (host stabilization wait)
        #   PASS + ~20-26 s    phase ON-1 (DebugForceRain true)
        #   PASS + ~26-32 s    phase OFF-1
        #   PASS + ~32-38 s    phase ON-2
        #   PASS + ~38-44 s    phase OFF-2 (final)
        # Take screenshots 4s into each phase (midway through the dwell).
        Step "PASS detected; WEATHER TEST armed -- forcing rain ON/OFF cycles on host..."
        Start-Sleep -Seconds 24          # land at PASS+24, midway through ON-1
        Capture-Pair "weather-rain-on1"
        Start-Sleep -Seconds 6           # PASS+30, midway through OFF-1
        Capture-Pair "weather-rain-off1"
        Start-Sleep -Seconds 6           # PASS+36, midway through ON-2 (peak rain)
        Capture-Pair "weather-rain-on2"
        Start-Sleep -Seconds 8           # PASS+44, end of OFF-2
        Capture-Pair "weather-end"
    } elseif ($FlashlightTest) {
        # Flashlight test timing (matches RunAutonomousFlashlightTest's
        # 5-iteration routine; each peer ends with flashlight ON):
        #   PASS + ~15 s   routine starts (post-stabilization)
        #   PASS + ~17 s   iter 0 ON
        #   PASS + ~19 s   iter 1 OFF
        #   PASS + ~21 s   iter 2 ON
        #   PASS + ~23 s   iter 3 OFF
        #   PASS + ~25 s   iter 4 ON  (final state)
        #   PASS + ~27 s   routine done
        # Peers can start staggered by up to ~5s (different boot times),
        # so we wait until BOTH have completed their last iter + the
        # wire round-trip lands. PASS+35s covers a 5s host-vs-client
        # stagger + 2s of buffer past the late peer's iter 4.
        Step "PASS detected; FLASHLIGHT TEST armed -- waiting for both peers' final ON toggle to land..."
        Start-Sleep -Seconds 22
        Capture-Pair "flashlight-mid"
        Start-Sleep -Seconds 13
        Capture-Pair "flashlight-end"
    } elseif ($GrabTest) {
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
    } elseif ($ChipPileTest) {
        # chipPile test timing (RunAutonomousChipPileTest): the HOST thread now GATES the grab on
        # the client puppet going live (in-world) + a 35 s snapshot-expression margin -- killing the
        # fixed-40s join race that made the client express its proxies AFTER the grab (smoke
        # 2026-06-21). Then: grab -> 1.6 s field probe -> 8 s SUSTAINED MOVING carry -> Phase B
        # re-pile -> tail. From PASS the puppet is live almost immediately, so the grab lands ~PASS+37s.
        # Capture mid-carry + post-re-pile (render-blind; the logs are the real verdict -- the client's
        # PROXY lifecycle + the GRAB-IN/drive-advance that prove the carry mirrors).
        Step "PASS detected; CHIPPILE TEST armed -- host waits for client puppet + 35 s margin, then grabs + carries 8 s + re-piles..."
        Start-Sleep -Seconds 48          # ~mid moving-carry (puppet-live ~immediate + 35 s margin + grab + probe)
        Capture-Pair "chippile-carry"
        Start-Sleep -Seconds 22          # ~after Phase B re-pile + the ToPile convert
        Capture-Pair "chippile-repile"
        Start-Sleep -Seconds 30          # >=30 s steady-state tail (let the last convert + drive settle)
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

# Phase 5F: separate verdict for the flashlight test (the connect-PASS above
# only proves the SESSION came up). We need the toggles to actually traverse
# the wire and apply on the puppet.
function Count-Matches($path, $pattern) {
    if (-not (Test-Path $path)) { return 0 }
    return @(Select-String -Path $path -Pattern $pattern -ErrorAction SilentlyContinue).Count
}

# Phase 5W Inc-fix-2: separate verdict for the red sky test.
if ($pass -and $RedSkyTest) {
    Write-Host ""
    Step "==== RED SKY TEST verdict ===="
    $hostForce     = Count-Matches $hostLog   "weather: DebugForceRedSky"
    $hostBroadcast = Count-Matches $hostLog   "weather: host broadcast RedSky"
    $clientApplied = Count-Matches $clientLog "weather: ApplyRedSky"
    Step "host   log: DebugForceRedSky=$hostForce  host-broadcast=$hostBroadcast"
    Step "client log: ApplyRedSky=$clientApplied"
    $rspass = ($hostForce -ge 1) -and ($hostBroadcast -ge 1) -and ($clientApplied -ge 1)
    if ($rspass) {
        Write-Host "[lan-test] RED SKY RESULT: PASS -- host fired $hostForce times; broadcast $hostBroadcast; client applied $clientApplied." -ForegroundColor Green
    } else {
        Write-Host "[lan-test] RED SKY RESULT: FAIL -- low counts." -ForegroundColor Red
        Step "==== HOST red-sky lines ===="
        Select-String -Path $hostLog -Pattern "weather:|RedSky|redSky" -ErrorAction SilentlyContinue |
            Select-Object -Last 20 | ForEach-Object { Write-Host "  $($_.Line)" }
        Step "==== CLIENT red-sky lines ===="
        Select-String -Path $clientLog -Pattern "weather:|RedSky|redSky" -ErrorAction SilentlyContinue |
            Select-Object -Last 20 | ForEach-Object { Write-Host "  $($_.Line)" }
    }
}

# Phase 5W: separate verdict for the weather test. PASS means host's
# DebugForceRain calls broadcast over the wire AND the client applied them.
if ($pass -and $WeatherTest) {
    Write-Host ""
    Step "==== WEATHER TEST verdict ===="
    # Host log expected lines:
    #   weather: DebugForceRain ... (one per phase = 4)
    #   weather: host broadcast flags=0x... (one per phase = 4)
    # Client log expected:
    #   weather: applied flags 0x... (one per phase = 4)
    $hostForce     = Count-Matches $hostLog   "weather: DebugForceRain"
    $hostBroadcast = Count-Matches $hostLog   "weather: host broadcast flags"
    $clientApplied = Count-Matches $clientLog "weather: applied flags"
    Step "host   log: DebugForceRain=$hostForce  host-broadcast=$hostBroadcast"
    Step "client log: applied=$clientApplied"
    # Expected: 4 phases (ON-1, OFF-1, ON-2, OFF-2). Allow for dedup
    # squashing of the ON-2-after-ON-1 sequence (no state change between
    # like-state phases triggers signature-match dedup); accept >=2 each.
    $wxpass = ($hostForce -ge 2) -and ($hostBroadcast -ge 2) -and ($clientApplied -ge 2)
    if ($wxpass) {
        Write-Host "[lan-test] WEATHER RESULT: PASS -- host forced rain $hostForce times; broadcast $hostBroadcast; client applied $clientApplied." -ForegroundColor Green
    } else {
        Write-Host "[lan-test] WEATHER RESULT: FAIL -- low counts (expected >=2 each)." -ForegroundColor Red
        Step "==== HOST weather lines ===="
        Select-String -Path $hostLog -Pattern "weather" -ErrorAction SilentlyContinue |
            Select-Object -Last 20 | ForEach-Object { Write-Host "  $($_.Line)" }
        Step "==== CLIENT weather lines ===="
        Select-String -Path $clientLog -Pattern "weather" -ErrorAction SilentlyContinue |
            Select-Object -Last 20 | ForEach-Object { Write-Host "  $($_.Line)" }
    }
}

if ($pass -and $FlashlightTest) {
    Write-Host ""
    Step "==== FLASHLIGHT TEST verdict ===="
    # Match either the observer send ("flashlight: sent state=") or the
    # autotest send ("flashlight: DebugForceToggle sent state="). Both
    # end with "sent state=", so a substring match catches both.
    $hostSent     = Count-Matches $hostLog   "sent state="
    $hostApplied  = Count-Matches $hostLog   "flashlight: applied to puppet="
    $clientSent   = Count-Matches $clientLog "sent state="
    $clientApplied= Count-Matches $clientLog "flashlight: applied to puppet="
    Step "host   log: sent=$hostSent   applied=$hostApplied"
    Step "client log: sent=$clientSent applied=$clientApplied"
    # Expected: each peer sends ~4 (one per toggle iteration; dedup may
    # reduce). Each peer also applies ~4 (the OTHER peer's toggles).
    $flpass = ($hostSent -ge 2) -and ($clientSent -ge 2) -and `
              ($hostApplied -ge 2) -and ($clientApplied -ge 2)
    if ($flpass) {
        Write-Host "[lan-test] FLASHLIGHT RESULT: PASS -- both peers sent + applied across the wire." -ForegroundColor Green
    } else {
        Write-Host "[lan-test] FLASHLIGHT RESULT: FAIL -- low send/apply counts (expected >=2 each)." -ForegroundColor Red
        # Dump any flashlight-related log lines to help diagnose.
        Step "==== HOST flashlight lines ===="
        Select-String -Path $hostLog -Pattern "flashlight" -ErrorAction SilentlyContinue |
            Select-Object -Last 12 | ForEach-Object { Write-Host "  $($_.Line)" }
        Step "==== CLIENT flashlight lines ===="
        Select-String -Path $clientLog -Pattern "flashlight" -ErrorAction SilentlyContinue |
            Select-Object -Last 12 | ForEach-Object { Write-Host "  $($_.Line)" }
    }
}

# 2026-06-21 trash-proxy phase 1: chipPile verdict. The proof is the CLIENT's PROXY
# lifecycle -- a single AStaticMeshActor re-skinned in place (no dup), driven + frozen
# correctly. Counts the markers and flags the dup/leak/NOT-FOUND failure signatures.
if ($pass -and $ChipPileTest) {
    Write-Host ""
    Step "==== CHIPPILE (trash proxy) verdict ===="
    $hostGrab       = Count-Matches $hostLog   "chippile_test: .*GRAB"
    $hostConvert    = Count-Matches $hostLog   "HOST GRAB ADOPT|HOST RE-PILE|HOST (GRAB|LAND)"
    $cliSpawn       = Count-Matches $clientLog "trash_proxy: SPAWN"
    $cliReskin      = Count-Matches $clientLog "PROXY re-skinned IN PLACE"
    $cliSpawnOnConv = Count-Matches $clientLog "proxy SPAWNED .* convert beat its spawn"
    # The CARRY proof (2026-06-21): re-skin alone only proves the convert landed -- it does NOT prove
    # the carry MIRRORS. GRAB-IN = the carry pose-drive established; drive #N [proxy] = it advanced
    # across the 8 s moving carry; HOLD = poses were stuck (the convert was not adopted -> known=0).
    $cliGrabIn      = Count-Matches $clientLog "GRAB-IN"
    $cliDriveAdv    = Count-Matches $clientLog "drive #.*\[proxy\]"
    $cliHold        = Count-Matches $clientLog "CLIENT HOLD carry pose"
    $cliClumpFall   = Count-Matches $clientLog "PILE-FALLBACK"
    # Failure signatures -- ANY of these is a regression.
    $cliNotFound    = Count-Matches $clientLog "mirror NOT-FOUND|no local mirror of E existed"
    $cliErr         = Count-Matches $clientLog "trash_proxy: .*FAILED|proxy spawn-on-convert FAILED|SkinProxy.*unresolved|SetComponentMaterial unresolved"
    Step "host   log: grab=$hostGrab  hostConvert=$hostConvert"
    Step "client log: proxySPAWN=$cliSpawn  reskinINPLACE=$cliReskin  spawn-on-convert=$cliSpawnOnConv"
    Step "client log: CARRY -> GRAB-IN=$cliGrabIn  driveAdvance=$cliDriveAdv  HOLD=$cliHold  clumpMeshFellBackToPile=$cliClumpFall"
    Step "client log: FAIL-sigs -> NOT-FOUND=$cliNotFound  errors=$cliErr"
    # PASS now requires the CARRY to actually mirror, not just the convert to land: a re-skin AND the
    # pose-drive established (GRAB-IN) AND it advanced (drive [proxy]). ZERO NOT-FOUND (dup-staleness)
    # and ZERO proxy errors. (HOLD>0 with GRAB-IN==0 is the carry-never-established bug; clumpMeshFell
    # BackToPile>0 is the cosmetic clump-looks-like-pile bug -- both surface in the lines below.)
    $cppass = ($cliSpawn -ge 1) -and (($cliReskin -ge 1) -or ($cliSpawnOnConv -ge 1)) -and `
              ($cliGrabIn -ge 1) -and ($cliDriveAdv -ge 1) -and ($cliNotFound -eq 0) -and ($cliErr -eq 0)
    if ($cppass) {
        Write-Host "[lan-test] CHIPPILE RESULT: PASS -- client proxy re-skinned + the carry pose-drive established AND advanced, no dup/NOT-FOUND/error." -ForegroundColor Green
    } else {
        Write-Host "[lan-test] CHIPPILE RESULT: FAIL (or no pile in save) -- see the PILE lines below." -ForegroundColor Red
    }
    # Always print the pile lines (PASS or FAIL) -- this is a diagnostic smoke; the timeline is the verdict.
    Step "==== HOST chippile/pile lines ===="
    Select-String -Path $hostLog -Pattern "chippile_test|\[PILE\]|trash_proxy|NO chipPile|PropPose emit" -ErrorAction SilentlyContinue |
        Select-Object -Last 50 | ForEach-Object { Write-Host "  $($_.Line)" }
    Step "==== CLIENT chippile/pile lines ===="
    Select-String -Path $clientLog -Pattern "\[PILE\]|trash_proxy|OnConvert|GRAB-IN|drive #.*proxy|mirror (FOUND|NOT-FOUND)" -ErrorAction SilentlyContinue |
        Select-Object -Last 70 | ForEach-Object { Write-Host "  $($_.Line)" }
}

# --- teardown ---------------------------------------------------------------
if (-not $Keep) {
    Step "stopping both instances..."
    Stop-Process -Id $hostProc.Id, $clientProc.Id -Force -ErrorAction SilentlyContinue
} else {
    Step "leaving instances running (-Keep). host pid=$($hostProc.Id) client pid=$($clientProc.Id)"
}
if (-not $pass) { exit 1 }
