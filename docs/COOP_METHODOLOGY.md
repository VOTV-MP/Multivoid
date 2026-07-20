# Coop-mod methodology: adding multiplayer to a single-player game

**Audience**: engineer (human or Claude) tasked with retrofitting
multiplayer / coop onto an existing single-player game without
modifying its files.

**Status**: distilled from MTA:SA precedent (22+ years of retrofitting
multiplayer onto GTA SA via hooks) and the Meet the Robinsons coop
mod (2026-05-04..2026-05-13). Battle-tested against both.

**How to use this document in a new project**:
1. Drop it into the new project's `docs/` directory.
2. Add a reference in `CLAUDE.md` that says: *"For any coop / multiplayer
   architecture decision, consult `docs/COOP_METHODOLOGY.md`."*
3. Read it in order before doing any coop work. Each phase has hard
   gates — don't proceed until the gate is met.

---

## Top-level rules

### RULE №1 — No crutches, no quick fixes

Always pick the proper root-cause fix. No "good enough", no "we can
fix later", no shortcuts. Weeks or months of work to do it properly is
OK.

When you find yourself adding a workaround (filter, skip-if,
suppress-X, catch-and-ignore), **STOP**. That is a crutch. Identify the
root cause and fix it at the point where the actual problem is, not
broadly.

### RULE №2 — No migration baggage

When a feature is replaced or retired, the old code goes. Fully,
immediately. No "deprecated but kept for now", no feature flags for
old behaviour, no parallel old + new code paths.

**Concrete triggers** (rule violations):
- `// deprecated, kept for now` / `// TODO: remove when X` comments.
- Cmdline flags that exist only to re-enable old behaviour.
- Two implementations of the same concept compiled together.
- Type aliases / re-exports kept "for compatibility".
- Stub functions that exist solely to satisfy old callers.

