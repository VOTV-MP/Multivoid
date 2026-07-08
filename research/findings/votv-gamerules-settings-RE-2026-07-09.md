# VOTV world-rules / game-settings RE + coop verdict + F1 panel AS-BUILT — 2026-07-09

**Target**: Alpha 0.9.0-n. **Task**: how does coop handle VOTV's world-creation settings + general
game settings so peers don't diverge (user worry: client turns fall damage OFF, host ON → one dies
from a fall, the other doesn't)? Plus the user ask: an F1 menu section showing the world rules FOR
EVERYONE. **Method**: CXXHeaderDump (struct layout authority) + bytecode reflection (kismet-json
disasm) + a real 2-peer LAN smoke with a read-back probe. Verified, not guessed.

---

## 0 — TL;DR (the verdict)

**World-rules sync needs NO gameplay build.** The world rules a peer plays under live in ONE struct,
`Fstruct_gameRules`, and every read funnels through the **per-peer `mainGameInstance.gameRules`**. A
joining client boots from the **host's LIVE-captured save** (`save_transfer.cpp:394`), so the host's
rules ride the GVAS save blob into the client's `loadObjects()` and populate the client's GI copy —
**host-authoritative for free**, the same root as `COOP_WORLD_PROP_DIVERGENCE.md` (host save = single
source of truth). A settings-broadcast packet was **rejected**: it would MASK a load-copy bug rather
than fix it. **G1 (does the client's GI actually end up holding the host's rules?) = CONFIRMED** on a
real smoke (§4).

The user-facing deliverable = a **read-only F1 > World > Rules panel for everyone** that reads the
local `GI.gameRules` (so any host/client mismatch stays visible, not masked). Shipped: commit
`27994f09`, DLL `bee181a6`.

---

## 1 — The rule struct: `Fstruct_gameRules` (MEASURED, CXXHeaderDump/struct_gameRules.hpp)

Size **0x29 bytes**, **~36 members** (all `bool` except 2 `TEnumAsByte` + 1 `float` — **no int32, no
FName**, which is what lets the F1 panel classify each field by size/bool-payload):

- `difficulty` (TEnumAsByte<enum_difficulty> @0x00), `fallDamage` (@0x01), `dailyTaskFine`,
  `enablePermanentSeason`, `permanentSeason` (TEnumAsByte<enum_seasons> @0x04), `extremeCombat`,
  `daySpeedMult` (float @0x08), `foodSpoilage`, `foodFatigue`, `physEvents`, `funnySetting`,
  `customContent`, `enableLightsOut`, `enableRadioTowerDecay`, `enableCoordRadarDecay`,
  `enableTransformerDecay`, `config`, 8× `enableMG_*` (math/wires/hack/slider/maze/pipe/simon/compare),
  `permanentFog`, `permanentRain`, `weeklySeason`, `permanentBlackhole`, `enableHolidays`,
  `experimentalWip`, `enableWaterFallDamage`, `noGrass`, `bloodLoss`, `enableNightmares`,
  `enableColdSwapUpgrades`.
- Enum **display names are stripped in the cook** (`enum_difficulty`/`enum_seasons`/`enum_gamemode` all
  read `NewEnumerator0..N`) → render difficulty/season as ordinals, don't invent names. `enum_gamemode`
  ordinals DO map (save-picker RE): 0 Story, 1 Infinite, 4 Sandbox, 5 Halloween, 6 Ambience, 7 Solar.

## 2 — Where it lives + who reads it (MEASURED, bytecode)

Three storage sites, but ONE is the runtime authority:
- **`mainGameInstance.gameRules` @0x0318** (0x2C slot) — **THE runtime authority**. The universal
  accessor is `lib` reading `getMainGameInstance().gameRules` (lib.json:128657); `mainGamemode` reads
  `gameInstance.gameRules.difficulty` (mainGamemode.json:70200); `mainPlayer` caches
  `enableWaterFallDamage` from a per-landing gamemode query (mainPlayer.json:102435; the gamemode also
  exposes `Is Player Fall Damage` — mainGamemode.json:2764). GameInstance is **one per process, never
  replicated, survives level loads.**
- **`saveSlot.localGameRules` @0x0DB0** — the SAVED copy (baked at world creation by the saveSlots
  widget `ui_gameRulesList_newSlot`; a serialized GVAS field → rides the save blob). [save-picker RE]
