--[[
  probe_terminals.lua v3 -- hands-on hookless probe.

  v1 crashed (RegisterHook on BP funcs). v2 mostly worked but crashed
  when invoking BndEvt OnClicked UFunctions WITHOUT terminal active-use
  state (E-12-CR1 finding).

  v3 strategy: AUTO-FIRE dangerous tests only when the player is actively
  using a terminal (mainPlayer.activeInterface != nil). Continuous state
  polling logs deltas so we capture E-press / Escape / scroll / WASD
  transitions while the user plays.

  All log lines tagged [TERMINALS-PROBE] for grep.
]]

local UEHelpers = require("UEHelpers")

local M = {}

local TAG = "[TERMINALS-PROBE]"
local function log(m) print(TAG .. " " .. m .. "\n") end
local function logf(fmt, ...) log(string.format(fmt, ...)) end

local function safe(label, fn)
    local ok, err = pcall(fn)
    if not ok then
        local es = type(err) == "string" and err or "<non-string>"
        log("ERR " .. label .. ": " .. es)
    end
    return ok
end

local function fnameStr(fn)
    if not fn then return "nil" end
    local ok, s = pcall(function() return fn:ToString() end)
    return ok and s or "?"
end

local function fullName(obj)
    if not obj or not obj:IsValid() then return "nil" end
    local ok, n = pcall(function() return obj:GetFullName() end)
    return ok and n or "?"
end

local function shortName(obj)
    if not obj or not obj:IsValid() then return "nil" end
    local ok, n = pcall(function() return obj:GetFName():ToString() end)
    return ok and n or "?"
end

-- ============================================================
-- One-shot enumeration (runs once at probe start).
-- ============================================================
local function EnumerateOnce()
    log("=== v3 ENUMERATE ===")
    safe("e1", function()
        local ads = FindFirstOf("analogDScreenTest_C")
        if ads and ads:IsValid() then
            logf("analogDScreenTest: %s rootConsole=%s",
                 fullName(ads),
                 ads.rootConsole and fullName(ads.rootConsole) or "nil")
        end
        for _, cls in ipairs({ "panel_radar_C", "panel_SATconsole_C",
                                "coordRadarDish_C", "serverBox_C",
                                "spaceRenderer_C", "driveSlot_C" }) do
            local insts = FindAllOf(cls) or {}
            local n = 0
            for _, o in pairs(insts) do if o:IsValid() then n = n + 1 end end
            logf("  %s instances = %d", cls, n)
        end
    end)
    safe("e2-screenbtns", function()
        local ads = FindFirstOf("analogDScreenTest_C")
        if not (ads and ads:IsValid()) then return end
        local cls = ads:GetClass()
        local children = cls.Children
        local hops, clicked, overlap = 0, 0, 0
        while children and children:IsValid() and hops < 2000 do
            hops = hops + 1
            local fname = fnameStr(children:GetFName())
            if fname:find("OnClickedSignature") then clicked = clicked + 1
            elseif fname:find("BeginOverlapSignature") then overlap = overlap + 1 end
            children = children.Next
        end
        logf("ads class: %d functions walked, %d OnClicked, %d BeginOverlap", hops, clicked, overlap)
    end)
end

-- ============================================================
-- Continuous state-delta poller. Logs only when something changes.
-- ============================================================
local prev = {
    activeInterface = nil,
    lookAtActor = nil,
    lookAtComponent = nil,
    coords_x = nil, coords_y = nil,
    move_r = nil, move_l = nil, move_u = nil, move_d = nil,
    isMoving = nil,
    sDL = nil, sVol = nil, sList = nil, sMin = nil, sMax = nil,
    active_play = nil, active_dl = nil, active_coords = nil, active_comp = nil,
    DL_po = nil, DL_Fr = nil, play_vol = nil, play_sel = nil,
    player_using_name = nil,
    auto_tests_ran = false,
}