The two rules complement each other: RULE №1 forbids shortcuts forward
(build it properly even if slow); RULE №2 forbids shortcuts sideways
(don't keep the old broken thing alive next to the new proper thing).

---

## The 7 architectural principles

These are the core invariants. Every non-trivial coop architecture
decision must be checked against ALL of them. They are NOT independent
— they reinforce each other.

### 1. No modification of original game files

Hooks, prologue patches, MinHook / Detours, vtable patches, runtime
memory patches — yes.

Editing the game's `.exe`, `.dll`, asset files, scripts on disk — **no**.

Why: every modification compounds. The moment you start editing assets,
you fork the asset pipeline. The moment you edit the .exe, you fork the
binary. The mod becomes a redistribution of the game, not a mod.

### 2. Engine-extension paradigm

Treat your mod as **a new engine on top of the existing engine**, not
"a few hooks". Modules own their lifecycles. Clean APIs between
subsystems. Each subsystem stays behind well-defined boundaries with
the base engine.

Anti-test: if your code is 50 hooks in one file calling each other via
file-scope globals, you do NOT have an engine-extension. You have an
opportunistic patch collection.

### 3. Parallel class hierarchy mirroring engine structures

The single biggest pattern from MTA. For coop:

- Your code owns a `RemotePlayer` (or `MtrRemotePlayer`, naming
  doesn't matter) — your class, with: network state, interpolation
  buffer, input buffer, ownership of the engine-side entity pointer.
- The **engine class** owns rendering, animation, physics — keep it.
- They are connected by a pointer: `MyRemote::m_pEngineEntity ->
  EngineEntity*`.
- MTA's exact shape: `CClientPed::m_pPlayerPed -> CPlayerPed*`.
  `CClientPlayer` is a subclass of `CClientPed` for the remote player
  case.

Why this works: the engine entity does the heavy lifting (50,000+
lines of animation, collision, rendering). Your class does only what
the engine doesn't know about (network, interpolation, ownership).

Why "spawn a second engine entity for P2" works: the engine already
supports more than one of its protagonist class — usually it's
single-instance only because the game only spawns one. The factory
function works fine with multiple invocations. Find the factory; call
it with the right args; you have an "orphan" P2 entity. (Term "orphan":
the entity exists in the engine but the engine doesn't know about it
through its normal lookup tables.)

### 4. Targeted crash fixes, not broad suppression

When you find a crash caused by a single-player assumption (NULL
checks, asserts that fire on the orphan, lookups that return the
wrong entity), you must:

- Find the SPECIFIC call site.
- Patch THAT call site (a per-site route, an init the engine forgot,
  whatever).
- **Never** add a broad mechanism like "filter out all candidates that
  look like our orphan" or "skip the engine's check entirely".

The MTR project shipped a `coop_orphan_filter` in 2026-05-12 and had
to retire it the same week because it masked 6+ different crashes
behind one broad mechanism. Each crash had its own root cause; the
filter was hiding all of them. Proper fix: replaced with per-site
patches (a registry mirror + per-site route per offending function).

**Transitional-crutch exception**: you MAY install a broad-suppression
mechanism temporarily IF you simultaneously write a retirement plan
that lists: (a) each concrete crash it masks, (b) the targeted fix for
each, (c) the gating criteria for retirement. The plan must be
executed within a small number of sessions, not "eventually".

### 5. Minimum viable subset

Define coop scope explicitly. Write a `COOP_SCOPE.md`. Anything not in
that doc is NOT replicated.

Without this discipline, every new feature triggers "should we
replicate X?" → "well, maybe, kinda, depends" → scope creep → coop
never ships.

With this discipline, every new feature has a clear binary answer.
"It's in scope" → replicate. "It's not in scope" → don't.

**Update scope when you find a gap**: living document. The MTR scope
doc has been amended several times when live testing revealed an
in-scope thing was missing (NPCs originally "out of MVP", then
amended to "in scope under Phase 3.x" when boss fights were
considered). Documented amendments preserve the audit trail.

### 6. Augment SP, never replace it

Coop is layered ON the single-player game, not "instead of". Where
coop logic meets SP logic, prefer **per-player routing inside the SP
system** over bypassing SP wholesale.

Why: every player will encounter SP content first (the game ships as
SP). SP must keep working. A coop session is just "SP with a second
player attached".

Practical implication: when you see a per-player thing in the engine
(input mapper, camera state, save slot), don't replace it. ROUTE it
per-player: P1 path stays as-is, P2 path is added alongside.

### 7. Engine-wrapper layer vs gameplay/network layer

Two completely separate code subtrees:

- **Engine-wrapper layer** (`src/engine_wrap/`, `Client/game_sa/` in
  MTA, etc.) — one C++ class per engine class. Wraps engine VAs,
  struct layouts, vtable thunks. **No network logic. No gameplay
  logic. No coop state.**
- **Gameplay / network layer** (`src/coop/`, `Client/mods/deathmatch/`
  in MTA) — owns network packets, interpolation, input buffers,
  scripting, your `MyRemotePlayer` class. Talks to the engine
  through the engine-wrapper layer.

They communicate via **clean APIs**, not shared globals.

Concrete trigger for a principle 7 violation: a new file that BOTH
dereferences engine memory addresses AND owns network state. Split
it into two files in two subtrees.

Why this matters: the engine-wrapper layer is read-only on the
engine (you describe what's there). The gameplay layer is yours to
evolve. The boundary is what lets either side change without breaking
the other.

---

## Phase 0: feasibility assessment

**Gate**: do not start Phase 1 until you can answer ALL of these.

### 0.0 Clone the MTA:SA reference (MANDATORY first step)

Before any other Phase 0 work, clone Multi Theft Auto: San Andreas
source into your project as a vendored reference:

```bash
git submodule add https://github.com/multitheftauto/mtasa-blue.git reference/mtasa-blue
```

(Or `git clone` if you prefer not to use submodules — but vendoring
ensures the precedent is pinned to a known commit.)

**Why this is mandatory**:
- MTA:SA is 22+ years of retrofitting multiplayer onto a single-
  player game (GTA: San Andreas) via hook-only mods. It is **the**
  canonical real-world precedent for this entire methodology.
- Every architectural pattern in this doc has a direct MTA analogue:
  the parallel class hierarchy, the engine-wrapper / gameplay split,
  the keysync packet, the ped-pure-sync packet, the session model,
  the host-authoritative AI, the cutscene sync — all of it.
- When in doubt about how to structure something, the answer is
  "look at how MTA does it". This is not slavish imitation — MTA
  made the mistakes already and the survivors are battle-tested
  designs.

**Where to look** (skim before starting Phase 1):
- `Client/game_sa/` — engine-wrapper layer. One C++ class per GTA
  engine class. No network logic. This is principle 7 in
  practice.
- `Client/mods/deathmatch/logic/` — gameplay / network layer. Owns
  network packets, scripting, interpolation, `CClientPed`,
  `CClientPlayer`. Talks to `game_sa` via clean APIs.
- `Client/mods/deathmatch/logic/CClientPed.cpp` — the parallel-
  class-hierarchy reference. Read end-to-end before designing your
  `RemotePlayer` class.
- `Client/mods/deathmatch/logic/CNetAPI.cpp` and the `*_pure_sync`
  packet files — wire format precedent for input replication and
  per-entity state.

**Phase 0.0 deliverable**: `reference/mtasa-blue/` exists in your
repo. `CLAUDE.md` references it as "the canonical precedent for
coop architecture decisions".

### 0.1 Repository bootstrap (directory layout + initial files)

A coop-mod project accumulates: source code (mod), research findings
(RE artifacts), design docs, third-party deps, tooling, and game-
install scratch. Each kind has a different lifetime and audience.
Setting up the layout BEFORE writing code prevents the "everything in
one folder" mess that becomes impossible to navigate after a few
weeks.

#### 0.1.1 Directory layout

Create this structure at the repo root:

```
<project-root>/
├── CLAUDE.md                       # Project rules (RULE №1, RULE №2,
│                                   # 7 principles, references to docs)
├── README.md                       # Public-facing what-is-this
├── .gitignore                      # See template below
├── .gitmodules                     # Submodule definitions
│
├── docs/                           # Stable design documentation
│   ├── ARCHITECTURE.md             # System architecture overview
│   ├── ROADMAP.md                  # Phase plan + milestones
│   ├── COOP_SCOPE.md               # In-scope / out-of-scope (Living)
│   ├── COOP_METHODOLOGY.md         # This file (drop in from MTR repo)
│   ├── FEASIBILITY.md              # Phase 0 deliverable
│   └── AUTONOMOUS_TESTING.md       # Test harness usage
│
├── research/                       # RE artifacts (analyst-readable)
│   └── findings/                   # One markdown per finding, dated
│       ├── <phase>-<topic>-<YYYY-MM-DD>.md
│       └── ...
│
├── reference/                      # Vendored read-only references
│   ├── mtasa-blue/                 # Submodule (MTA precedent)
│   └── <other-refs>/               # PDFs, papers, decompile dumps
│
├── src/                            # Mod source code (yours)
│   └── <mod-name>/                 # e.g. mtr-asi, gta-coop-mod
│       ├── CMakeLists.txt
│       ├── README.md
│       ├── include/<mod-name>/     # Headers
│       │   ├── <engine-wrap>/      # Principle 7: engine-wrapper layer
│       │   └── coop/               # Principle 7: gameplay/network layer
│       ├── src/                    # Implementation
│       │   ├── <engine-wrap>/
│       │   └── coop/
│       └── third_party/            # Submodules: minhook, imgui, etc.
│
├── third_party/                    # Project-wide submodules
│   ├── dxvk/                       # If translating D3D9 to Vulkan
│   └── <other-deps>/
│
├── tools/                          # PowerShell / Python scripts
│   ├── README.md
│   ├── run-test.ps1                # Single-process autonomous test
│   ├── run-coop-test.ps1           # Dual-process LAN test
│   └── <build-helpers>/
│
└── Game/                           # GITIGNORED. Local game install.
    │                               # Force-add only scripts:
    ├── <Game>.exe                  # (gitignored)
    ├── <mod>.asi                   # (gitignored — built artifact)
    ├── coop-host.bat               # force-added (text script)
    └── coop-client.bat             # force-added (text script)
```

**Optional but recommended**:
```
ida/                                # IDA Pro project files (.i64
                                    # gitignored, but folder structure
                                    # is collaboration-relevant for
                                    # pre-RE'd function name exports)
└── exports/                        # Function name lists, struct
                                    # definitions — commit these
```

#### 0.1.2 What goes where (decision flow)

When you produce a new artifact, decide its home by what KIND of
artifact it is:

| Artifact kind | Goes to | Lifetime |
|---|---|---|
| RE finding (single function / single struct / single discovery) | `research/findings/<topic>-<date>.md` | Snapshot — frozen at write time, supersede with new dated file |
| Design / architecture doc | `docs/<TOPIC>.md` | Living — kept in sync with reality |
| Scope decision (in / out of scope for coop) | `docs/COOP_SCOPE.md` | Living — append to existing sections |
| Phase plan (what comes next) | `docs/ROADMAP.md` | Living — re-arranged as priorities shift |
| Project rule (always-on guidance) | `CLAUDE.md` | Living — add only after a real incident teaches the rule |
| Code (header) | `src/<mod>/include/<mod>/<subsystem>/X.h` | Living |
| Code (impl) | `src/<mod>/src/<subsystem>/X.cpp` | Living |
| Tool (PowerShell / Python) | `tools/X.{ps1,py,sh}` | Living |
| Test harness scenario | Within the mod's test_harness module + a `docs/AUTONOMOUS_TESTING.md` line | Living |
| Vendored library (with source) | `third_party/X/` (submodule) | Pinned to a known commit |
| External read-only reference (PDF, dump) | `reference/X.pdf` | Static, rarely changes |
| Built artifact (DLL, EXE, ASI) | DO NOT COMMIT (`.gitignore`) | Regenerable |
| Test output (JSON reports, screenshots) | DO NOT COMMIT (`.gitignore`) | Regenerable |
| User-specific config | DO NOT COMMIT (`.gitignore`) | Per-machine |

**Findings vs. design docs — the distinction matters**:

- `research/findings/` is **append-only history**. Each finding is a
  snapshot at a point in time. You don't edit old findings; you write
  a new one that supersedes (with a note linking back).
- `docs/` is **living truth**. You edit them as understanding evolves.
  If `ARCHITECTURE.md` says X and X is no longer true, you EDIT it,
  not append.

This split lets new contributors read `docs/` to learn the current
state and `research/findings/` to understand HOW the current state
was reached. The MTR project has ~50 findings under
`research/findings/coop-*` — each a snapshot of a specific
investigation moment.

**Why dated filenames in findings**:
- Two findings on the same topic written months apart MUST coexist
  (the later supersedes the earlier; both are kept for audit).
- The date gives you a quick "when was this written" without git
  blame.
- Sort order in the directory listing tells you investigation
  chronology at a glance.

Pattern: `<phase>-<topic>-<YYYY-MM-DD>.md`. Examples:
- `coop-phase-0a-entity-factory-2026-05-10.md`
- `coop-phase-1-6-input-routing-2026-05-12.md`
- `dt-correctness-root-cause-2026-05-07.md`

#### 0.1.3 `.gitignore` template

Copy this into your repo as `.gitignore`. Adjust the game-binary
extensions for your specific game.

```gitignore
# ===== Game install — never commit copyrighted binaries =====
Game/
*.exe
*.dll
*.bik       # Bink Video (adjust per game)
*.bnk       # Wwise (adjust per game)
*.dbl       # MTR-specific data files (adjust per game)
# ... add your game's asset extensions here ...

# ===== IDA Pro working files (per-user, large, regenerable) =====
*.i64
*.idb
*.id0
*.id1
*.id2
*.nam
*.til
*.t01
*.t02

# ===== Inspection scratchpad (per-user) =====
_inspect/

# ===== Heavy local working data (per-machine, not collab-relevant) =====
ida/dumps/
*.7z
tools/x64dbg_snapshot*/
__pycache__/

# Autonomous test run artifacts (regenerable, can be gigabytes)
tools/test-runs/

# Build outputs of vendored deps (regenerable from submodule + build script)
third_party/*-build/

# ===== Build output =====
build/
out/
bin/
obj/
*.obj
*.lib
*.exp
*.pdb
*.ilk
*.ipdb
*.iobj
*.asi

# ===== CMake =====
CMakeCache.txt
CMakeFiles/
cmake_install.cmake
*.cmake.in
compile_commands.json

# ===== IDE =====
.vs/
*.user
*.sln.docstates
*.suo
*.userprefs
.idea/
.vscode/
!.vscode/launch.example.json
!.vscode/tasks.example.json

# ===== OS junk =====
Thumbs.db
ehthumbs.db
.DS_Store
desktop.ini

# ===== Temporary / log =====
*.log
*.tmp
*.bak
*.swp
*~

# ===== Claude Code per-machine config (MCP paths etc.) =====
.claude/

# ===== Allow-list (override broad bans for our own outputs) =====
!src/**/*.dll
!tools/**/*.exe
```

**Critical**: the `Game/` exclusion is broad to prevent ANY
copyrighted asset from being committed. Scripts that LIVE in Game/
(`coop-host.bat`, `coop-client.bat`) must be **force-added** with
`git add -f Game/coop-*.bat` because they `cd /d "%~dp0"` and need
to launch from the install location.

#### 0.1.4 Submodules to register

Initial submodules every coop-mod project needs:

```bash
# MTA precedent (mandatory — see 0.0)
git submodule add https://github.com/multitheftauto/mtasa-blue.git \
    reference/mtasa-blue

# Hooking library (mandatory)
git submodule add https://github.com/TsudaKageyu/minhook.git \
    src/<mod>/third_party/minhook

# Immediate-mode GUI for mod menu / debug overlays (mandatory)
git submodule add https://github.com/ocornut/imgui.git \
    src/<mod>/third_party/imgui

# Optional but common:
# - D3D9-to-Vulkan translation for old games on modern hardware
git submodule add https://github.com/doitsujin/dxvk.git \
    third_party/dxvk
```

**Pin to known commits.** After adding, immediately `git -C <path>
checkout <commit>` to a tested version. Don't track upstream HEAD —
breaking changes ship without warning.

#### 0.1.5 Initial `CLAUDE.md` template

Create `CLAUDE.md` at the repo root with this skeleton. Customize
the bracketed parts:

```markdown
# Project rules for Claude

## RULE №1 — No crutches, no quick fixes
[Copy verbatim from docs/COOP_METHODOLOGY.md "Top-level rules"]

## RULE №2 — No migration baggage
[Copy verbatim from docs/COOP_METHODOLOGY.md "Top-level rules"]

## The 7 architectural principles
[Copy from docs/COOP_METHODOLOGY.md, can be condensed]

## Other standing rules
- Document findings + rename functions in IDB
- Verify, don't guess
- Don't reinvent the wheel
- The mod menu uses Dear ImGui
- No emojis in code / files unless explicitly requested

## Reading order after a session reset / new conversation
1. MEMORY.md index (if using auto-memory).
2. Top entry of memory/ (latest project state).
3. CLAUDE.md (this file).
4. docs/COOP_METHODOLOGY.md for architecture decisions.
5. reference/mtasa-blue/ for precedent.
6. docs/COOP_SCOPE.md for scope decisions.
```

The CLAUDE.md is read on every conversation start (Claude Code
auto-loads it). Keep it concise — long CLAUDE.md eats context
budget that should go to actual work.

#### 0.1.6 Initial `docs/` files

Create these as skeletons at bootstrap:

- **`docs/ARCHITECTURE.md`**: one paragraph "what is this mod
  architecturally" + a placeholder for diagrams + a list of the
  7 principles in summary form. Will grow as you go.
- **`docs/ROADMAP.md`**: ordered list of phases (Phase 0 ..
  Phase N). Each phase has: goal, deliverable, status, dates.
- **`docs/COOP_SCOPE.md`**: skeleton with empty "In scope" and "Out
  of scope" sections. Populate as scope is decided. Add a header
  block: *"Anything not listed here is NOT in scope and should not
  drive code or replication decisions."*
- **`docs/COOP_METHODOLOGY.md`**: this file. Copy from the MTR repo
  unchanged.
- **`docs/FEASIBILITY.md`**: the Phase 0 deliverable. Will be filled
  in as you answer 0.2 through 0.8 below.

#### 0.1.7 Memory system setup (if using Claude Code)

Claude Code's auto-memory lives **outside** the repo, in:

```
~/.claude/projects/<flattened-project-path>/memory/
```

(`<flattened-project-path>` is the absolute path with separators
swapped: `d--Projects-Programming-MeetTheRobinsons` for example.)

Structure inside `memory/`:
- `MEMORY.md` — index file, ~150-char-per-line pointers
- `project_<topic>.md` — project-state memory entries
- `feedback_<topic>.md` — collaboration feedback memory entries
- `reference_<topic>.md` — pointers to external references

**Do NOT commit memory to the repo.** It's per-user, per-machine,
and contains user-specific phrasings. The repo holds AUDITABLE
state (code, docs, research findings); memory holds COLLABORATION
state (preferences, patterns, working context).

If you want a colleague to have the same memory entries, write
shareable parts into `docs/` instead.

#### 0.1.8 Initial commit + push

The first commit should contain ONLY the skeleton:

```bash
git init
# Create the layout above with empty / skeleton files
git add CLAUDE.md README.md .gitignore .gitmodules \
        docs/ARCHITECTURE.md docs/ROADMAP.md docs/COOP_SCOPE.md \
        docs/COOP_METHODOLOGY.md docs/FEASIBILITY.md \
        src/<mod>/CMakeLists.txt src/<mod>/README.md \
        tools/README.md
git commit -m "Initial skeleton: docs + .gitignore + submodule pins"
git push -u origin main
```

The submodule clone happens via `git submodule add` BEFORE the
commit; the .gitmodules + submodule pointers go in the same first
commit.

**Don't commit code yet.** First commit is the project shape, not
the first hook. Code comes in commits 2+.

#### 0.1.9 Commit conventions for the project

Adopt early:
- **One topic per commit.** Don't bundle "renamed two functions" +
  "added a network packet". Each commit gets a one-line title +
  body explaining the WHY.
- **Phase tag in commit title** where applicable: `[Phase 1.6]`,
  `[RE]`, `[docs]`, `[tools]`.
- **Cross-reference findings**: if a commit implements something
  documented in a finding, mention the finding file in the body.
- **Co-author Claude** if Claude wrote substantial portions:
  `Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>`.

These aren't enforced; they're conventions. The point is: future-you
or a new contributor should be able to read `git log --oneline` and
follow the project's evolution without reading the diffs.

#### 0.1.10 Bootstrap checklist

Don't proceed past Phase 0.1 until ALL of these are TRUE:

- [ ] Directory layout matches 0.1.1 (or close — adapt for your mod
      name).
- [ ] `.gitignore` excludes game binaries, build outputs, test
      artifacts, user-specific configs, IDA working files.
- [ ] `reference/mtasa-blue/` submodule cloned + pinned (0.0).
- [ ] `src/<mod>/third_party/{minhook,imgui}/` submodules cloned +
      pinned.
- [ ] `CLAUDE.md` has the 2 rules + 7 principles + reading order.
- [ ] `docs/COOP_METHODOLOGY.md` copied from MTR repo (this file).
- [ ] `docs/COOP_SCOPE.md` exists as skeleton.
- [ ] `docs/FEASIBILITY.md` exists (will fill in below).
- [ ] First commit pushed: skeleton + submodule pointers.
- [ ] Memory system set up (`MEMORY.md` index in
      `~/.claude/projects/.../memory/`) if using auto-memory.

### 0.2 Is the binary unpacked?

- Modern AAA games with Denuvo / VMProtect / Themida / IL2CPP =
  effectively impossible without major effort. Don't try.
- DRM that's already cracked by the scene (old games, ~2000-2012
  era) = trivial. The cracked binary is what your mod hooks.
- Games with no DRM = easiest.

If the binary is encrypted at runtime and you can't dump a clean
post-unpack version, **stop here**. The project is not viable.

### 0.3 What's the rendering API?

- DirectX 8 / 9 = mature hooking ecosystem (D3D9 hook for menu
  overlay, ImGui, etc.).
- DirectX 10/11/12 = harder but doable.
- Vulkan / Metal = harder; native overlay tools exist.
- OpenGL = mostly DX9-era; doable.

You need to render YOUR overlay (mod menu, debug, HUD). Pick a
strategy here.

### 0.4 What's the input API?

- DirectInput (DI) = old games. Has cooperative-level quirks
  (`DISCL_EXCLUSIVE | DISCL_FOREGROUND` won't allow background poll).
  Plan for DI hooks.
- Raw Input / XInput = simpler.
- WinAPI WndProc only = simplest; just hook the game's wndproc.

You need to inject input (mod menu navigation, synthetic actions for
autonomous testing). Plan for this.

### 0.5 What's the entity model?

Reverse-engineer the engine's entity/object system to confirm:
- Is there a clear entity factory? (One function that creates
  entities by class name / ID.)
- Is the player a distinct class, or implicit?
- Can you call the factory to spawn a second player-class entity?
  (Try it from your mod: spawn the protagonist class as an extra
  entity; see if it ticks alongside the real one without crashing
  immediately.)

This single test ("can I spawn a second protagonist?") is the
cheapest derisk of the entire project. If it crashes irrecoverably,
the project is much harder. If it ticks for a few seconds and then
crashes, you have hooks to write. If it ticks indefinitely (rare),
you have a gift.

### 0.6 What's the script VM?

If the game has a scripting layer (`.sx` in MTR, `.scm` in GTA SA,
`.gsc` in CoD), the AI / NPCs are likely driven by scripts. This
matters for entity replication design — script runs on the host,
client receives state.

If the game has no scripting and NPC logic is hardcoded in the
engine, replication is simpler but engine RE is harder.

### 0.7 What's the save format?

Saves encode the "world state": which entities exist, their progress,
inventory, unlock flags. Coop should reuse the host's save (host-
authoritative). Confirm:
- Are saves a single file with known layout?
- Can you load a save programmatically (cmdline flag, public API)?
- Are saves encrypted? (If yes, you'll need to decrypt to read host
  state.)

### 0.8 Is there per-process or per-machine state?

Some games have a per-machine mutex (Steam license check, DRM,
single-instance enforcement). For LAN coop on the same machine
(testing), you may need to bypass this. For LAN coop across machines,
this doesn't matter.

**Phase 0 deliverable**: a `docs/FEASIBILITY.md` answering each of the
above. If any are "blocking", document why and decide whether to
proceed.

---

## Phase 1: engine archaeology

**Goal**: produce a research finding for each of the following. These
are the engine entry points you'll hook in Phase 2+.

**Gate**: do not start Phase 2 until you have ALL of these documented.

### 1.1 Entity factory

The function that creates any entity by class name or ID.

How to find:
- Look for places that call `new ClassName` or equivalent in
  decompilation.
- Search for the constructor of the player class; follow xrefs
  backward to find what calls it.
- Look for strings that match entity class names (`"protagonist"`,
  `"enemy"`, `"npc_dialog"`, etc.) in the binary. The factory
  reads these.

Document: function VA, signature, calling convention.

### 1.2 Player class layout

The entity-class instance representing the protagonist. Map its
fields:
- Position (`+0x00..0x0C` typically).
- Velocity.
- Orientation (quaternion / euler).
- Animation state.
- Inventory pointer.
- Active weapon ID.
- Health.
- Mode (walking / driving / climbing).

Document offsets in a research finding.

### 1.3 Input dispatch path

The function that reads the current frame's input state (button bits,
analog values) and uses it to drive the player.

How to find:
- Set a breakpoint on `DirectInputDevice::GetDeviceState`. Follow
  the return value's consumer through the engine.
- Or, set a write-watchpoint on the player's velocity field; the
  call stack at the write site includes the input handler.

This is critical for Phase 2.0 input replication.

### 1.4 Sim tick

The function that runs the per-frame world simulation (move
entities, update AI, run scripts, resolve physics).

Document the call tree: main loop → sim_tick → sub-ticks (physics,
AI, script, animation).

### 1.5 Render tick

Separate from sim tick. The function that renders the current frame.

Important: sim and render may be decoupled (variable Hz) or coupled
(60Hz lock-step). This matters for interpolation design.

### 1.6 Level-load entry + completion callback

The function that loads a level by name. The callback that fires
when level-load is complete.

You need both: to trigger a level transition (loading host's level
on client) and to react when the load finishes (re-spawning the
orphan, resetting latches).

### 1.7 Save / load entry

The function that loads a save file. Ideally callable via cmdline
flag or with a small wrapper.

Used to start coop sessions: host loads their save, client connects,
client's engine loads the same save, both are now in the same world
state.

### 1.8 Screen / UI system

How the game shows menus, dialogs, HUDs. Most games have a "screen
stack" — push a screen, it gets input + render time, pop it to
return to the previous one.

Find: the screen-push function. The list of all screens. Each
screen's vtable (init / tick / render / handle_input / destroy).

### 1.9 Script VM (if present)

For scripted games, find the VM entry point, the command parser, the
list of registered commands.

You'll hook command-level events (`cutscene_play`, `trigger_zone`,
`npc_spawn`) to know what the engine is doing.

