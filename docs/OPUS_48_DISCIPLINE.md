# Working discipline for Opus 4.8 on VOTV_MP

Written 2026-07-06 by the Fable-5 session, at the user's request, for the period when
the project runs on Opus 4.8. Premise, stated plainly: you are running with less
reasoning headroom than the model that wrote most of this codebase's recent history.
That is FINE — this project's quality never came from raw cleverness; it came from
codified process. Your job is to run that process MORE strictly, not to compensate
with improvisation. Everything below is ranked by how often skipping it has burned
this project.

## 0. The prime directive: the docs are your working memory — USE them

Do not reason from scratch about anything this project has already learned. Before
ANY task: MEMORY.md index -> the READ-FIRST topic file -> CLAUDE.md -> the two
cross-cutting maps (COOP_DISPATCH_VISIBILITY, COOP_ENTITY_EXPRESSION_MAP) ->
COOP_SYNC_MAP (who owns which sync). This reading order exists because sessions that
skipped it re-derived known facts wrong. You have LESS margin for a wrong re-derivation
than the sessions that wrote these docs — so for you the reading order is a hard
precondition, not a suggestion.

## 1. Bias every decision toward SMALLER

- One axis / one defect / one lane per arc. Never batch two features into one diff.
- Build (Release! deploy-all ships Release) + deploy + hash-verify + smoke after EVERY
  functional change, not at the end of a stack of them.
- If a fix seems to require touching more than ~3 subsystems, STOP. Either you found a
  genuinely architectural root (write the analysis down, cite logs, let the user
  green-light "per rule 1") or — more likely at your capability tier — you
  misidentified the root. Re-derive from logs before proceeding.

## 2. Never trust your own inference where a measurement exists

This is the single biggest adjustment. Fable-5 sessions were repeatedly WRONG when
inferring instead of measuring — you will be wrong more often. Mechanically apply:

- **PROBE-DON'T-GUESS**: measure the failing moment (read-only probe / real logs)
  before building any fix. The v105 hand-item root was found by grepping the user's
  actual morning logs, not by theorizing.
- **Verify game-domain facts** against the SDK dump / bytecode disasm / wiki BEFORE
  asserting them. Tools: `research/bp_reflection/_cfg.py <asset> <UberName>` +
  `_fn.py <asset> <FuncName>` (cwd = repo root); kismet-analyzer converts pak assets.
- **Statuses are claims, not facts**: before repeating any OPEN/DONE/VERIFIED label
  from a doc, check the code/commit. Both directions.
- When your conclusion disagrees with a doc or a comment — the CURRENT CODE is the
  authority; read it, don't average the two.

## 3. The rules that are ALWAYS in force (no judgment calls needed)

These are mechanical — obeying them requires no intelligence, only discipline:

- RULE 1 no crutches (a filter/skip-if/suppress-X you're adding = STOP, find the root).
- RULE 2 no migration baggage (replaced code is DELETED same commit).
- RULE 3 standalone (UE4SS/IDA are dev tools; nothing of them ships).
- New ReliableKind = THREE places (enum+payload / family dispatcher / event_feed) + a
  kProtocolVersion bump + a COOP_SYNC_MAP row.
- Mirrors: brains OFF (tick/RNG/fuses/inputs) — a mirrored actor with a live brain
  acts on the WRONG machine (it will grab the viewer's local player, double-roll RNG,
  double-explode). This class of bug is subtle to debug and trivial to prevent.
- Cached actor pointers across ticks: IsLiveByIndex with the CAPTURED index, never
  bare IsLive (recycled slots).
- Engine UFunctions: game thread only. Raw field writes only for fields the game
  itself raw-writes; anything with a setter goes through the setter.
- Per-tick code: nothing that allocates or walks GUObjectArray in the steady state;
  latch on identity, re-check on a slow throttle if state can mutate in place.
- ini [dev] for flags; never bats/env. No AskUserQuestion UI — plain text. No emojis
  in files. Never git add -A (explicit paths; tools/mp.py + "kerfur skins icons/" are
  NEVER committed). Commit autonomously at verified checkpoints; ask before push
  unless the user's standing directive covers it.

## 4. The handoff checklist is not optional

CLAUDE.md "Pre-deploy / pre-handoff checklist" — run it verbatim, answer each item in
writing. "Build clean" is NOT evidence of anything but compilation. A smoke PASS is
NOT "verified" — only a hands-on or a matching real log earns VERIFIED. Use the exact
status ladder DESIGN -> AS-BUILT -> VERIFIED and never promote without evidence.
Forbidden phrases: "should work", "ready for test" without the checklist table.

## 5. When stuck: escalate by PROCESS, not by trying harder

- Two failed attempts at the same bug = the patch level is wrong. Stop. Re-derive the
  root from logs (the architectural-root rule).
- The ladder: reflection/dumps -> IDA MCP -> UE4SS probes. In that order.
- Spawn agents for SEARCH (mining SDK dumps, greps across the tree) and AUDIT (every
  shipped diff gets a code-reviewer agent — this is mandatory, and doubly so for you).
  NEVER design/architect agents (user rule 2026-06-30): design comes from code + docs
  + the MTA precedent, synthesized in the main context where the user can see it.
- If the least-crutch option is obvious — build it without asking. If two defensible
  architectures genuinely conflict — write both in 5 lines each, recommend one, ask
  in plain text. Do NOT silently pick on a coin-flip.

## 6. Honesty calibrated for a smaller model

- Say "I don't know" and "I did not verify this" freely. A wrong confident claim
  costs this project days (see the docs' history of REFUTED entries); an honest gap
  costs minutes.
- Report failures verbatim (log lines, error text), not summaries of what you think
  they mean.
- Never mark your own todo done on intention. The smoke ran or it didn't.
- Keep the user's Russian/English mix; answer technical substance in the language
  the user used.

## 7. Project-specific traps that WILL bite you (each burned a session)

- BP inner calls (EX_CallMath / EX_LocalFinalFunction) are INVISIBLE to ProcessEvent
  hooks. Check COOP_DISPATCH_VISIBILITY before designing any hook. When in doubt:
  POLL state + diff (the alarm/hand-item shape) instead of hooking verbs.
- `getMainPlayer` / `GetPlayerCharacter(0)` inside game BPs = the LOCAL player on
  whatever machine runs the code. Any mirrored BP logic touching it is a landmine.
- updateHold DESTROYS+RESPAWNS the hand actor per hotbar switch -- but the whole
  switch is ONE synchronous updateHold call (bytecode: destroy @935 + spawn @1023 +
  holding_name @3183 in the same invocation), so a per-tick poll never sees a
  mid-switch null: a polled null IS the stow. (The v105 first-cut 250ms debounce
  guarded a flicker that does not exist and read as pure lag -- removed 2026-07-06.)
- UE TArray<struct> stride = 16-aligned, not the raw Size.
- deploy-all ships Release — always build Release and hash-verify all 4 game folders
  before trusting a smoke.
- The smoke host slot s_1234 is stateful — restore coop_backup after killed runs.
- HighResShot in hands-on scenarios = toast spam; screenshots only in autonomous runs.
- User AT the PC = user tests; you never launch the game then.

## 8. Scope guard

Do not start: engine-level redesigns, new frameworks "for the future" (the
fix-then-generalize rule: N>=3 working cases before any abstraction), speculative
performance work without a [HITCH-SRC] profile, or any change to the versioning /
loader / reflection substrate — flag those for a Fable-5 session or explicit user
direction. The smart-items implementation (docs/items/ pattern) is designed and
ratified: when building it, follow docs/items/README.md §pattern + each item doc's
§3 LITERALLY — the design decisions are already made; your job is faithful assembly
with smokes at every step.
