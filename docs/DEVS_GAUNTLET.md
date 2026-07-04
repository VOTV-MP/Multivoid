# The devs' gauntlet — VOTV developers on multiplayer (saved 2026-07-04)

**Why this doc exists:** the VOTV developers published the statement below about
why multiplayer mods fail. We keep it verbatim as the project's north star. Every
claim in it is a testable engineering assertion; this doc maps each claim to the
architecture that answers it. The bar they set — "safely and consistently",
items + events included, mid-event joins included — is the bar we build to.
They explicitly said they would **endorse a mod that achieves it**. That is the
goal line.

## The statement (verbatim)

> A lot of people assume you can just tack multiplayer on and it will just work,
> that's further from the truth.
>
> To get a function like multiplayer to work in such a way would require a total
> and complete re-build of the game at a foundational level, and even then, it
> still may not work as intended.
>
> Mod developers have been attempting this for years, and they all hit the same
> snag, event synchronisation.
>
> Some mods achieved getting people in the same lobby on the map, but that was
> it, the moment events or items came into play the game fell apart, people
> disconnected, people saw completely different events or nothing at all.
>
> So, rebuilding the entire game for 1 feature is not only a complete waste of
> resources and time, its also a death sentence for a project like this.
>
> Lets say we did all that work, and it turns out that no one actually liked the
> multiplayer ... We can't just undo all that time, effort and resource
>
> Then there's the security aspect, all I'll say in regards to that is
> Webfishing, you can have the greatest intentions but if you do it wrong, you
> put people in harms way and your game is DOA.
>
> I wish mod developers all the luck in the world trying to find a way to get
> multiplayer to work, and we'll happily endorse a mod that achieves it safely
> and consistently, but its not something we're going to throw our time and
> resources trying to accomplish for potentially little to no return on it.

## Claim-by-claim: how this project answers

| Devs' claim | Our answer (code-verified state, 2026-07-04) |
|---|---|
| "requires a total re-build of the game at a foundational level" | False premise we reject by construction: **augment SP, never replace it** (principle 6). No game file is modified; the mod is an engine layer (proxy DLL + AOB reflection + ProcessEvent detour) that routes per-player inside SP systems. MTA did exactly this to GTA:SA — a fully SP game — at multi-thousand-peer scale, for 15+ years, without a rebuild. |
| "they all hit the same snag, event synchronisation" | The exact snag is real and we hit it too — and root-caused it: SP events are fired by a local scheduler (`saveSlot.settime` -> `eventer.runEvent`) with **zero replication hooks**. Our answer is the event lane: host is the only firer (client `allEvents` suppressed), host observes fires via the `passEvents` growth seam, broadcasts `EventFire`, clients replay per a **per-row dupe-matrix policy** (69 rows classified replay / lane-owned / host-local — `event_fire_sync.cpp`). "People saw completely different events" is precisely what the dupe matrix prevents: a row whose outputs ride an entity lane is never double-fired. |
| "the moment items came into play the game fell apart" | Items = the entity-identity problem. Answer: stable-ID identity layer (874/874 native bind + ordinal sidecar), one-owner delivery axis (`prop_snapshot`), grab/throw/hold sync, pile nativization, inventory wire, order sync. The dupe matrix + join-window reconcile are the anti-"fell apart" machinery, each instance verified hands-on. |
| "people disconnected" | Sessions are GNS (Valve GameNetworkingSockets) with reliable ARQ + unreliable pose streams, heartbeats, clean rejoin (join-window identity root fix, purge-aware sweeps). Disconnect-on-desync is a symptom of state divergence; the architecture makes the host authoritative so divergence converges instead of compounding. |
| "security aspect ... Webfishing" | Taken seriously, not hand-waved: every inbound payload is length-checked at the dispatcher trust boundary, NUL-bound, range-validated; clients cannot drive host authority (host-authoritative lanes refuse client sends — `session_lanes.h`); save transfer is CRC'd + torn-read-guarded; no remote code paths, no eval, no asset loading from the wire. Security audits ride the standing agent-audit discipline. |
| "safely and consistently ... we'll happily endorse" | The acceptance bar. Consistency = the verification culture: nothing is "working" from a smoke; hands-on verdicts per runbook, probe-don't-guess, docs that say AS-BUILT vs VERIFIED honestly. |

## The hard case we build to: join DURING an event

The devs' strongest implicit test: a player joins mid-pyramid (or any active
event). This is where "same lobby but different worlds" mods die.

**The native constraint that shapes the design (user-confirmed 2026-07-04, gate
location being verified in bytecode):** the game itself NEVER allows saving
during an event. That is the game telling us its own save format cannot
represent an in-flight event. Consequence for coop: the join save-transfer
(live world capture) is inherently insufficient DURING an event — by the
game's own admission, no save blob can carry that state. So mid-event join
can never be "just send the save": it is save blob (the stable base world)
**plus** per-lane live snapshots (creatures, props, weather, time, cues)
**plus** an active-event snapshot lane that tells the joiner which events are
in flight and how to converge.

Design + as-built status: `docs/COOP_EVENT_JOIN.md` (the join-during-event
contract — every lane must answer "what does a late joiner receive?").

## Timeline commitment

User's stated goal (2026-07-04): a proper, robust multiplayer in ~3 months,
built under the standing rules (RULE 1 root-cause-only, MTA precedent, no
game-file edits, host-authoritative, verify-don't-guess). This doc is the
scoreboard we return to.