local function PollOnce()
    safe("poll", function()
        local pc = UEHelpers.GetPlayerController()
        if not (pc and pc:IsValid() and pc.Pawn and pc.Pawn:IsValid()) then return end
        local mp = pc.Pawn

        local cur = {}
        cur.activeInterface = (mp.activeInterface and mp.activeInterface:IsValid()) and fullName(mp.activeInterface) or "nil"
        cur.lookAtActor     = (mp.lookAtActor and mp.lookAtActor:IsValid()) and shortName(mp.lookAtActor) or "nil"

        local ads = FindFirstOf("analogDScreenTest_C")
        if ads and ads:IsValid() then
            cur.player_using_name = (ads.player_using and ads.player_using:IsValid()) and fullName(ads.player_using) or "nil"
            cur.sDL    = ads.scroll_downloadScroll
            cur.sVol   = ads.scroll_volume
            cur.sList  = ads.scroll_list
            cur.sMin   = ads.scroll_remapMin
            cur.sMax   = ads.scroll_remapMax
            cur.active_play   = ads.active_play
            cur.active_dl     = ads.active_download
            cur.active_coords = ads.active_coords
            cur.active_comp   = ads.active_comp
            cur.DL_po    = ads.DL_poFilterSpeed
            cur.DL_Fr    = ads.DL_FrFilterSpeed
            cur.play_vol = ads.play_volume
            cur.play_sel = ads.play_selectIndex
        end

        local sr = FindFirstOf("spaceRenderer_C")
        if sr and sr:IsValid() then
            cur.coords_x = sr.coords.X
            cur.coords_y = sr.coords.Y
            cur.move_r   = sr.move_right
            cur.move_l   = sr.move_left
            cur.move_u   = sr.move_up
            cur.move_d   = sr.move_down
            cur.isMoving = sr.isMoving
        end

        -- Compare to prev; log only deltas.
        local changed = {}
        for k, v in pairs(cur) do
            if prev[k] ~= v then
                table.insert(changed, string.format("%s:%s->%s", k, tostring(prev[k]), tostring(v)))
                prev[k] = v
            end
        end
        if #changed > 0 then
            logf("DELTA %s", table.concat(changed, " "))
        end

        -- AUTO-FIRE: when activeInterface transitions from nil to non-nil,
        -- run the dangerous tests safely.
        if cur.activeInterface and cur.activeInterface ~= "nil" and not prev.auto_tests_ran then
            prev.auto_tests_ran = true
            log("=== activeInterface DETECTED — auto-firing safe tests in 1s ===")
            ExecuteWithDelay(1000, function() ExecuteInGameThread(function() AutoFireSuite(mp) end) end)
        end
        -- Reset auto-tests guard when user exits (activeInterface back to nil).
        if cur.activeInterface == "nil" and prev.auto_tests_ran then
            prev.auto_tests_ran = false
            log("=== activeInterface CLEARED — ready for next entry ===")
        end
    end)
end

