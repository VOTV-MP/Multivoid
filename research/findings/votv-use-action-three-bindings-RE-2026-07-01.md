# RE: the "use" (E) InputAction has THREE delegate bindings, not one (2026-07-01)

**Durable RE.** Why the client heard `use_deny` "EHHH" on every E-press even though our `InpActEvt_use_41`
interceptor cancelled the dispatch. Source: `research/bp_reflection/mainPlayer.json` (UAsset export dump) +
`_useaction_full.txt` + `_mainplayer_uber_full.txt`.

## The fact
`AmainPlayer_C`'s "use" InputAction compiles to **THREE** `K2Node_InputActionEvent` delegate bindings, all
listed in `Export 483` (`InputActionDelegateBinding_0`):

| Node | Trigger | Hooked before 2026-07-01? |
|---|---|---|
| `InpActEvt_use_K2Node_InputActionEvent_41` | **IE_Pressed** | YES (`MainPlayerUseInputEventFn`) |
| `InpActEvt_use_K2Node_InputActionEvent_38` | **IE_Pressed** | NO |
| `InpActEvt_use_K2Node_InputActionEvent_42` | **IE_Released** | NO |

Two Pressed bindings (`_38`, `_41`) both fire on a single E-tap; `_42` fires on release. Each runs the use
flow that can reach `useAction`'s `use_deny` (`PlaySound2D(use_deny, 0.25)` at `useAction` bytecode offsets
1350 / 1425 / 1511 / 2256; the ubergraph also has its OWN `use_deny` @21791 for the dream/sleep/dead/pause
branch, distinct from the pile path). `useAction` is called from the ubergraph at @91916 and @114936.

## Consequence (the EHH regression)
Hooking only `_41` and cancelling its ProcessEvent dispatch kills the grab + deny **from `_41`'s path only**.
`_38` (the second IE_Pressed) still fires on every E-press, unhooked → runs `useAction` → plays `use_deny` in
PARALLEL with our cancelled grab. So the client heard "EHHH" on every grab/release even though the log said
`native use CANCELLED (no use_deny)` — that log was true FOR `_41`, but not for `_38`/`_42`.

The `dc8bd6af` interceptor comment claimed "cancelling the dispatch subsumes [the deny]" — WRONG: it subsumes
the deny on one of three seams.

## Fix (d7620ed5)
A side-effect-free deny-suppressor (`OnPileUseDenySuppress`) is registered on `_38` and `_42` too. On a CLIENT
pile interaction (carrying, or aimed at a bound-native/proxy pile) it ONLY returns true (cancel the dispatch,
killing the parallel deny) — it does NOT send a grab/throw intent or play a cue (`_41` is the sole author, and
`_42`-on-release must never throw). Host + non-pile presses are unaffected (client-only; returns false when not
aimed at a pile). Constants: `MainPlayerUseInputEventFn38`, `MainPlayerUseInputEventFn42R` (sdk_profile.h).

## Generalizable lesson
An UE InputAction can have MULTIPLE delegate bindings (multiple event-graph nodes + press/release edges). A
ProcessEvent interceptor on ONE `InpActEvt_<name>_<NN>` UFunction covers only that node. To fully gate an
action, enumerate ALL its bindings (the class's `InputActionDelegateBinding` export) and hook each. The K2Node
ordinals (`_38/_41/_42`) are BP-recook-fragile (sdk_check flags the embedded ordinal). See
[[lesson-input-action-multiple-delegate-bindings]].
