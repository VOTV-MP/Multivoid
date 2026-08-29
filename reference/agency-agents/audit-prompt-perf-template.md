# Audit-prompt template — performance / hot-path re-entry section

Paste this section into every `feature-dev:code-reviewer` brief AND
every `testing-performance-benchmarker` brief, in addition to the
existing focus areas (RULE 1/2/3 crutch hunt, MTA fidelity, file-size
check per `[[feedback-modular-file-size-rule]]`, thread-safety,
correctness).

Born from a 2026-05-27 failure where two parallel code-reviewer
agents BOTH flagged `std::wstring` allocations in a one-shot connect-
edge replay path but BOTH missed the SAME pattern in `Install()`
called every 125 Hz from the harness pump. Result: ~1.1B wstring
allocs/sec, 19 GB RSS in one host launch. The CLAUDE.md rule against
per-frame full-array scans existed; the audit prompt did not force a
mechanical check.

---

## PASTE-ABLE SECTION (copy into audit brief verbatim)

### Hot-path re-entry analysis (MANDATORY -- fail the audit if missing)

For EVERY function defined or modified in this diff -- not just the
obvious hot-loop bodies -- answer the following. If any function is
omitted from the analysis, the audit is incomplete and must be re-run.

**Step 1: Enumerate callers.** For each new/modified function, grep
the codebase (`Grep -n "FunctionName\\("`) for callers. List every
caller and its own caller's caller until you reach a known entry
point.

**Step 2: Classify the hottest reachable call site.** For each
function, classify based on the hottest caller in its reachable call
graph:

- **HOT**: per-tick pump (`InstallGrabObservers`, harness pump, net
  pump, animation tick), per-`ProcessEvent` observer, per-frame loop,
  any caller firing at >= 1 Hz steady-state. INCLUDES `Install()` /
  `Setup()` / `Register()` functions called from such pumps -- a
  function being NAMED "Install" does NOT make it cold-path.
- **WARM**: per-connection, per-spawn, per-packet-of-rare-type, per-
  actor-`Init`, per-minute cycle, hotkey-triggered.
- **COLD**: genuinely one-shot (DLL_PROCESS_ATTACH, first session
  create, single boot path).