-- ============================================================
-- Auto-fire suite — runs only when player is ACTIVELY using a terminal.
-- ============================================================
function AutoFireSuite(mp)
    log("=== AUTO-FIRE SUITE START ===")
    local ads = FindFirstOf("analogDScreenTest_C")
    if not (ads and ads:IsValid()) then log("no ads"); return end

    -- (a) playerHandMouseWheel WITH active state
    safe("af-wheel", function()
        local before = ads.DL_poFilterSpeed
        ads:playerHandMouseWheel(mp, 1.0)
        local after = ads.DL_poFilterSpeed
        logf("[F-8.11] playerHandMouseWheel(+1) DL_po %s -> %s (delta=%s)",
             tostring(before), tostring(after), tostring(after - before))
    end)
    safe("af-wheel-neg", function()
        local before = ads.DL_poFilterSpeed
        ads:playerHandMouseWheel(mp, -1.0)
        local after = ads.DL_poFilterSpeed
        logf("[F-8.11] playerHandMouseWheel(-1) DL_po %s -> %s (delta=%s)",
             tostring(before), tostring(after), tostring(after - before))
    end)

    -- (b) screenbutton OnClicked direct invoke — now safe (active state)
    safe("af-screenbtn", function()
        local cls = ads:GetClass()
        local children = cls.Children
        local hops = 0
        while children and children:IsValid() and hops < 2000 do
            hops = hops + 1
            local fname = fnameStr(children:GetFName())
            if fname:find("screenbutton_switchToCoord1") and fname:find("OnClickedSignature") then
                local before = ads.active_coords
                local ok = pcall(function() ads[fname](ads, nil, nil) end)
                local after = ads.active_coords
                logf("[F-8.10] invoke %s ok=%s active_coords %s->%s",
                     fname:sub(1, 80), tostring(ok), tostring(before), tostring(after))
                break
            end
            children = children.Next
        end
    end)

    -- (c) gatherSignal direct invoke
    safe("af-gather", function()
        local sr = FindFirstOf("spaceRenderer_C")
        if sr and sr:IsValid() then
            local ok = pcall(function()
                local ret, idx, dir, radCheck, anyCaught = sr:gatherSignal(false)
                logf("[F-8.5] gatherSignal -> ret=%s idx=%s dir=%s rad=%s caught=%s",
                     tostring(ret), tostring(idx), tostring(dir),
                     tostring(radCheck), tostring(anyCaught))
            end)
            logf("  call_ok=%s signals_count=%s", tostring(ok), tostring(#sr.signals))
        end
    end)

    -- (d) spaceRenderer integrator empirical probe
    safe("af-integrator", function()
        local sr = FindFirstOf("spaceRenderer_C")
        if sr and sr:IsValid() then
            local cx_before, cy_before = sr.coords.X, sr.coords.Y
            sr.move_up = true
            logf("[F-8.1] integrator: set move_up=true, coords_before=(%.2f,%.2f)", cx_before, cy_before)
            ExecuteWithDelay(500, function() ExecuteInGameThread(function()
                local cx_mid, cy_mid = sr.coords.X, sr.coords.Y
                logf("[F-8.1] +500ms coords=(%.2f,%.2f) delta=(%.2f,%.2f)",
                     cx_mid, cy_mid, cx_mid - cx_before, cy_mid - cy_before)
            end) end)
            ExecuteWithDelay(1500, function() ExecuteInGameThread(function()
                local cx_end, cy_end = sr.coords.X, sr.coords.Y
                logf("[F-8.1] +1500ms coords=(%.2f,%.2f) delta=(%.2f,%.2f)",
                     cx_end, cy_end, cx_end - cx_before, cy_end - cy_before)
                sr.move_up = false
                log("[F-8.1] integrator: reset move_up=false")
            end) end)
        end
    end)

    -- (e) setData audio side-effects: poll audio before, call setData, poll after
    safe("af-setdata", function()
        local audio_fields = {
            "audio_redphone", "audio_coordFail", "audio_coordKeyPress",
            "audio_coordDishSwitchSound", "newdesk_radioLoop_frequency",
            "newdesk_radioLoop_frequencyTune", "newdesk_beepLoop_polarityMove",
            "audio_coordButtonSound", "corrds_loop", "audio_coord_pingSound",
            "audio_coord_pingLoop", "audio_scrape", "computerWorking_Cue",
            "computerHum_coords", "computerHum_play", "computerHum_downl",
            "prog", "deny", "Done", "beep_detecFinish", "beep_detecEmpty",
            "beep_detecProcess", "signalSound",
        }
        local function active_set()
            local s = {}
            for _, fn in ipairs(audio_fields) do
                pcall(function()
                    local ac = ads[fn]
                    if ac and ac:IsValid() then
                        local p = false
                        pcall(function() p = ac:IsPlaying() end)
                        if p then table.insert(s, fn) end
                    end
                end)
            end
            return s
        end
        local before = active_set()
        logf("[F-8.7] BEFORE setData: %d audio playing (%s)", #before, table.concat(before, ","))
        -- gatherData -> setData round-trip
        local ok = pcall(function()
            local state = ads:gatherData()
            if state then
                ads:setData(state)
            else
                log("  gatherData returned nil — skipping setData round-trip")
            end
        end)
        logf("  gatherData/setData call_ok=%s", tostring(ok))
        ExecuteWithDelay(500, function() ExecuteInGameThread(function()
            local after = active_set()
            local same = (#before == #after)
            logf("[F-8.7] AFTER setData (+500ms): %d audio playing %s",
                 #after, same and "(UNCHANGED)" or "(CHANGED)")
            if not same then logf("  diff: %s", table.concat(after, ","))end
        end) end)
    end)

    log("=== AUTO-FIRE SUITE END ===")
end

-- Continuous polling: every 250ms.
local function StartPoller()
    LoopAsync(250, function()
        PollOnce()
        return false  -- continue looping
    end)
end

-- Manual snapshot keybind (CTRL+5)
local function ManualSnapshot()
    log("=== CTRL+5 MANUAL SNAPSHOT ===")
    PollOnce()
    -- Force-log even if no delta
    local ads = FindFirstOf("analogDScreenTest_C")
    if ads and ads:IsValid() then
        logf("  ads: active(p=%s d=%s c=%s comp=%s) scroll(DL=%s vol=%s list=%s rMin=%s rMax=%s)",
             tostring(ads.active_play), tostring(ads.active_download),
             tostring(ads.active_coords), tostring(ads.active_comp),
             tostring(ads.scroll_downloadScroll), tostring(ads.scroll_volume),
             tostring(ads.scroll_list), tostring(ads.scroll_remapMin),
             tostring(ads.scroll_remapMax))
        logf("  ads: player_using=%s controllingCoord=%s",
             ads.player_using and fullName(ads.player_using) or "nil",
             tostring(ads.controllingCoordinatePanel))
    end
    local sr = FindFirstOf("spaceRenderer_C")
    if sr and sr:IsValid() then
        logf("  sr: coords=(%.2f,%.2f) move=R%s/L%s/U%s/D%s vel=%.2f isMoving=%s",
             sr.coords.X, sr.coords.Y,
             tostring(sr.move_right), tostring(sr.move_left),
             tostring(sr.move_up), tostring(sr.move_down),
             sr.movementVelocity, tostring(sr.isMoving))
    end
end

function M.Run()
    log("v3 probe starting — hookless polling + auto-fire on terminal-active-state")
    log("USER: walk to the SAT computer (big analog desk), press E to activate it,")
    log("      wait ~5s for auto-fire suite, press Escape to exit.")
    log("      Optional: do the same at a panel_radar / coordRadarDish / serverBox.")
    log("      The probe logs DELTAS continuously; CTRL+5 forces a snapshot.")

    ExecuteInGameThread(EnumerateOnce)
    ExecuteWithDelay(2000, function() ExecuteInGameThread(StartPoller) end)

    -- Manual snapshot keybind: CTRL+5
    RegisterKeyBind(Key.NUM_FIVE, { ModifierKey.CONTROL }, function()
        ExecuteInGameThread(ManualSnapshot)
    end)
    log("CTRL+5 manual snapshot keybind registered")
end

return M
