# Should we switch the substrate to UE4SS's C++ API? — /qf decision base (WIP)

**STATUS (SECTION-SCOPED SUPERSEDE, 2026-07-26): the /qf converged at R26 ("that
holds") and the USER TOOK F1 (keep RULE 3) the same day. §4's draft conclusion
and §6's residual plan are SUPERSEDED by the decision record —
`docs/VERSION_MIGRATION.md` §11 (conclusion, trip-wires, drill evidence, the
TRIPWIRE-DECISION ledger). §3's measured table and §5's wire definitions REMAIN
the fact base that record cites.** Rounds 16-26 (the residual-action design +
the user's own three probes: ImGui/DX12, DLL-shape, mod-compat semantics) live
in the session transcript and are summarized in §11 itself. One correction to
R21's phrasing: the vendored `deps/first/Unreal` gitlink is PRESENT but EMPTY
(unresolvable → `Re-UE4SS/UEPseudo`, 404 under both org spellings, zero public
forks/mirrors GitHub-wide).

Companion fact base (already committed): `votv-ue4ss-coexistence-FACTS-2026-07-26.md`.

## 1. The question and where it came from

The user re-opened standing RULE 3 (standalone substrate; UE4SS = dev tool only)
after a public Discord critic argued: "switch everything over to UE4SS because it
does 99% of what you're doing already, just maintained by people who have been
doing it for years... when VotV ships a10 and the signatures break, 'owning it'
means an agent re-deriving offsets from IDA. That has worked for two months but I
heavily question whether it works at month 18, or if you step away."

(His other two points — VoidTogether credit and director-role transparency — were
already shipped in b128 and the site; they are not this decision.)

## 2. The forks

- **F1** keep RULE 3 (standalone) + the migration playbook.
- **F2** swap the loader/reflection/PE/UFunction pillars onto UE4SS's C++ API.
- **F3** vendor their engine lib as source (= maintain it ourselves).
- **F4** de-risking, NOT a gate: run the migration playbook as a drill on an old
  VOTV build to convert "untested playbook" into a measured cost.
- **F5** harden the engine half ourselves (second resolve MECHANISM). **WITHDRAWN
  at R11**: OPUS §11 forbids unprompted substrate work, no measured failure exists
  to close, and a hard fail is more honest than booting on a spare tire while the
  game half is broken anyway. Kept only as a note for the day a recook actually
  fails the HealthCheck (the cheap second mechanism is PE via Default__Object
  vtable slot 68 — measured from three independent sources).

## 3. What was MEASURED this pass (each with its check)

| Claim | Measurement |
|---|---|
| Swap scope | **2,404 LOC / 11 files** of 146,347 (**1.6%**), parts-sum verified twice per the xargs-truncation lesson. 144 of those (hook.cpp/.h) stay needed by the overlay regardless. §1 of VERSION_MIGRATION's existing "7,174 = 5%" is the SUBSTRATE size — a different denominator; the two must be linked, never shipped as rivals. |
| Lifetime maintenance | Across **1,282 commits**: pe_detour.cpp touched 3x; reflection.cpp 26x, **all 26 read and classified** -> 5 substrate repairs, ~15 API growth, 3 refactors, 3 creation. The 5 AOB signature constants were edited **once, at creation** (`d14f9c56`); zero AOB-breakage incidents. |
| Would a framework have absorbed those repairs? | **2 of 5, not 0** (self-correction at R12): UE4SS exposes `GetPropertyByNameInChain` (LiveView.cpp:737) — chain-walking at the API surface we would call — so `cf2c5250` (FindPropertyOffset walk) and `460da7e4` (setKey climb) come free with it. The other 3 (level-transition use-after-free `0889ef73`; stale-pointer/SEH IsLive `83ef1800`+`428177f2`) are our own pointer-cache lifecycle, framework-agnostic. |
| Our own gap | `reflection.cpp:420-431` `FindFunction` still does not walk the superclass chain (a standing project lesson) — the ONE measured area where their API is more mature than ours. Fixable in our own code. |
| Is F2 even buildable? | **CORRECTED 2026-07-26 (same day, user probe "check their wiki"): buildable via the self-service Epic↔GitHub linkage (their README:80-82 + docs + issue #577), NOT anonymously — the surviving blocker is STRUCTURAL (Epic-derived EULA-gated code: non-vendorable, kills public-clone reproducibility; see §11's wire-a ledger line).** Original anonymous measurement (still true): **No, for outsiders, as of 2026-07-26.** A UE4SS C++ mod builds only from the full source tree, whose `deps/first/Unreal` gitlink points at `git@github.com:Re-UE4SS/UEPseudo.git`; anonymous HTTPS `ls-remote` and the GitHub API both return **404 "Repository not found"**, while the same transport against `UE4SS-RE/RE-UE4SS` returns refs (positive control). Same failure on the stable v3.0.1 tag and on the tip. The release channel ships no C++ SDK: `zDEV-UE4SS_v3.0.1.zip` (166 entries) = configs, vtable/layout templates, signatures, UE4SS.dll + .pdb, **zero .h/.hpp, zero .lib**. Scope of this claim: that asset, that date — not "every channel ever". |
| What a recook actually breaks | The GAME half: 29 BP offsets + 235 content names + the 1,141 name-driven lookups' anchors — **no framework covers this**. UE4SS's team-maintained value is cross-ENGINE-version maps; VOTV has been UE4.27 its whole life (observation, not a dev statement). Zero recooks during the mod's life. |
| Game-offset premise (closed a §9 known-unknown) | Raw `+ 0x..` literals: **26 in coop/, 8 in ue_wrap/** (not ~136), and they are wire-struct/protocol parsing, not a third copy of game offsets -> "game offsets live in the two sdk_profile files" holds. |
| Our bus-factor half | A fresh `git clone --recursive` of the public origin resolves **every** gitlink (all public URLs); CI already does fresh checkout + submodule init + full compile on every build. This measures build reproducibility — **not** a successor's ability to carry the game half. |
| UE4SS project health | 50 contributors, commits daily; but the last **stable** is 2024-02-14 (2.5 years), v4 is rc/TBD, and the C++ core is private. |

## 4. The conclusion the pass reached (user has NOT accepted it)

Keep RULE 3; recommend **no substrate work**. The "99%" is measurably a 1.6%
pillar whose lifetime cost was 5 repair commits in 1,282 (2 of which a framework
would have absorbed), whose framework value targets engine-version churn this game
has never had, and whose C++ path is un-buildable for outsiders as measured today.

**The caveat belongs INSIDE that sentence, never a paragraph below it** (the
FEASIBILITY:25 crop failure): *the game has never been re-cooked during the mod's
life, and the migration playbook is untested.*

Honest residuals a substrate swap would NOT fix: the untested playbook, the
bus-factor question, and our property/function-lookup maturity gap.

## 5. Trip-wires (each one FLIPS the decision, or it does not belong)

- `git ls-remote https://github.com/Re-UE4SS/UEPseudo` succeeds -> the strongest
  leg of the F2 refusal falls; re-open the fork.
- A **non-prerelease** UE4SS release newer than v3.0.1 exists -> the "stable is
  2.5 years old" leg falls.
- The game's engine is no longer 4.27 (HealthCheck reads the exe) -> the only
  scenario where their cross-version maps pay for themselves.

Design decided at R10/R13: the gate fires on the **conditions** (verbatim
commands), not on a date; the date is only the stamp; the gate must be
forced-FAIL drilled. A fail-open CI WARN nobody is obliged to read was DROPPED.

## 6. Residual actions (next session — the pass is unfinished)

1. **Sweep FEASIBILITY.md 71 / 86 / 94 / 126** — they still read "via UE4SS" as
   the current runtime mechanism in the exact public doc the critic quoted
   (line 30 is already annotated). Classifier known-positive-validated.
2. ~~**RULE 2 on the version docs**~~ **DONE 2026-07-26 (documentize):**
   `VERSION_PORTABILITY.md`'s two unique sections were migrated into
   `VERSION_MIGRATION.md` §10 (with the `multivoid-compat-report.txt` rename
   fixed), then the file moved to `docs/_archive/` with a pointer banner.
   The §9-before-§8 print order was fixed in the same pass.
3. **Land the record** — the /qf conclusion still needs a home in
   `VERSION_MIGRATION.md` (now as §11, since §10 is taken) once the user decides.
4. **Reachability**: one-line pointers from RULE 3 (CLAUDE.md), README and the
   site Q&A — the critic arrived from a public surface, so the record must be
   reachable from the surface he reads. Same commit as the record.
5. **Bind the trip-wires to a ritual that already runs** (RELEASE.md step or the
   `/documentize` checklist) — an unattached check fires only if a human
   remembers §10, which is the artifact class this whole record exists to avoid.
6. **F4 honesty**: no old VOTV build is on disk (measured); until one is
   obtainable the drill does not run and the playbook stays unproven — an
   accepted risk, not a closed item. Pre-named threshold: >3 working days back to
   a green smoke after a recook = "unbearable".
