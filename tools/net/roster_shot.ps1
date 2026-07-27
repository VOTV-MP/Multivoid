# tools/net/roster_shot.ps1 -- does a CLIENT's TAB actually RENDER the whole
# roster, with IDs?
#
# WHY THIS EXISTS SEPARATELY FROM THE OTHER ARC-A DRILLS. They all grade the
# LEDGER, from log lines. The reported feature is a SCREEN: "TAB must list
# everyone and show IDs". A client listing only itself and the host was the
# pre-existing bug arc A fixed, so the client is the peer that must be
# photographed -- the host's list was never broken and proves nothing here.
#
# mp.py's own `scoreshot` is 2-peer and captures the HOST, which is exactly the
# case that always worked. This runs the 4-peer smoke and captures every peer, so
# the four shots can be compared side by side.
#
# IT PRESSES THE REAL KEY. There is a VOTVCOOP_SCOREBOARD_OPEN=1 override whose
# comment says the smoke "can't" press it -- that was never true, it was a
# shortcut. The overlay owns a WndProc hook, so PostMessage-ing WM_KEYDOWN /
# WM_KEYUP for VK_OEM_3 to each peer's window drives the SAME path a player does,
# per window, with no focus stealing across four instances. Using the real key
# also tests the keybind itself -- and the first thing it established is that the
# key is TILDE (VK_OEM_3, the physical key above TAB), not TAB, which the hands-on
# runbook had wrong.
#
# Note the asymmetry the overlay documents: the scoreboard is HOLD-to-peek on a
# client and TOGGLE on the host. So the client shots are taken while the key is
# held down, and WM_KILLFOCUS clears the flag -- another reason to PostMessage
# rather than SendInput.
#
# It answers ONE question. Whether the list READS well -- column widths,
# ordering, what "VIA HOST" looks like where a ping would be -- is a human
# question and stays in research/handson_runbook_2026-07-27_arc_a_roster.md.

$ErrorActionPreference = 'Stop'
$root = 'D:\Projects\Programming\VOTV_MP'
$rel  = 'WindowsNoEditor\VotV\Binaries\Win64'
$dirs = [ordered]@{
    HOST    = Join-Path $root "Game_0.9.0n_HOST\$rel"
    CLIENT1 = Join-Path $root "Game_0.9.0n_CLIENT_1\$rel"
    CLIENT2 = Join-Path $root "Game_0.9.0n_CLIENT_2\$rel"
    CLIENT3 = Join-Path $root "Game_0.9.0n_CLIENT_3\$rel"
}
$shots = Join-Path $root 'research\roster_shots'
New-Item -ItemType Directory -Force -Path $shots | Out-Null

function Get-PeerProc {
    param([string]$Dir)
    @(Get-Process VotV-Win64-Shipping -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -and $_.Path.StartsWith($Dir, [StringComparison]::OrdinalIgnoreCase) })
}

Add-Type -Namespace Win -Name Msg -MemberDefinition @'
[DllImport("user32.dll")] public static extern int PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
'@
$WM_KEYDOWN = 0x0100; $WM_KEYUP = 0x0101; $VK_OEM_3 = 0xC0

function Send-TildeDown { param($Proc)
    [Win.Msg]::PostMessage($Proc.MainWindowHandle, $WM_KEYDOWN, [IntPtr]$VK_OEM_3, [IntPtr]0) | Out-Null }
function Send-TildeUp { param($Proc)
    [Win.Msg]::PostMessage($Proc.MainWindowHandle, $WM_KEYUP, [IntPtr]$VK_OEM_3, [IntPtr]0) | Out-Null }

# Wait until a log contains $Rx. Returns $false on timeout so the caller throws
# with a MEANINGFUL reason -- a shot taken before its precondition holds is not a
# weaker result, it is a picture of the wrong thing.
function Wait-ForLogHits {
    param([string]$Log, [string]$Rx, [int]$TimeoutMin)
    $deadline = (Get-Date).AddMinutes($TimeoutMin)
    while ((Get-Date) -lt $deadline) {
        if (Test-Path $Log) {
            $n = @(Get-Content -LiteralPath $Log -Tail 80000 |
                   Select-String -Pattern $Rx -CaseSensitive).Count
            if ($n -ge 1) { return $true }
        }
        Start-Sleep -Seconds 5
    }
    return $false
}

