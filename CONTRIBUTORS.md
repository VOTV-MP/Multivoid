# Contributors

Multivoid is written by **pelmentor** with **Claude** (Anthropic) as the
day-to-day engineering pair — and it gets real fixes and features from the
community. Community commits are adopted with their **original authorship
preserved** (`git log --author=<name>` shows exactly what each person wrote);
this file is the human-readable ledger of who contributed what.

## Code contributors

### Tarangok
- **KO respawn** (`death.ko_respawn`, on by default): lethal damage becomes a
  knock-out + respawn at the КПП start instead of the stock kick-to-menu
  permadeath — the foundation of the mod's respawn system.
- **Live mannequin skin preview**: hovering a skin in the F1 menu shows it on
  a real in-world mannequin.
- Cross-peer held-prop visibility fix (clients now see props carried by other
  clients, not just the host's).
- Container extraction fix (a client-extracted item now reaches the host's
  world) and the author-side volume re-derive.
- Duplicate keyed props on a joining client (the double starting suitcase).

### hediiiqq
- Dish mirror interpolation: the 4 Hz dish pose stream now glides through a
  proper lerp window instead of snapping every 250 ms.

## Testers

Field reports with logs and screenshots have driven real root-cause fixes —
among them **arigalit** (the red-mist weather desync, ATV/prop reports, and
multi-instance test sessions).

## How to contribute

Fork, branch, and open a pull request against `main`. Fixes are reviewed
against the project's engineering rules (`CLAUDE.md` — root-cause fixes only,
no migration baggage); adopted commits keep your authorship in the history.
Note the repository currently ships **no license file** — by default that
means "all rights reserved": contributions are accepted into this project via
GitHub's inbound=outbound convention, but redistribution of the codebase is
not otherwise granted. A proper license decision is tracked by the project.
