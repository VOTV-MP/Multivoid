# The developers' gauntlet

The developers of *Voices of the Void* published the statement below on why multiplayer mods
fail. It is kept here whole as the project's north star: every claim in it is a testable
engineering assertion, and this page maps each claim to the architecture that answers it. The bar
they set, "safely and consistently", items and events included, mid-event joins included, is the
bar this project builds to, and they said they would endorse a mod that reaches it.

## The statement

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

## Claim by claim

| The claim | The answer |
|---|---|
| "requires a total re-build of the game at a foundational level" | A premise the project rejects by construction: augment single-player, never replace it. No game file is modified; the mod is an engine layer with its own reflection and hooks that drives the game's own systems ([ARCHITECTURE.md](ARCHITECTURE.md)). |
| "they all hit the same snag, event synchronisation" | The snag is real and was hit here too, then root-caused: the game's events fire from a local scheduler inside Blueprints with no replication hook. The answer is a host-observed fire replayed per row, a mirrored registry of in-flight events for late joiners, and one lane per event actor ([events-and-weather.md](events-and-weather.md)). |
| "the moment items came into play the game fell apart" | Items are the entity-identity problem. The answer is one identity per actor assigned at birth, a host-owned name across every transition, and a join that streams the host's world and reconciles the rest ([props.md](props.md), [piles.md](piles.md), [join.md](join.md)). |
| "people disconnected" | Sessions run on a reliable transport with ordered channels, heartbeats and a clean rejoin. A disconnect on desync is a symptom of state the peers stopped sharing, and every lane names its owner. |
| "the security aspect" | Taken as a design position rather than a feature: every inbound payload is length-checked and range-validated at the trust boundary, a client acts by intent and never authors host state, and what the mod does and does not protect is stated on [../SECURITY.md](../SECURITY.md). |
| "safely and consistently, we'll happily endorse" | The acceptance bar. Consistency is measured system by system: every claim in these docs names its evidence, and [STATUS.md](STATUS.md) says how far each system is. |

## The hard case: joining during an event

The strongest implicit test in the statement is a player joining while an event runs. The game
itself refuses to save during an event (the pause menu disables its save button while one is
active), which is the game saying its own save format cannot represent an event in flight; so a
mid-event join can never be "send the save". It is the save as the
base world, plus every lane's current state, plus a snapshot of the in-flight events that tells
the joiner what to replay ([join.md](join.md)). The walking pyramid is the acceptance case.
