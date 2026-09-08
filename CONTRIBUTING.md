# Contributing to Multivoid

Multivoid is a co-op multiplayer mod for *Voices of the Void*, built as a UE4SS mod that
modifies no game file. This page is the rulebook: what the code must respect, how a change is
built and tested, and the shape a commit and a doc must have. It is written for people and for
the AI tools the maintainer works with; both follow the same rules, and a script checks every
rule a script can check.

The mod is written by one maintainer (pelmentor) with Claude as the day-to-day engineering
pair. That is stated openly here, in the README and in every commit's trailer. Outside
contributions are adopted with their original authorship preserved (`git log --author=<you>`).

## Ground rules

Three rules govern every change:

1. **No crutches.** Fix the root cause at the site of the problem. No filters, skip-ifs,
   suppress-X or catch-and-ignore around a symptom. If the proper fix is large, it is still
   the fix.
2. **No migration baggage.** When something is replaced, the old code goes in the same change:
   no "deprecated, kept for now", no flags that re-enable old behaviour, no two implementations
   of one concept compiled together.
3. **Own substrate.** UE4SS is the loader and the development tool, never the engine layer.
   The DLL imports zero symbols from it (`.github/ci/abi_gate.py`, run in CI); reflection,
   hooking, transport and UI are the mod's own.

And eight architectural principles; the reasoning behind them is in
[docs/architecture.md](docs/architecture.md):

| # | Principle |
|---|---|
| 1 | No modification of original game files. Hooks and runtime patches, yes; editing the exe, the paks or the cooked assets, no. |
| 2 | The mod is an engine-extension layer on top of UE4 + VOTV, not a set of hooks. |
| 3 | A parallel class hierarchy: our `RemotePlayer` owns network state; the engine's `APawn` and `APlayerController` own rendering, animation and physics. |
| 4 | Targeted crash fixes, never broad suppression. |
| 5 | Minimum viable subset: [docs/scope.md](docs/scope.md) is the law of what is synced. |
| 6 | Augment single-player, never replace it: route per player inside the game's own systems. |
| 7 | Two layers, two subtrees: `ue_wrap/` (engine wrapper, no gameplay or network logic) and `coop/` (gameplay and network, reaching the engine only through `ue_wrap/`). |
| 8 | Mid-activity join is always handled: every sync lane defines what a peer joining mid-event, mid-download or mid-drive sees. |

The architectural precedent is MTA:SA, vendored read-only in `reference/mtasa-blue/`. When a
design question has an MTA answer, that answer is the default; a deliberate divergence says so
in a comment at the site.

The rules on this page are not a one-off cleanup. The repository was once hard for a person to
read -- half its lines were prose, much of it stale, and its own history was the only way to tell
a working note from a document -- and it was rewritten to fix that. Everything below holds for
every change from now on: a new file, a new comment, a new document and a new commit are written
to the same shape as the ones already here, so the tree does not drift back. Each rule names the
script that checks it, because a rule nothing enforces is a rule that decays.

## Where things live

| Path | What |
|---|---|
| `src/votv-coop/src/ue_wrap/` | engine wrapper: reflection, signatures, hooks, the game-thread pump |
| `src/votv-coop/src/coop/` | gameplay and network: elements, sync lanes, sessions, players |
| `src/votv-coop/src/ui/` | the in-game UI: native UMG screens and the ImGui overlay |
| `src/votv-coop/src/harness/` | boot glue and the scripted test scenarios |
| `src/votv-coop/src/loader/` | the UE4SS `start_mod()` entry |
| `src/votv-coop/include/` | the headers, same split |
| `server/` | the master server and the signaling relay, in Rust |
| `.github/ci/` | the scripts the workflows run: the public-surface gates, the release predicates |
| `docs/` | the documentation; [docs/README.md](docs/README.md) is the index |
| `reference/` | vendored read-only references (UE4SS, MTA:SA) |

Each source folder maps to one domain concept and is named after it. There are no catch-all
folders (`utils`, `misc`, `helpers`), on purpose.

## Building and testing a change

Build: [BUILDING.md](BUILDING.md), locally with CMake and the Visual Studio Build Tools, or on
GitHub Actions from a fork with no local toolchain.

A change is not done when it compiles. Before a pull request:

1. Build **Release** and deploy it to a game install, as BUILDING.md describes.
2. Run two peers for at least 30 seconds of steady state -- host on one, join from the
   other, on one machine or across a LAN.
   Read both logs: no `[Warn]` or `[Error]` lines from the mod, no line repeating at a rate
   the design did not intend.
3. Keep files under 800 lines. A file that needs to grow past that is split first, in its own
   commit. One feature per file.
4. Per-frame and per-packet code is measured, not assumed: no full-object-array scans on a hot
   path, no allocation in the pose tick, engine functions only on the game thread.

CI runs the same gates on every push (`.github/workflows/`): the build, the zero-import ABI
gate, the config-registry gate, the atlas gate, the package drill, the public-leak gate, and the
commit-message and public-prose checks described below.

## Commits

Small, single-concern commits, with a message a reader can follow in `git log --oneline`:

```
[scope] imperative summary, at most 72 characters

What changed and why, in at most 12 lines. One line of evidence:
what was measured and what it showed.

Co-Authored-By: ...
```