$fail = New-Object System.Collections.Generic.List[string]
$smoke = $null
try {
    Write-Host '[shot] launching smoke4 (the real tilde key is pressed per window later)...'
    $smoke = Start-Process -FilePath 'python' -ArgumentList 'tools/mp.py','smoke4','--duration','420' `
        -WorkingDirectory $root -PassThru -WindowStyle Minimized

    $deadline = (Get-Date).AddMinutes(7)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 5
        if (@(Get-Process VotV-Win64-Shipping -ErrorAction SilentlyContinue).Count -ge 4) { break }
    }
    if (@(Get-Process VotV-Win64-Shipping -ErrorAction SilentlyContinue).Count -lt 4) {
        throw 'four peers never came up; nothing to photograph'
    }

    # PRECONDITIONS -- and both of the obvious ones are WRONG, each measured on its
    # own failed run:
    #   * "knows all four rows" is not enough. CLIENT_3 knew them while sitting on
    #     the OMEGA WARNING screen and photographed as "PLAYERS offline".
    #   * "net_pump: ClientWorldReady announced" is not enough either. It means the
    #     SYNC layer is satisfied (world up, registry coherent, load tail quiesced)
    #     and it fires BEFORE the client has opened and rendered the level -- the
    #     second run passed this gate and still caught CLIENT_3 mid `open untitled 1`
    #     with the connect dialog up.
    # The causal marker for "this peer is actually simulating and visible" is
    # ANOTHER peer seeing its first pose: that requires a live pawn streaming. It is
    # what the replacement drill waits on, which is why that drill never caught a
    # peer on a loading screen.
    foreach ($k in 'CLIENT1','CLIENT2','CLIENT3') {
        $log = Join-Path $dirs[$k] 'multivoid.log'
        if (-not (Wait-ForLogHits $log 'ledger: slot 3 occupied by #' 6)) {
            throw "$k never learned the slot-3 row; the shot would prove nothing"
        }
    }
    # Every client's pawn must be visible TO THE HOST -- one host-side check covers
    # all three, and slots 1..3 are exactly the three clients.
    $hostLog = Join-Path $dirs.HOST 'multivoid.log'
    foreach ($slot in 1,2,3) {
        if (-not (Wait-ForLogHits $hostLog "first remote pose on slot $slot -> auto-spawning puppet" 6)) {
            throw "the host never saw slot $slot stream a pose; that peer is still loading"
        }
    }
    Write-Host '[shot] every client is simulating (host sees all three puppets); 15 s settle.'
    Start-Sleep -Seconds 15

    $cap = Join-Path $root 'tools\capture_window.ps1'
    foreach ($k in $dirs.Keys) {
        $p = Get-PeerProc $dirs[$k]
        if ($p.Count -lt 1) { $fail.Add("$k -- process gone at capture time"); continue }
        $out = Join-Path $shots "roster_$($k.ToLower()).png"
        # HOLD the real tilde across the capture: toggle on the host, hold-to-peek
        # on a client, so holding is correct for both.
        #
        # The hold is 2.5 s with repeats, not a single press with a 600 ms wait.
        # Measured: 600 ms missed CLIENT_3 entirely -- it was at 4.2 FPS rebuilding
        # its node graph, so the window covered ~2 frames and the overlay had not
        # drawn the list before the BitBlt. A hold sized in MILLISECONDS is really a
        # hold sized in FRAMES, and a loaded peer is exactly the one whose frames are
        # scarce. Repeats also mimic real key auto-repeat.
        for ($i = 0; $i -lt 5; $i++) { Send-TildeDown $p[0]; Start-Sleep -Milliseconds 500 }
        & powershell -NoProfile -ExecutionPolicy Bypass -File $cap -ProcId $p[0].Id -Out $out | Out-Null
        Send-TildeUp $p[0]
        if (Test-Path $out) {
            Write-Host ("  {0} -> {1} ({2:N0} B)" -f $k, (Split-Path $out -Leaf), (Get-Item $out).Length)
        } else {
            $fail.Add("$k -- capture produced no file")
        }
    }

    # The log-side companion to the picture: what the ledger believed at shot time.
    Write-Host ''
    foreach ($k in 'CLIENT1','CLIENT2','CLIENT3') {
        $log = Join-Path $dirs[$k] 'multivoid.log'
        $rows = @(Get-Content -LiteralPath $log -Tail 80000 |
                  Select-String -Pattern 'ledger: slot \d+ occupied by #' -CaseSensitive)
        Write-Host "$k ledger rows: $($rows.Count)"
        foreach ($r in $rows) { Write-Host "  $r" }
        if ($rows.Count -lt 3) { $fail.Add("$k -- fewer than 3 occupancy lines; TAB cannot be complete") }
    }
}
finally {
    if ($smoke) { try { Stop-Process -Id $smoke.Id -Force } catch {} }
    try { & python (Join-Path $root 'tools\mp.py') kill | Out-Null } catch {}
}

Write-Host ''
if ($fail.Count -gt 0) {
    Write-Host "[shot] FAIL -- $($fail.Count):"
    foreach ($f in $fail) { Write-Host "  - $f" }
    exit 1
}
Write-Host "[shot] captured. The PNGs must be READ, not just counted: a client's list has to show"
Write-Host "       FOUR rows and an ID column (#1 host, #2/#3/#4 clients). $shots"
exit 0
