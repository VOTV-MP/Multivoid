# docs/events/ — the per-event coop knowledge base

One doc per VOTV event SCENARIO (user rule 2026-07-04): each event's native behavior, its
coop sync design, and its caveats live in that event's own file here. This folder answers
"what exactly happens during event X and how does coop deliver it to every peer" — the
cross-cutting CONTRACTS stay where they are:

- `docs/COOP_EVENT_JOIN.md` — the join-during-event contract (EventSnapshot AS-BUILT v98,
  the per-lane late-join table, phases). Per-event docs LINK to it; they do not restate
  it. A new event's registrant class also gets an `event_active_sync::kClassRowMap` entry
  (unmapped classes WARN LOUD on a joiner — that log line is the fill signal).
- `src/votv-coop/src/coop/world/event_fire_sync.cpp` — the replay/no-replay dupe matrix
  is AUTHORITY in code (kReplayRows/kNoReplayRows). A per-event doc cites its row's
  verdict; changing a verdict changes the CODE first, the doc second.
- `research/findings/votv-event-system-RE-2026-06-13.md` — the runEvent dispatcher RE
  (all 69 list_events rows' concrete outputs). Durable ground truth; per-event docs cite
  it instead of re-deriving.
- `research/findings/votv-active-events-registry-RE-2026-07-04.md` — the native
  activeEvents registry (lib_C::setEvent refcount + senders; the ~95-class census).

## Doc skeleton (copy for a new event)

```markdown
# <event name> — <one-line what the player sees>   (STATUS: DESIGN | AS-BUILT | VERIFIED)

## 1. Native behavior (ground truth)
   list_events row(s) / trigger; actor class(es); the full scenario timeline; every
   externally visible effect. Tag every claim [wiki] / [bytecode fn] / [live log] / [?].
## 2. Sync-axis table
   | axis | deterministic-from-start / host-random / per-viewer | carried by (lane/wire) |
## 3. Coop design
   What is mirrored, by which lane; what replays natively; what is host-local. Cite the
   dupe-matrix verdict + the late-join answer (COOP_EVENT_JOIN.md 3.4 row).
## 4. Caveats / known quirks
   Native bugs, save/reload interactions, timing races, what autonomy can't verify.
## 5. Verification
   What was proven, HOW (smoke line / hands-on / matching real log), what remains.
```

A "sync-axis" is one externally-visible state stream (position, anim phase, sound cue,
sub-spawn, decal...). The table forces the design decision per axis instead of a vague
"mirror the event".

## THE METHOD (user rule 2026-07-04): granular, every single event

**Going through every event the game has, one by one, is the only way events get synced.**
No event is "covered" by a blanket mechanism claim; each one gets its own granular pass:

1. **Doc** it here (all 5 skeleton sections; wiki + bytecode ground truth, tagged).
2. **Verify** its dupe-matrix verdict against the RE (the piramid pass PROVED this step
   earns its keep: `piramid` sat in kReplayRows, but replay = a divergent client-local
   pyramid — the verdict was WRONG and only the granular dig caught it).
3. **Design + build** the sync where a gap exists; extend the COOP_EVENT_JOIN.md 3.4
   late-join row.
4. **Verify** live (autonomy for flow, hands-on for visuals) and stamp the doc's status.

An event counts as DONE only when its doc says VERIFIED with the evidence named.

## Master tracker

The inventory is enumerated in three places (do not re-derive): the 69 `list_events` rows
(`votv-event-system-RE-2026-06-13.md` section 10 — every row's concrete output), the
~95 `setEvent` registrant classes (`votv-active-events-registry-RE-2026-07-04.md` census),
and the wiki taxonomy (Story Mode / Ariral-reputation / signal-triggered / random / dreams
/ player-activated / time-related / endings — voicesofthevoid.wiki.gg/wiki/Events).

| Event (row / class) | Doc | Granular pass |
|---|---|---|
| piramid (`piramid2_C`) — devs'-gauntlet acceptance case | [piramid.md](piramid.md) | AS-BUILT 2026-07-04 late (v97 lane; autonomous e2e PASS: mirror + brain suppression + gather replay dist=9495 + dead-retire END); hands-on visual pass pending |
| obelisk (`obelisk_C`) — Phase 0 registry-probe exemplar | (todo) | BEGIN/END proven live; doc pending |
| wisps (`wisp_C` swarm) + killerwisp — creature lanes shipped 07-03 | (todo) | AS-BUILT in code; doc = record the verdicts |
| starRain (`skyFallingEvent`) — event_cue lane | (todo) | known late-join gap (COOP_EVENT_JOIN.md) |
| everything else (65 rows + non-row registrants) | — | queue below |

Queue order = the wiki Story Mode ladder first (the scheduled spectacles every save hits:
warning obelisk D25, black-hole sun D27, piramid D31, rozital scouts D33, mothership D38,
gray firetank D44, gray invasion D47), then signal-triggered + Ariral-reputation, then
creature controllers (lane-covered — docs record verdicts), then pranks/ambience
(mostly host-local by design). Each finished doc adds its row to this table.
