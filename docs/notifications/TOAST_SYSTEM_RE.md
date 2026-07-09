# VOTV Toast / Hint system — MECHANISM (RE 2026-07-09)

Code-verified against `research/bp_reflection/*.json` + `research/pak_re/cfg/*/*.txt` bytecode +
`docs/COOP_DISPATCH_VISIBILITY.md`. Confidence: **[V]** from-code/reflection · **[RD]** from-a-dump ·
**[?]** needs-a-runtime-probe.

## The one primitive: `lib_C::addHint`
Every corner toast funnels through ONE static Blueprint function. **[V]**
- **Defined in `lib_C`** (the game's BP function library `/Game/main/lib`), NOT mainPlayer/mainGamemode
  (those only CALL it). Def: `research/bp_reflection/lib.json:47340` (export), body `46853-47328`.
- Signature (`FUNC_Static, FUNC_BlueprintCallable, FUNC_BlueprintEvent`):
  | # | Param | Type | Meaning |
  |---|---|---|---|
  | 1 | `InText` | Text | the message string (`lib.json:46691`) |
  | 2 | `type` | Byte enum **`enum_notifyType`** | icon/severity/category. Seen: **0**=info/normal, **2**=error (the red `SAVE ERROR`), **3** seen. (`lib.json:46712`) **[?]** full value→meaning needs the enum dumped |
  | 3 | `debug` | Bool | editor-only gate (dead in Shipping). NOT a duration. (`lib.json:46738`) |
  | 4 | `__WorldContext` | Object | world-context to find the local gamemode (`lib.json:46759`) |
- Body: `if (debug && WithEditor())` editor branch → `getMainGamemode(ctx)` → create a **`ui_hint_C`**
  widget (`WidgetBlueprintLibrary::Create`, import `-1191`) → **`gamemode.hints.verT_hinTs.AddChild(newHint)`**
  → `newHint.begin(InText)`. **[V lib.json:47156-47318]**

## The widget / queue
- **`mainGamemode.hints`** = a **`ui_hints_C`** widget (`/Game/umg/misc/ui_hints`), created in gamemode
  BeginPlay and `AddToViewport(ZOrder 10)`. Prop def `mainGamemode.json:359335`; creation bytecode
  `mainGamemode.txt:7279-7294`. **[V]**
- The "active/queued toasts" state is **NOT an array property** — it is the **UMG child list of the
  VerticalBox `ui_hints_C::verT_hinTs`**. Each on-screen toast = one `ui_hint_C` child. To read/clear the
  queue: enumerate `verT_hinTs` (`GetChildrenCount`/`GetChildAt`) or `ClearChildren`/`RemoveChildAt`; to
  drop one, `RemoveFromParent` the child. **[V]** — but `verT_hinTs`'s exact property offset + `ui_hint_C`'s
  text/duration fields + auto-expire are **[?] not in bp_reflection** (only the raw `ui_hints.uexp` exists;
  dump `ui_hints_C` + `ui_hint_C` via a runtime reflection probe before relying on offsets).

## THE LOAD-BEARING FACT — dispatch is INVISIBLE to both our seams
Every `addHint` call site is `EX_Context (Default__lib_C CDO) → EX_LocalVirtualFunction "addHint"`
(measured in mainPlayer.json:97738/98433/100536, mainGamemode.txt:8868/8895, serverBox). **[V]**

`addHint` is a **SCRIPT (BP) UFunction invoked via `EX_LocalVirtualFunction`** → it dispatches through
**`ProcessLocalScriptFunction`**, which touches **NEITHER `UObject::ProcessEvent` (our detour) NOR
`UFunction::Func` (our thunk-patch)**. Per `COOP_DISPATCH_VISIBILITY.md:126-133` this is the exact
"invisible to both" class (and `[[lesson-script-fn-invisible-to-func-patch]]`). **Consequences:**
- **You CANNOT intercept/cancel the `addHint` call at the call** via our two seams.
- **A Func patch on `lib_C::addHint` will NOT fire** for these local calls (the existing
  `save_indicator_suppress` addHint hook is on a *different, gamemode-level* wrapper and is marked
  latent/probe-only — and most toasts bypass that wrapper straight to `Default__lib_C::addHint`).

## Suppression feasibility (given the invisibility)
| Approach | Feasible? | Notes |
|---|---|---|
| (a) Intercept/cancel the `addHint` call | **NO** via our seams. Only a **native hook on `ProcessLocalScriptFunction`** with a per-call name filter could cancel-at-call — broad + on a HOT path (perf-flag). Or, per `[[lesson-script-fn-invisible-to-func-patch]]`, Func-patch a NATIVE call INSIDE addHint (`WidgetBlueprintLibrary::Create` / `AddChild`) — but those fire for ALL UMG (broad). |
| (b) Poll-and-remove the queue | **YES, selective** | Poll `gamemode.hints.verT_hinTs` children each host/client tick; read each `ui_hint_C`'s text/type; `RemoveFromParent` the misleading ones. After-the-fact (a possible 1-tick flash). This is the canonical "poll the observable RESULT of an INVISIBLE event" pattern. **[?]** needs the ui_hint text-field offset. |
| (c) Hide/remove the whole widget | YES, non-selective | `SetVisibility(Hidden)` / `RemoveFromViewport` on `gamemode.hints` kills ALL toasts. Too blunt (would also kill legit local ones). |

## Mirroring the HOST's toasts — clean
The host sends a reliable event; the client calls `lib_C::addHint(text, type, false, worldCtx)` **itself via
reflected `CallFunction`**. Outbound reflected calls ARE driveable/visible because WE are the dispatcher (the
same pattern the codebase uses for alarm `runTrigger`, event fire, etc.). No dispatch-observation problem on
the mirror side. **[V pattern]**

## Where a toast's TRIGGER lives (local vs world/host-authoritative)
Rendering is **always local per machine** (`ui_hints_C` is per-machine, per-viewport). Trigger origins are
mixed:
- **Local per-player** (fine as-is): e.g. `mainPlayer` `"Cannot be used when held"`.
- **World/gamemode/server state** (the divergence problem): e.g. `serverBox_C.active_downl → addHint`
  (`serverBox.json:92/102/586/2735`), save errors (`mainGamemode.txt:8868 "SAVE ERROR: invalid mode"` type 2),
  events. Each CLIENT's own serverBox/gamemode computes its diverged local state and self-shows the toast —
  **this is why clients show false "server down"**. → suppress-on-client + mirror-from-host.

## Secondary on-screen popup systems (same invisibility, own widgets)
| System | Class.fn | Widget | Dispatch |
|---|---|---|---|
| Subtitles | `lib_C::runSubtitle` (`lib.json:46638`) | subtitle UI (uses `list_subtitles`) | `EX_LocalVirtualFunction` — INVISIBLE |
| Achievement toasts | gamemode → `ui_achievementPopup_C` into `VerticalBox_achievements` (`mainGamemode.json:369/4428/207949`) | own viewport widget | BP-internal — INVISIBLE |
| Geiger alert | `mainPlayer.geigerAlert` (state field, `mainPlayer.json:1604/444296`) | local HUD | local state |
| **Server-down** | `serverBox_C.active_downl` → `addHint` (`serverBox.json:102`); gamemode `serverDown_GEN_VARIABLE` + `serverEfficiency_downl` (`mainGamemode.json:390640/358092`) | via `addHint` → `ui_hints_C` | INVISIBLE — **NOT a distinct system**, just an `addHint` toast |

## Open probes (before implementation)
- **[?]** Dump `ui_hints_C` + `ui_hint_C` (offsets: `verT_hinTs`, the hint's text field, auto-expire timer).
- **[?]** Dump `enum_notifyType` (name each value; is there a "server/world" category we can filter by type
  instead of by text?).
- **[?]** Extract the exact localized `SERVER "X" is down` string (for any text-match suppression) from
  serverBox / `ui_hint` text.
- **[?]** Read-only Func-patch probe on `lib_C::addHint` to CONFIRM it never fires (the doc asserts it; cheap
  to verify before relying).

Related: `[[lesson-script-fn-invisible-to-func-patch]]`, `docs/COOP_DISPATCH_VISIBILITY.md`,
`src/votv-coop/src/coop/session/save_indicator_suppress.cpp` (existing latent addHint-wrapper hook).