- **`mainGamemode.settings`** (StructProperty) + `settingsSlotCopy` + `gamemodeSettingsApplied`
  delegate — the gamemode's applied copy. `difficulty`/`permanentSeason` are **delegate-applied**
  (gamemodeSettingsApplied / seasonUpdated fire derived state), so a raw struct stomp would skip the
  apply — the correct "make client == host" is VOTV's own load-apply, not a field write.

**No mid-session gameRules writer found** (no `setDifficulty`/`setDaySpeed`/rule-setter in
`mainGamemode`/`lib`; the console only READS gameRules) → rules are effectively static post-creation
(partial UI-dump caveat; the F1 panel snapshots on tab-open so a stray writer can't leave it stale).

## 3 — The two categories (the "generalized settings" answer)

- **(A) World rules** = `Fstruct_gameRules` — gameplay law, must be host-authoritative + uniform.
  Rides the host save-load (§0). Fall damage / funny / custom content / seasons / difficulty are all A.
- **(B) User/system settings** (graphics, audio, sensitivity, keybinds, FOV) — a SEPARATE settings
  object, per-client, **correctly never synced**. Not `Fstruct_gameRules`.
- `customContent` is A (the flag gates whether CC assets exist in the shared world), BUT its **asset
  availability** (a client may lack the host's CC files) is a separate content-delivery/product
  question — a flag broadcast can't conjure missing files. UNBUILT / open product question.

## 4 — G1 CONFIRMED on a real 2-peer LAN smoke (matching-log evidence, not "smoke passed")

The `VOTVCOOP_RUN_WORLDRULES_PROBE=1` probe ran `ue_wrap::game_rules::ReadLocal` on BOTH peers ~35 s
post-join and logged every rule. **Host and client are IDENTICAL field-by-field + `gamemode=Story`**
(host log `votv-coop-host.log`, client `votv-coop-client.log`, 2026-07-08 23:41). So the client's
`GI.gameRules` ends up holding the host's rules — the save-load carries them. Smoke otherwise clean
(RESULT: PASS, ~45 s steady-state, no crash/SEH/Warn, join both ways).

**CAVEAT (honest):** both peers showed **story-default** rules, so this confirms the mechanism
(client GI populated + read correctly + == host) but does NOT discriminate "load copied the host's
NON-default rules" from "both default-initialized identically." **The fuller test is a human create-
a-world-with-a-non-default-rule check** (e.g. fall damage OFF → client's F1 panel shows OFF).

## 5 — AS-BUILT: the F1 > World > Rules panel (commit 27994f09, DLL bee181a6)

- **`ue_wrap::reflection::EnumerateStructFields(structObj)`** (NEW primitive) — walks a UStruct's own
  ChildProperties FField chain → `{name, offset, size, flags}`. Reuses the FunctionParams walk shape.
- **`ue_wrap::game_rules::ReadLocal(Snapshot&)`** (NEW) — re-resolves the GameInstance FRESH each call
  (never a captured pointer), reads `gameRules` by name (`FindPropertyOffset` + `PropertyInnerStruct`),
  enumerates members, classifies each (`FindBoolProperty`→Bool / size==4→Float / else 1-byte→Enum —
  measured-complete for this struct), reads GameMode by name → friendly gamemode. Labels derive from
  the (unique) BP member name with the `_NN_GUID` tail trimmed → rot-proof, auto-adapts.
- **`ui::world_rules_panel::Render()`** (NEW) — read-only table; snapshot-on-(re)open via
  `game_thread::Post` one-shot (mutex-guarded cache; open-edge via `ImGui::GetFrameCount` gap) + a
  Refresh button. No per-frame walk, no poll.
- **`dev_menu` tab** — a new non-dev, non-host category `World` > `Rules` (the gate
  `if (cat.dev && !devMode) continue;` passes dev=false categories to clients; `g_selCat` reset only
  touches host-gated selections). Shown to host + clients + solo.
- **`harness/autotest` WorldRulesProbeThread** — the §4 G1 probe (env `VOTVCOOP_RUN_WORLDRULES_PROBE`).
- Principle 7: `ue_wrap::game_rules` owns the reflected read; `ui/world_rules_panel` renders.

## 6 — Cross-refs
- `docs/COOP_WORLD_PROP_DIVERGENCE.md` — the parent class (host save = authority). §"static vs mutating"
  note added 2026-07-09: STATIC world-state (gameRules) rides the save with no divergence; only
  MUTATING autonomous local-accumulator state (a prop's own dryTimer) diverges.
- `research/findings/votv-save-picker-create-new-RE-2026-06-06.md` (localGameRules @0x0DB0, the
  create-path, the enum_gamemode ordinal map).
- Memory: `memory/project_world_rules_panel_2026-07-08.md`.