---

## Phase 2: foundation infrastructure

**Gate**: do not start Phase 3 until you can spawn an orphan second
protagonist and have it tick without crashing for at least 60 seconds.

### 2.1 Spawn the orphan

Call the entity factory (1.1) to spawn a second instance of the
player class. Do NOT add it to the engine's "main player" lookup
(it's an orphan — managed by your code, not the engine's normal
entity manager).

This will likely crash immediately. Don't panic; this is normal.

### 2.2 Crash-by-crash root-cause fixes

For each crash:
- Capture the call stack at the crash.
- Identify the offending function and the actual root cause (null
  pointer in a field the orphan didn't init? lookup table missing
  an entry? hardcoded P1-only assumption?).
- Patch the specific call site or initialize the missing state.

**Do not write a filter / suppression / skip mechanism** to make the
crash go away. That violates principle 4.

This is the bulk of Phase 2 work. Expect dozens of crash sites. Each
gets its own RE + fix + verification.

### 2.3 Registry / state mirror

Many engines have a per-entity "subscriber list" or "registry" that
the engine populates for the main player. The orphan needs its own
registry, populated identically.

In MTR, this was the `coop_registry_mirror` — 21 subscriber keys at
specific offsets that the movement / collision / health subsystems
look up. Without mirroring, those subsystems crash on the orphan.