**Step 3: Compute steady-state per-call cost (call #2, #1000,
#1000000), not just call #1.** A function's "first-call" cost is
irrelevant if it's called from a HOT path; only the cost of call #N
in steady state matters. Specifically flag any of the following inside
a HOT-classified function body:

| Pattern | Why it's a hot-path bomb |
|---|---|
| `R::FindClass(name)` | Linear `GUObjectArray` walk (~1M entries) + `std::wstring` allocation per compared entry. See `src/votv-coop/src/ue_wrap/reflection.cpp:157`. ONE call = ~1M wstring allocs. |
| `R::FindFunction(cls, name)` | Same GUObjectArray walker; same per-entry wstring allocation. Does NOT climb `SuperStruct` -- subclasses that inherit the function return null; if your code retries on null, you have an infinite hot-path loop. |
| `R::FindObject(name)` / `R::FindObjectByClass(cls)` | Same GUObjectArray walker. |
| `std::wstring` / `std::string` / `std::vector` / `std::map` construction inside the body | Heap allocation per call. |
| `ProcessEvent` to engine UFunctions | Tens of microseconds per call; never on the per-frame path unless designed for it. |
| `std::mutex::lock()` / any mutex acquisition | Game-thread contention; long tails on lock-convoying. |
| `std::filesystem` / `fopen` / disk IO | Latency spike on every call. |
| Iterating a container > 100 elements | Frequently a sign of a missed cache / index. |

**Step 4: Idempotency latch check.** For every HOT-classified function
whose steady-state per-call cost is NOT O(1), is there an
`std::atomic<bool>` "done" latch at the TOP of the function body that
early-outs once first-success has been recorded? Pattern:

```cpp
std::atomic<bool> g_allInstalled{false};
void Install() {
    if (g_allInstalled.load(std::memory_order_acquire)) return;
    // ... do the work ...
    if (everything_succeeded()) g_allInstalled.store(true, std::memory_order_release);
}
```

If no such latch, the function is a hot-path bomb on repeated calls.
**FLAG CRITICAL.** Do not accept "but it's behind an `if` guard" as a
defence -- `if (!cls)` after `R::FindClass` does NOT short-circuit
the search; it only short-circuits the work after the search.

**Step 5: Failure-mode retry policy.** If any inner line can fail
(BP class not loaded yet, UFunction not resolved, peer not connected,
mutex not acquirable), does the function record success/failure such
that a subsequent call short-circuits the already-tried-and-failed
work? "Try again next tick" without a precondition gate IS a hot-path
bomb when the precondition never becomes true (e.g. a BP class that
never loads because the user is in main menu; a UFunction that doesn't
exist on this subclass).

**Step 6: UFunction-inheritance trap (UE4-specific).** If the diff
calls `R::FindFunction(subclassCls, L"FuncName")` and the subclass is
a BP variant (`*_erie_C`, `*_leaves_C`, `*_wetConcrete_C`, `*_variant_C`)
of a parent BP class, `FindFunction` will return null because it
doesn't climb `SuperStruct`. The function exists on the PARENT.
Register the subclass UClass into your routing table anyway (variant
dispatch) and rely on the PARENT's POST observer firing with `self` =
subclass actor.

### Cost we CAUSE but do not EXECUTE (MANDATORY -- added 2026-08-29)

Every counter this project owns times **our own instructions**. None of them counts the engine
work our code provokes, so an audit that only reads our timers can CLEAR a real regression.
Measured instance (2026-08-29): the input-ownership focus scan issued **~9,300 reflected
dispatches per second**, all inside ONE frame, and appeared in **no bucket at all** -- the
instrumented totals for that build summed to ~0.6 ms of a 14 ms frame while saying nothing
about it.

(This section first shipped citing a "the mod costs ~6 ms / 120 -> 70 fps" figure. That figure
was **withdrawn the same day**: the 50 fps was the developer machine's own tooling -- UE4SS's
bundled Lua mods plus the Blueprint paks `BPModLoaderMod` loads -- and Multivoid measured at no
detectable cost. Keep the mandate, drop the number; and note the second lesson that episode
taught, which belongs in any perf audit: **when a cost is claimed by comparing two environments,
diff the environments before instrumenting either one.**)

For every function in the table, answer:

1. **Does it call the engine?** A `reflection::CallFunction` returns only after ProcessEvent has
   run a whole blueprint on the game thread. Our timer sees the call; the frame pays for the
   script. Count the CALLS PER FRAME, not the microseconds our side spent issuing them.
   (Real instance: the input-ownership focus scan issued ~9,300 reflected dispatches per second
   and appeared in NO bucket.)
2. **Does it spawn or keep an actor/component alive?** The engine ticks, animates and renders it
   every frame afterwards, attributed to nobody.
3. **Does it land in ONE frame?** A 1 Hz posted task issuing thousands of dispatches is a
   periodic STALL, not a steady tax -- it shows as a p90 far above the median and as [HITCH]
   lines, and fixing it will NOT move steady fps. Say which of the two you are looking at.

**The measurement that settles it, and it should be the FIRST move rather than the last:**
`game_thread::SetTransparentBypass(ms)` makes our dispatch path inert while leaving our actors
and threads exactly as they are (`perf_probe_bypass=N` arms it). Comparing fps across that
window splits *what we run* from *what we put in the process* in one 5 s sample. Two supporting
discriminators, both console commands with zero hot-path code: `r.ScreenPercentage 25`
(CPU-bound vs GPU-bound) and `r.VSync 0` + `t.MaxFPS 0`, which MUST precede any `stat unit`
reading -- on a capped frame the game thread blocks on the sync and `stat unit` counts that
block inside `Game`, so every capped frame reads "the game thread is the bottleneck".

**And a hard constraint on the instrument itself:** anything added to the ProcessEvent path goes
INSIDE `ProcessEventDetourImpl`. The outer `ProcessEventDetour` frame sits OUTSIDE the
`RunDetourSEH` crash firewall; an instrument placed there hard-crashed the game on its first
boot, with a dump, in a rig that had never produced one.

### Output format (mandatory)

Provide a table covering every new/modified function:

| Function | File:Line | Hottest caller | Steady-state cost (call #N>1) | Idempotency latch | Retry policy | Verdict |
|---|---|---|---|---|---|---|
| Install | non_prop_entity_sync.cpp:547 | InstallGrabObservers (HOT, 125 Hz) | O(GUObjectArray) x 9 = ~9M wstring allocs/call | NO | unconditional retry every tick | CRITICAL FAIL |
| ApplyState | non_prop_entity_sync.cpp:570 | net dispatch (WARM, per-packet) | O(1) (GT::Post) | n/a | n/a | OK |

If the verdict column contains any CRITICAL FAIL, the audit
recommends ROLLING BACK or HOTFIXING before the user is asked to
test.

### Question the auditor must answer explicitly

> For every UFunction observer install, registry setup, or class-
> table population in this diff: what triggers re-entry on subsequent
> ticks, and is the work done per re-entry O(1)? If not O(1), where is
> the latch?

A diff that adds a new "install" / "register" / "setup" function
without that question being answered in writing is rejected by the
audit.

---

## Why this section is mandatory (don't strip it)

Per `[[feedback-audit-prompt-hot-path-reentry]]`: previous audit
prompts gave code-reviewer agents a list of focus areas
(correctness/threading/security/perf/MTA fidelity/file-size) without
forcing a mechanical per-function enumeration of the hot-path
property. Agents then reasoned about the obvious hot loops (POST
observers, per-frame ticks) and missed `Install()`-style functions
that LOOK one-shot but are wired into a pump. The mandatory
enumeration closes that gap.

## Where this fits with the other audit-prompt requirements

This section is ADDITIVE to the existing brief contents. The full
audit prompt should still include:

1. RULE 1 crutch hunt (workaround / filter / suppress-X / catch-and-
   ignore pattern detection).
2. RULE 2 baggage check (parallel old + new code paths, deprecated-
   but-kept stubs).
3. RULE 3 standalone check (no UE4SS runtime dependency leaked in).
4. MTA fidelity (against `reference/mtasa-blue/` precedent).
5. Threading invariants (engine UFunctions game-thread only).
6. Modular file-size check (soft cap 800 LOC, hard cap 1500 LOC per
   `[[feedback-modular-file-size-rule]]`; report post-change LOC for
   every touched file, propose extraction if > 800).
7. **THIS SECTION: hot-path re-entry analysis.**
8. Wire/trust-boundary validation (clamps, length limits, NaN guards
   on receiver-side payload application).
9. Correctness (does the code do what the comment claims).

If the audit-agent's report omits any of these sections, the audit is
incomplete; re-spawn with the missing section pasted in.

## Cross-refs

- `feedback_audit_prompt_hot_path_reentry.md` (memory) -- the rule
  that mandates this template.
- `feedback_install_idempotent_o1_steady_state.md` (memory) -- the
  underlying design rule the audit enforces.
- `feedback_modular_file_size_rule.md` (memory) -- the sibling audit-
  prompt requirement (file-size check).
- `CLAUDE.md` "After shipping, audit with agents" subsection.
- `src/votv-coop/src/ue_wrap/reflection.cpp:157` -- the
  `R::FindClass` definition (O(GUObjectArray) walker with per-entry
  wstring allocation; cited in Step 3 above as the canonical hot-
  path bomb pattern).
