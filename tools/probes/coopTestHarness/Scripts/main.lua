--[[
  coopTestHarness -- autonomous test/launch automation (UE4SS v3.0.1 Lua).

  Purpose: make iterating on coop fast -- launch the game, skip the menus,
  drop straight into gameplay, screenshot, and report, with no manual
  clicking. Pairs with tools/run-test.ps1 (which launches the game windowed
  and writes the scenario file this mod reads).

  This is test tooling (an EXPERIMENT, Lua); the shipping mod is C++ in
  src/votv-coop. Retire/fold into the real harness once the C++ side exists.

  Scenario: read from Mods/coopTestHarness/scenario.txt (one word):
    newgame   -> auto-start a New Game into gameplay (most deterministic)
    loadlast  -> attempt to load the most recent save
    none      -> do nothing automatic (manual keybinds only)
  Falls back to "none" if the file is missing.

  Keybinds (CTRL):
    CTRL+8 -> run the skip-to-gameplay action now (on demand)
    CTRL+9 -> screenshot (HighResShot to the game's Saved/Screenshots)
    CTRL+7 -> report current state (map, local pawn, validity)

  RUNTIME-VALIDATION NOTE (first run, 2026-05-22+): the exact "skip to
  gameplay" call chain is grounded in the reflection dump but UNVERIFIED at
  runtime. SkipToGameplay() tries, in order: (1) invoke ui_menu_C's NewGame
  bound event (mirrors a real click, uses VOTV's own path); (2) fall back to
  OpenLevel("untitled_1"). Confirm which works and prune the other (RULE 1).
]]

local UEHelpers = require("UEHelpers")

local TAG = "[coop-harness]"
local GAMEPLAY_MAP = "untitled_1"

local function log(m) print(TAG .. " " .. m .. "\n") end

-- Read the scenario file next to this script.
local function ReadScenario()
    local path = "Mods/coopTestHarness/scenario.txt"
    local f = io.open(path, "r")
    if not f then return "none" end
    local s = (f:read("*l") or "none"):gsub("%s+", "")
    f:close()
    return s == "" and "none" or s
end

local function GetLocalPawn()
    local pc = UEHelpers.GetPlayerController()
    if pc and pc:IsValid() and pc.Pawn and pc.Pawn:IsValid() then return pc.Pawn end
    return nil
end

local function Screenshot()
    ExecuteInGameThread(function()
        local ok, err = pcall(function()
            local ksl = UEHelpers.GetKismetSystemLibrary()
            local world = UEHelpers.GetWorldContextObject()
            -- HighResShot writes to <Game>/Saved/Screenshots/WindowsNoEditor/
            ksl:ExecuteConsoleCommand(world, "HighResShot 1920x1080", nil)
        end)
        log(ok and "screenshot requested (HighResShot 1920x1080)" or ("screenshot error: " .. tostring(err)))
    end)
end

local function Report()
    ExecuteInGameThread(function()
        local pawn = GetLocalPawn()
        if pawn then
            local ok, n = pcall(function() return pawn:GetClass():GetFName():ToString() end)
            log("state: in gameplay, local pawn = " .. (ok and n or "?"))
        else
            log("state: no local pawn (likely at menu / loading)")
        end
    end)
end

-- Reflection introspection of the live UI: logs the active widgets and
-- every button's full path (which reveals the owning widget class + the
-- button's name), so we can drive the real handlers instead of clicking
-- pixels. Read these from UE4SS.log.
local function InspectWidgets()
    ExecuteInGameThread(function()
        local ok, err = pcall(function()
            local buttons = FindAllOf("Button") or {}
            local n = 0
            for _, b in pairs(buttons) do
                if b:IsValid() then n = n + 1; log("button: " .. b:GetFullName()) end
            end
            log("button count = " .. n)
            for _, cls in ipairs({ "UserWidget", "ui_menu_C" }) do
                local ws = FindAllOf(cls) or {}
                for _, w in pairs(ws) do
                    if w:IsValid() then log("widget(" .. cls .. "): " .. w:GetFullName()) end
                end
            end
        end)
        if not ok then log("inspect error: " .. tostring(err)) end
    end)
end

-- Jump straight into gameplay via VOTV's OWN load path -- no menu nav, no
-- pixel clicks. For a save: load the slot, register it on the persistent
-- mainGameInstance_C (the real entry VOTV's load uses), then OpenLevel the
-- gameplay map; the GameMode applies the save on BeginPlay. slot=nil => new.
local function EnterGameplay(slot)
    ExecuteInGameThread(function()
        local ok, err = pcall(function()
            local gs  = UEHelpers.GetGameplayStatics()
            local wco = UEHelpers.GetWorldContextObject()
            if slot and slot ~= "" then
                local gi   = FindFirstOf("mainGameInstance_C")
                local save = gs:LoadGameFromSlot(slot, 0)
                if gi and gi:IsValid() and save and save:IsValid() then
                    gi:setSaveSlotObject(save, slot)
                    gi.loadObjects = true
                    log("registered save '" .. slot .. "' on mainGameInstance_C")
                else
                    log("WARN: load slot '" .. slot .. "' or GameInstance invalid; opening map without save")
                end
            end
            gs:OpenLevel(wco, FName(GAMEPLAY_MAP), true, "")
            log("OpenLevel(" .. GAMEPLAY_MAP .. ") issued" .. (slot and (" with save '" .. slot .. "'") or " (new)"))
        end)
        if not ok then log("EnterGameplay error: " .. tostring(err)) end
    end)
end

-- Keybinds
RegisterKeyBind(Key.NUM_EIGHT, {ModifierKey.CONTROL}, function() EnterGameplay(nil) end) -- new game
RegisterKeyBind(Key.NUM_NINE,  {ModifierKey.CONTROL}, Screenshot)
RegisterKeyBind(Key.NUM_SEVEN, {ModifierKey.CONTROL}, Report)

-- Self-documenting timeline so an unattended run leaves evidence (the log
-- + screenshots, both readable after the fact).
local function StateScreenshot(label)
    log("=== " .. label .. " ===")
    Report()
    Screenshot()
end

-- scenario forms: "newgame" | "load:<slot>" | "inspect" | "none"
local function RunTimeline(scenario)
    if scenario == "inspect" then
        ExecuteWithDelay(20000, function() log("=== inspect @20s ==="); InspectWidgets() end)
        ExecuteWithDelay(40000, function() log("=== inspect @40s ==="); InspectWidgets() end)
        return
    end
    ExecuteWithDelay(20000, function() StateScreenshot("T+20s boot state") end)
    local slot = scenario:match("^load:(.+)$")
    if scenario == "newgame" or slot then
        ExecuteWithDelay(25000, function()
            log("auto: EnterGameplay(" .. (slot or "new") .. ")"); EnterGameplay(slot)
        end)
    end
    ExecuteWithDelay(50000, function() StateScreenshot("T+50s post-load state") end)
    ExecuteWithDelay(80000, function() StateScreenshot("T+80s settled state") end)
end

local scenario = ReadScenario()
log("loaded. scenario='" .. scenario .. "'. shots @20/50/80s. Keys: CTRL+8 newgame/9 shot/7 report.")
RunTimeline(scenario)