Find the engine's "for each player, set up these registry entries"
function. Hook it; when called on the orphan, populate the
identical state.

### 2.4 Sustained-soak validation

Run the orphan ticking alongside the real player for at least
several minutes. Walk around (real player's movement). The orphan
sits in idle pose but its sim runs.

Watch for: late crashes (something that only crashes on
animation-transition / weapon-switch / scripted-event), memory
leaks (allocation in orphan's tick that isn't freed), state drift
(orphan ends up at impossible positions).

Each issue → per-site root-cause fix.

**Phase 2 deliverable**: orphan ticks indefinitely without crashing,
in idle pose, while the real player plays normally. No filters or
suppressions installed.

---

## Phase 3: networking transport

### 3.1 UDP, host-authoritative, LAN-first

- UDP (not TCP). Game state is real-time; lost packets are stale
  faster than retransmits would help.
- Host-authoritative (one side owns the truth). The client receives
  state and sends input + commands. NEVER the reverse.
- LAN first (low latency, simple, no NAT, no internet topology). WAN
  is a the public-server phase concern.

### 3.2 Sessions, not connections

A session is a host listening on a port + zero or one clients
connected. Not a connection-pool, not a server. Sessions have:
- A peer (the other side's address + port).
- A connection state (disconnected / handshaking / connected).
- A per-session sequence counter (for ordering / replay protection).

### 3.3 Pure I/O at the bottom

The transport layer is **pure I/O**. It sends bytes, receives bytes.
It does NOT know what game data is.

Above the transport sits the packet serialization layer: turns
"PoseSnapshot { x, y, z, yaw }" into bytes; turns bytes back into a
struct.

Above that is the application layer: routes packets to their
handlers, manages send rate, etc.

This 3-layer split is principle 7 applied to networking.

### 3.4 Position-only first

Ship the simplest possible packet: pose (x, y, z, orientation) at
~13-30 Hz. The receiver interpolates between the last two snapshots
for smooth visual.

This lets you validate the network plumbing end-to-end before
adding complexity. If position replication doesn't work, no other
replication will.

### 3.5 Auto-spawn the remote

On first packet receipt from a peer, spawn an orphan to represent
them. Once the orphan exists, push pose snapshots into it.

This is your first integration point between Phase 2 (orphan
infrastructure) and Phase 3 (network).

**Phase 3 deliverable**: both players see each other's wilbur
moving in real time on LAN. No animation (still T-pose sliding), no
weapons, no NPCs. Just position. Both players' position-only state
is replicated and visualized.

---

## Phase 4: replication layers

In order:

### 4.1 Input replication

Send the remote player's button + analog state. The receiving side's
engine ControlMapper drives the orphan from "P2's input bytes" — so
animation, weapon firing, jumping, all the normal player behaviors
"just work" on the orphan because the engine's own systems drive them.

Wire cost: small (~10-30 bytes per packet at frame rate).

Why this is preferred over "replicate animation state directly":
- One mechanism covers anim, weapon, interaction, movement, etc.
- The engine's own systems run on the input — no reimplementation
  needed.
- The engine remains the source of truth for player behavior.

This is the **MTA "keysync packet" pattern**. MTA ships pure
controller state at frame rate; the receiving engine replays it on
the remote player entity.

### 4.2 Equipment / weapon state

Equipped weapon ID, ammunition, special-ability mode. These don't
follow from input alone (they're owned state, not input-derived).

Wire cost: ~5-10 bytes per packet.

### 4.3 Entity manifest + per-entity state

When entering a level, the host enumerates all script-spawned
entities (NPCs, enemies, bosses, interactables) and broadcasts the
manifest: `[entity_id, class, initial_state]`.

The client materializes orphans for each. After that, per-entity
state deltas flow.

Per-entity state: position, orientation, anim state, small bag of
script-state vars (health, target, current-action).

Wire cost: ~30-100 bytes per active entity per tick, delta-compressed
+ per-entity send-rate gated (visible entities update more often than
out-of-sight ones).

**This is the biggest replication layer.** It covers: enemies,
bosses, dialogue NPCs, scripted interactables, all in one mechanism.

### 4.4 Cutscenes / scripted events

When the host's script VM fires a cutscene / dialogue / scripted
event, RPC the client: "play cutscene X starting at host_time T".
Both engines run the canned sequence locally driven by their own
clocks.

Skip semantics: any player presses skip → both end the cutscene.

### 4.5 Save / world state sync

On session start, the client loads the host's save (transferred
over the network or assumed local). The world state is now
synchronized; per-entity state then keeps it that way.

### 4.6 Inventory / progression

Shared, host-side. Resources, unlocked blueprints, completed
objectives — all live on the host. Client UI overlays read host
state via RPC.

---

## Phase 5: validation

### 5.1 Autonomous test harness

Build a scenario-driven test runner BEFORE Phase 4 ships. Each
scenario:
- Boots the game with specific cmdline flags.
- Drives the engine through specific actions (load save, move,
  fire, transition level, etc.) via synthetic input.
- Runs for a fixed number of frames.
- Exports a JSON report at exit.

A test harness lets you regression-test every Phase 4 change without
manual play.

### 5.2 LAN soak

A dual-process scenario: launch host + client on the same machine
(if dual-launch is possible) or two machines. Both run the harness.
Both produce JSON reports. The runner aggregates them.

Validate: packets sent + received within expected counts, no
crashes, no leaks over N minutes.

### 5.3 Live testing

Manual play sessions, both windows side-by-side. Record observations
of what works and what doesn't. Each observation maps to a phase
(O1 = anim slide → Phase 4.1; O2 = NPC desync → Phase 4.3; etc.).

Live testing finds bugs the automated harness can't (UX issues,
visual desync, scripted-edge-cases). It is NOT a substitute for
the harness; both are needed.

### 5.4 Multi-agent audit

After substantial work, spawn 2-3 review agents in parallel. See the
**Working patterns** section below for the full pattern (WP2).

---

## Working patterns

These are the collaboration / development patterns that make the
methodology work in practice. They are specific to working with
Claude (or any capable agent) on a coop-mod project, but most also
apply to a senior-engineer pair. Each pattern was learned from
specific incidents in the MTR project and is captured here so they
don't have to be re-discovered.

### WP1 — Treat the agent as a senior collaborator, not a transcriber

The agent has the same access to the codebase, the same RE tools, the
same memory of past decisions. Tell it WHAT to accomplish, not HOW to
do every step. Bad: "read file X, then file Y, then edit file Z line
N". Good: "the orphan crashes during weapon switch — root-cause it
and fix it per principle 4".

The agent picks the right tools (IDA queries, code reads, grep) and
sequences them. You verify the result, not the steps.

**When to be more prescriptive**: irreversible actions (git push,
delete files, large refactors), or when the user has context the
agent can't infer (a specific runtime crash they just saw).

### WP2 — Multi-agent audit after substantial work

After completing a substantial change (a phase ship, a non-trivial
hook, a new replication layer), spawn 2-4 review agents in parallel
BEFORE declaring the work done:

- **Agent 1 — Domain-fidelity audit**: "Does the implementation
  match the design / scope / engine semantics?" Reads the design
  docs + the new code + relevant engine RE; reports mismatches.
- **Agent 2 — Code-quality / safety audit**: "Are there bugs,
  races, memory issues, security gaps?" Reads the new code +
  related call sites; reports concrete issues with confidence
  levels.
- **Agent 3 — MTA-precedent fidelity audit** *(mandatory for any
  network / replication / parallel-class-hierarchy work)*: "Does
  this match how MTA solves the same problem?" Reads
  `reference/mtasa-blue/Client/game_sa/` + relevant
  `Client/mods/deathmatch/logic/` files + the new code. Reports
  divergences from MTA's pattern + an opinion on whether each
  divergence is justified (game-specific quirk) or a mistake
  (we're reinventing a wheel that MTA already debugged).
- **Agent 4 (optional) — Anti-pattern audit**: "Does this violate
  any of the 7 principles or the 9 anti-patterns?" Reads the
  methodology doc + the new code; reports violations.

Spawn them in **parallel** in a single message. Wait for all. Apply
findings BEFORE declaring done. Documentize the fixes.

**MTA-precedent audit is mandatory for**:
- Class design (any new `RemotePlayer`-like class).
- Wire format design (new packet types, serialization).
- Replication strategy decisions (what's host-auth, what's
  per-client, what's interpolated).
- Session / connection lifecycle changes.
- Cutscene / scripted-event sync.
- Input replication.

**Skip MTA audit only for**:
- Pure engine RE work (no architectural decision yet).
- Cosmetic / UI changes that don't touch network or replication.
- Bug fixes that don't change a design surface.

**Why parallel**: agents run independently → no anchoring on each
other's framing → catches different issues. The MTR filter-
retirement decision (2026-05-12) was caught by a 3-agent consensus
where each agent flagged the gating-metric ambiguity from a different
angle.

**Cost**: 30-90 sec wall time per audit pass. Cheap compared to
shipping a regression OR re-inventing an MTA pattern that's already
solved.

**Example prompt for MTA-fidelity agent**:
> "We just implemented X in `src/coop/X.cpp`. Read MTA's
> implementation of the equivalent at
> `reference/mtasa-blue/Client/.../X.cpp`. Compare. Report:
> (1) structural differences and whether each is justified;
> (2) MTA features we omitted and whether they should be added;
> (3) MTA gotchas / bug fixes / edge cases we may not have
> accounted for. Cite specific MTA file:line for each finding."

### WP3 — Don't ask the user when the rules dictate

When the methodology / `CLAUDE.md` / MTA precedent settles a
question, **do not ask the user**. Apply the rule directly. The user
has already pre-committed by writing the rule.

**Examples**:
- "Should I add a filter to make this crash go away?" — NO,
  principle 4. Don't ask.
- "Should I replicate this field?" — check `COOP_SCOPE.md`. If
  unclear, spawn a research agent. Don't ask the user.
- "Should I write migration glue for the old API?" — NO, RULE №2.
  Don't ask.

**When TO ask**:
- Irreversible actions (delete, force-push, dependency removal).
- Ambiguity in the user's own message that you can't resolve from
  context.
- Scope decisions that would extend `COOP_SCOPE.md` (these are
  user-owned).
- UX / aesthetic decisions where the user's preference is
  decisive.

**When in doubt between "ask user" and "spawn 3 parallel agents to
research and decide"**: pick agents. They can verify in code; the
user has to recall from memory.

### WP4 — Verify, don't guess

Before describing a function, file, or behavior, **verify it exists
in current state**. Run the tool. Read the file. Grep for the
symbol. Don't reason from memory of how it "should" be.

**Common failure mode**: "I remember that function X is at address
Y" — but Y has been renamed, refactored, or removed. The memory
record is stale.

**Mitigations**:
- For function VA claims: `xrefs_to` or `disasm` to confirm.
- For file content claims: `Read` the file.
- For symbol existence: `grep`.
- For runtime behavior claims: re-run the test, don't recall the
  output.

This applies even to claims from memory entries. Memory is a
snapshot in time; current state is ground truth.

### WP5 — Document findings + rename in IDB before declaring done

Every RE finding has two outputs:
1. Function / variable renamed in the IDB (`game_load_save`,
   `g_resource_pool`, etc. — semantic names, not `sub_XXXXXX`).
2. A research finding markdown in `research/findings/`
   describing what was learned, with cross-references to function
   VAs.

Both must exist BEFORE the finding is considered shipped. The
markdown without the rename rots (future Claude re-finds the
function and re-names it differently). The rename without the
markdown loses context (future Claude doesn't know why it matters).

### WP6 — Per-site routes, not broad filters

When fixing a crash or wrong behavior, the patch is at THAT call
site. Not at a layer above. Not at a layer below. Not "I'll filter
all the inputs that cause this".

This is principle 4 restated as a working pattern: when reaching for
a tool, reach for the SCALPEL (per-site patch), never the
SLEDGEHAMMER (broad filter).

**Smell test**: if your patch would change behavior for any caller
OTHER than the one you're fixing, it's too broad. Narrow it.

### WP7 — Retirement gating metric specificity

When installing a transitional crutch (per principle 4 exception),
write down the EXACT condition for its retirement. Not "when it's
safe", not "when N frames pass", not "when soak completes".

**Specific** = "when mechanism X has been active for N≥M frames
without firing, soak result reports `mechanism_x_zero=true`, then
retire".

**Why specificity matters**: the MTR filter retirement (2026-05-12)
was almost retired on the wrong criterion ("100 frames of soak") —
a 3-agent audit caught that the soak was measuring something
different than the criterion required. Specificity at write-time
prevents this.

### WP8 — Autonomous test passes don't mean work is done

A single-player autonomous test exercises ONE process's code paths.
It cannot exercise:
- Multi-process coop interactions (peer-to-peer).
- Engine state that depends on real player input.
- Visual / UX correctness.
- Edge cases that only appear after minutes of play.

When the autonomous test passes, you've validated ONE slice of the
work. You have NOT validated coop-correctness, performance under
load, or live-play behavior.

**Implication**: autonomous test pass = ship to live test. NOT
autonomous test pass = ship to production.

### WP9 — Naked-stub PRE/POST hook pattern

For functions you can't safely modify the prologue of (stolen-byte
IAT thunks, mid-instruction stub points, vectored exception
handlers), use a naked-stub hook:

```asm
hook_pre:
    push registers
    call cpp_pre_handler   ; your code runs here
    pop registers
    call original_function ; tail-call the real function
    push registers
    call cpp_post_handler  ; your post-code runs here
    pop registers
    ret
```

**Critical pitfall** (learned 2026-05-09): the forward-call save
MUST use a static memory slot, NOT a register. The trampoline is
`__cdecl` and clobbers `EAX/ECX/EDX`. Saving to a register and
restoring loses the saved value to a 0xC0000005 crash.

Reference impl: MTR's `src/mtr-asi/src/widget_probe.cpp`.

### WP10 — Plain English UI, technical depth in tooltips

For mod menus and debug overlays: the **visible labels** are plain
English ("Free camera", "Aspect ratio override"). The
**technical depth** (function VAs, struct offsets, milestone tags,
research notes) lives inside a `(?)` info tooltip.

Why: the user does RE work too and values technical depth, but
walls of jargon in the main UI make it unusable for casual mod
configuration. Tooltips give both: clean UI for fast use, depth on
drill-in.

Apply to: every mod menu screen, every overlay, every config UI.

### WP11 — Check 0-3 byte offsets for alignment filler

When a stack-trace address appears "unanalyzed" or "no xrefs", check
addresses 1-3 bytes BEFORE it. Compilers insert 1-3 byte alignment
filler before functions, so the trace might point at the filler
byte rather than the function entry.

This saved the C1 wrapper-ID gap diagnosis in 2026-05-12 (the real
function was 2 bytes before the address shown in IDA).

### WP12 — Verify orphan functions before patching

If a function has zero xrefs AND zero immediate-value references AND
zero find-bytes matches in the rest of the binary, it's dead code.
Patching it does nothing.

Before declaring "I patched this function", confirm it's actually
called by someone in the running game.

### WP13 — Use established libraries, don't reinvent

For mod-side infrastructure:
- **Hooks**: MinHook (mature, simple API, well-tested).
- **Overlay rendering**: Dear ImGui (immediate-mode, integrates
  with D3D9/11/12 easily).
- **D3D translation (if game uses old API)**: DXVK or D3D8to9.
- **PDB symbol lookup**: DbgHelp WinAPI.

Don't write your own hook engine, overlay renderer, or D3D wrapper.
The established ones handle edge cases you don't know about yet.

### WP14 — IDA MCP: serial, not parallel

If using IDA Pro MCP (model context protocol) for RE queries, the
Hex-Rays decompiler is **single-threaded**. Parallel `decompile`
calls hang the plugin.

- Parallel queries OK: `xrefs_to`, `disasm`, `find`, `list_funcs`,
  `get_bytes` — these are read-only on the IDB.
- Serial only: `decompile`, `analyze_function`, anything that
  invokes Hex-Rays.

If a `decompile` hangs, ask the user to restart IDA. Do NOT retry
on timeout — that compounds the hang.

### WP15 — DirectInput exclusive mode defeats WndProc input

For input hooks: if the game uses `DISCL_EXCLUSIVE | DISCL_FOREGROUND`
on DirectInput devices, the OS routes ALL input to DI bypassing the
normal WndProc message pump. Your `WM_KEYDOWN` / `WM_MOUSEMOVE`
hooks see nothing.

Solutions:
- Poll `GetAsyncKeyState` for mod hotkeys (per-OS-window, ignores DI).
- Hook `IDirectInputDevice::GetDeviceState`; read or modify the
  buffer there.
- For mouse: `WH_MOUSE_LL` low-level hook catches OS-level mouse
  events that bypass DI.

The mod menu polls keys via `GetAsyncKeyState` and feeds them to
ImGui's `io.AddXxxEvent` instead of relying on WndProc.

### WP16 — Don't gate mod-side input with engine-side gates

If you install a foreground-gate to prevent the engine from reading
background-window input, do NOT apply the same gate to mod-side
input reads. The mod is debug tooling; it should respond regardless
of focus.

Split the read paths: mod reads from an un-gated DI cache; engine
reads from the gated buffer. Or migrate mod hotkeys to
`GetAsyncKeyState` (per-window, no gate needed).

MTR project hit this bug on 2026-05-12 (F3 and MMB-tp dead on
non-focused client window). Recorded as research finding.

### WP17 — Module split when files cross subtree boundaries

When a single file BOTH dereferences engine memory addresses AND
owns network state, that's a principle 7 violation. Split it.

Concrete split: extract the engine-touching code to
`src/engine_wrap/X.cpp`; keep the network state in
`src/coop/X.cpp`; they communicate via a clean header API in
`include/engine_wrap/X.h`.

This is mechanical refactoring — boring but mandatory. Files that
straddle the boundary accumulate complexity geometrically.

### WP18 — Memory entries decay; verify before recommending

Memory records (between-session persistence) are TIME SNAPSHOTS.
A memory entry that says "function X is at address Y" was true when
written; it may not be true now.

Before recommending an action based on a memory entry:
- Verify the file / function / symbol still exists at the
  documented location.
- If it doesn't, update or remove the memory entry, then act on
  current state.

**Smell test**: "the memory says X" is NOT equivalent to "X is true".
Memory is a hint, not authority. Current code is authority.

---

## Anti-patterns (do NOT do these)

### A1 — Broad suppression filters

Symptoms: "filter out all entities that look suspicious from this
lookup", "skip the engine's check if it returns NULL", "catch the
crash and ignore it".

Why it's bad: hides multiple root causes behind one mechanism. Each
underlying problem grows hidden. When the filter inevitably leaks (it
will), you have N crashes with no individual diagnoses.

Right approach: find each specific call site, patch each one
individually with a targeted fix.

### A2 — "Replicate everything from the engine"

Symptoms: per-frame replication of every entity, every state, every
field. Massive wire bandwidth. Complex serialization.

Why it's bad: most engine state is derivable (animation from input,
visual state from physics, etc.). You only need to replicate
**authoritative state** that can't be re-derived.

Right approach: minimum viable replication. Replicate what's
authoritative (input, owned state). Let the engine re-derive the
rest locally on each machine.

### A3 — "We'll fix it later"

Symptoms: shortcuts marked with `// TODO`, code that "works for now",
known bugs left in the ship because they're "edge cases".

Why it's bad: the shortcuts never get fixed. They become permanent.
Each one constrains future work.

Right approach: fix it now. RULE №1.

### A4 — Per-process global state assumed shared

Symptoms: "we store this in a `static` and it works in SP, surely it
also works in coop". The state is per-process; SP has one process;
coop has two. The "shared" assumption is implicit and broken.

Right approach: identify all global state. Decide for each: is it
host-owned, replicated, or per-machine? Document the decision.

### A5 — Mocking the engine in tests

Symptoms: tests that don't actually exercise the engine's behavior,
just the wrapper's expectations of what the engine should do.

Why it's bad: divergence between mock and engine causes false-pass
tests. The shipped code breaks in production.

Right approach: integration tests against the real engine, even if
slower. The cost of slow tests is less than the cost of false passes.

### A6 — Editing assets

Symptoms: "we changed the model file to add a hat", "we patched the
.sx script to add a trigger".

Why it's bad: principle 1. The mod becomes a redistribution. Asset
edits forks the pipeline. Future engine updates won't merge.

Right approach: hook the runtime asset path. Add the new content
via memory patches or runtime data injection.

### A7 — Network protocol coupling to engine internals

Symptoms: wire format includes engine memory addresses, vtable
pointers, engine-internal IDs that vary per-build.

Why it's bad: brittle. Recompiling the engine breaks the protocol.

Right approach: wire format is **semantic** — entity class IDs, not
vtable pointers; position vec3, not entity-memory layout. The
engine-wrapper layer translates between wire and engine.

### A8 — Single shared state for testing and production

Symptoms: the same global is used for "currently playing this
scenario" (test) and "currently in this game mode" (production).

Why it's bad: test scenarios can leak into production behavior.

Right approach: separate paths. Test harness state is its own
subsystem; production reads its own state.

### A9 — Skipping IDA / RE rename + docs

Symptoms: function VAs known but not named in the IDB; findings
known but not written down.

Why it's bad: in 6 weeks you'll forget why this function matters.
Future Claude / future you re-RE the same thing.

Right approach: every RE finding → rename function in IDB + write
research finding markdown. Before declaring "done", check that both
are committed.

---

## Decision tree: should we replicate X?

```
Q1: Is X in COOP_SCOPE.md "in scope"?
├── YES → REPLICATE.
└── NO  → CONTINUE
    │
    Q2: Does either player's experience break if X is not replicated?
    ├── NO  → DO NOT REPLICATE. Document in scope as "out of scope".
    └── YES → CONTINUE
        │
        Q3: Can X be re-derived locally on each machine from already-
            replicated state?
        ├── YES → DO NOT REPLICATE. The local engine re-derives.
        └── NO  → CONTINUE
            │
            Q4: Is X authoritative on one side (host owns it)?
            ├── YES → REPLICATE host-to-client. Add to scope.
            └── NO  → THIS IS A DESIGN PROBLEM. Decide who owns X first.
```

---

## Common engine RE techniques

### T1 — String search

The binary has strings: entity class names, script command names,
error messages. Search for them. The strings' callers are often the
functions you're looking for.

### T2 — Save-file diff

Two saves with one variable change → diff the bytes → the offset of
that variable. Then write-watchpoint on that offset to find the
write site.

### T3 — Stack-walk at crash

Crash gives you a call stack. Every frame is a function. Look at
each. The "interesting" function is usually 2-4 frames up from the
crash site.

### T4 — Vtable enumeration

`g_vtable_X[0..N]` is a list of N function pointers. Each is a method
of class X. Name them in IDB; their signatures often clarify the
class's design.

### T5 — Cross-reference (xref) walking

For function F, look at all callers (`xrefs_to F`). Group them by
behavior. The pattern usually tells you what F does (e.g. all
callers fire on a button press → F is an input handler).

### T6 — Diffing two related games

If the engine is from a known family (RenderWare, Unreal, Avalanche,
etc.), older games on the same engine may be RE'd already. MTR =
Avalanche engine = related to GTA SA in structure. MTA's GTA SA
research is directly applicable.

---

## What's NOT in this methodology

This document does NOT cover:
- Anti-cheat / DRM bypass beyond what's needed for the mod to load.
- Wide-area / internet networking. LAN-only.
- Server-cluster architecture / dedicated servers.
- Custom scripting layers (Lua, etc.) for end-user mods of the mod.
- Asset modding pipelines.

If your project needs any of those, supplement this doc with
additional research. The core 7 principles + the 5 phases stay valid
regardless.

---

## When this methodology applies

**Good fit**:
- Single-player 3D game with a protagonist + AI NPCs.
- Hookable APIs (DX9 / DX11 / DI / WinAPI).
- Binary that can be unpacked / debugged.
- Coop story that makes sense at the game level (a story to share, a
  level to traverse, boss fights to defeat).

**Bad fit**:
- Modern AAA with anti-tamper that resists hooking.
- Games designed only as multi-player (no single-player to extend).
- Games with hardcoded P1-only assumptions in proprietary engines
  that can't be RE'd in reasonable time.
- Browser / cloud / streaming games (no local binary).

**Adjustable fit**:
- 2D / isometric games: same principles, simpler entity model.
- Turn-based games: networking is simpler (event-driven), but
  consistency is critical.

---

## Reading order for a fresh project

If you're starting a coop project from zero:

1. Read this doc end-to-end.
2. **Clone the MTA:SA reference repo into your project** (Phase 0.0
   above). Without it, every architectural decision is uninformed
   guesswork:
   ```bash
   git submodule add https://github.com/multitheftauto/mtasa-blue.git reference/mtasa-blue
   ```
3. Skim [`reference/mtasa-blue/Client/game_sa/`](../reference/mtasa-blue/Client/game_sa/)
   and [`reference/mtasa-blue/Client/mods/deathmatch/logic/`](../reference/mtasa-blue/Client/mods/deathmatch/logic/)
   — see principle 7 in real code.
4. Read MTA's `CClientPed.cpp` end-to-end before designing your
   `RemotePlayer` class.
5. Add a line to your project's `CLAUDE.md`:
   > For any coop / multiplayer architecture decision, consult
   > `docs/COOP_METHODOLOGY.md` AND
   > `reference/mtasa-blue/`. After any non-trivial coop work,
   > spawn an MTA-fidelity audit agent (WP2).
6. Read the MTR project's `docs/COOP_SCOPE.md` for an example of a
   completed scope doc.
7. Read the MTR project's `research/findings/coop-*` files for
   examples of phase deliverables.
8. Start Phase 0.

---

## Final note

Coop is a 6-month to 2-year project for a non-trivial game. There is
no shortcut. Every "we can ship faster if we skip X" decision is a
trap that costs more time later. Apply RULE №1 ruthlessly.

The shape of the project is: 4-8 weeks of Phase 0 + Phase 1 (RE,
boring). 8-16 weeks of Phase 2 (crash-by-crash, painful). 4-8 weeks
of Phase 3 (network plumbing, satisfying). 12-26 weeks of Phase 4
(actual replication, the bulk). 8-12 weeks of Phase 5 (validation,
polish).

Pace yourself. Document everything. Trust the principles.