- `[scope]` is the subsystem or area: `[coop]`, `[net]`, `[ui]`, `[tools]`, `[docs]`,
  `[release]`, `[lights]`, `[atv]`. Lowercase, one word. Git's own `Merge branch ...` and
  `Revert "..."` subjects are exempt; `[docs] close:` belongs to the maintainer's session-close
  script.
- The body is at most 12 non-empty lines; the attribution trailers at the end are not counted.
- English only (no Cyrillic), valid UTF-8, no byte-order mark.
- No quoted conversations, no session narrative, no references to the maintainer's private
  notes or tooling. The checker refuses these words and paths: `/qf`, `documentize`, `agent`,
  `verbatim`, `USER` (all caps), `Docs-Census`, `memory/`, `research/`, `CLAUDE.md`, `.claude/`,
  `docs/security/`. The long story of a change belongs in the subsystem's doc if it is still
  current, and nowhere if it is history: `git log -p` keeps the diff.
- A doc that describes a subsystem changes in the same commit as the subsystem.

`.github/ci/commit_msg_check.py` is the checker. Install it as a hook once per clone:

```powershell
git config core.hooksPath .github/ci/hooks
```

The hook refuses a message that breaks the shape and says why. CI runs the same check over
every commit of a push.

## What we add

A change has to be intelligible and maintainable by the next person, not only correct. Prefer the
shape a maintainer can hold in their head: an umbrella header over an include every caller must get
right, one example over a paragraph of rules, a name that says what a thing is over a clever one.
Precision that forces a lookup on every future edit is a cost, and it is usually larger than the
duplication it removes. When the two pull apart, say in the commit message which you chose and why.

## Documentation

`docs/` is written for two readers, a player and a contributor. It describes what the mod is,
what it syncs, how it works now, and how to work on it. Every tracked doc:

- is in English, in the present tense, and is edited in place when the code changes (the
  history of a doc is `git log`, not a changelog inside it);
- is one of: the index, the architecture, the roadmap, the scope, or one doc per subsystem on
  one skeleton: *Purpose · How it works · Who owns what · Wire messages · Late join · Known
  limits · Code map*;
- stays under 300 lines (600 is the hard cap) and links rather than repeats;
- carries no working notes: no dated diary entries, no quoted conversations, no record of the
  review process, no pointers to files that are not in the repository.

The last rule is about the whole repository, not only `docs/`. Every tracked file is published:
the build files, the CI workflows, `.gitignore`, and the scripts you are invited to run. A comment
in one of them says what the rule or the step does, not the story of the day it was added, and
never describes material that is deliberately not in the repository -- an explanation of why
something is unpublished can publish the interesting half of it.

A claim about behaviour is tagged with how it was established: `[V]` measured, `[RD]` derived
from reverse engineering, `[?]` unverified. The legend is on [docs/README.md](docs/README.md).
A doc that says something works without naming its evidence is a bug in the doc.

Working notes, reverse-engineering logs, design drafts and session records are kept by the
maintainer outside the repository. The `docs/` tree is an allowlist in `.gitignore`: a new doc is
published by adding its `!docs/<file>` line, so a working note left there stays local by default.
The filename says which half a document is in, so nobody has to ask git: a published doc is named
in lower case (`voice-and-chat.md`, `coop-dispatch-visibility.md`), a local one in capitals
(`DEATH_ARC.md`). The exceptions are the files an ecosystem expects to find shouting -- `README`,
`LICENSE`, `CONTRIBUTING`, `SECURITY`, `THIRD-PARTY-NOTICES`, `CHANGELOG` -- because GitHub gives
several of them special treatment and moving them would add exactly the strangeness this rule
removes.
`.github/ci/public_prose_gate.py` measures the public tree
against these rules: the working-notes words and paths above, dead links and paths, docs over
the hard cap, dated lines, and in the source the comment blocks over 15 lines, the files that are
more than half comment, offsets pinned in prose that the code resolves elsewhere, declarations
nothing calls, and the citation vocabulary below. CI refuses a push that makes any of those
measures worse; the plain volume of prose is reported, not gated.

## Code comments

A comment says what the code does and why, as of now. It carries no dates, no quotes, no
references to reviews or to notes outside the repository, and not the story of what the
previous version did: that story is the commit message. A file may open with a block of up to
15 lines saying what it is and why it exists; anything longer belongs in the subsystem's doc.

Two habits from the working notes stay out of the source. A finding or work item named by its
label -- `CRIT-1`, `Inc-2`, `take-9`, `K-5` -- points at a document that is not in the
repository, so state what the code does instead. Evidence tags (`[V]`, `[?]`) belong in the
documentation, where the index page carries their legend; a source comment has no legend, and
the reader is better served by the fact than by a mark on it.

## Pull requests

Fork, branch, open a pull request against `main`. A review checks the three rules, the
principles, the build, the smoke, and the commit and doc shape above. Adopted commits keep your
authorship in the history, and you keep the copyright on them.

The repository is licensed under the [MIT License](LICENSE). Contributions are accepted under
the same terms (GitHub's inbound = outbound convention). Commits adopted before the license
file existed (2026-08-29) are covered by the same convention; if you authored one and object,
say so in an issue and it will be honoured.

Bug reports and questions: [GitHub issues](https://github.com/VOTV-MP/Multivoid/issues) or the
[Discord](https://discord.gg/bA6tGBvGMN). Every report and review that changed the mod is
credited in [docs/credits.md](docs/credits.md).
