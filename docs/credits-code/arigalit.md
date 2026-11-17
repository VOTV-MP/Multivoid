# arigalit

One contributor's code, from the [credit ledger](../credits.md#the-ledger).

- **The drone leave timer** ([#50](https://github.com/VOTV-MP/Multivoid/pull/50), its first piece): a
  client pressing the garage console's leave-timer face now sets the host's drone, and every peer sees
  the setting. The commit is theirs as written; the console's click and the drill's timer leg were
  added beside it.
- **The desk ping's scan progress** ([#50](https://github.com/VOTV-MP/Multivoid/pull/50), its second piece):
  while one player pings coordinates at the main desk, every other peer now sees the coordinate lines
  fill in stage by stage. Their two commits are kept as written (the notebook half of the second waits for
  the notebook piece); a measurement, the pinger's identity on the wire and the observer checks were added
  beside them.
- **The sleeping bag and mattress** ([#50](https://github.com/VOTV-MP/Multivoid/pull/50), its third piece): a
  bag a player unrolls or folds stays on the other peers (FI-11). The host runs the
  game's own transition for a client and the unrolled bag, which the game never keys, gets a key. Their
  commit is kept as written; the bunk-bed mattress, the press-side watch and the drill were added.
- **The notebook** ([#50](https://github.com/VOTV-MP/Multivoid/pull/50), its fourth piece): what one player
  writes in a notebook is now what every peer reads, one editor at a time. Their two commits are kept
  as written; the text was then routed through the notebook's own save record, which already crossed,
  and the editor claim, the write rule and the drill were added beside them.
- **The revive's fire** ([#50](https://github.com/VOTV-MP/Multivoid/pull/50), its fifth piece): a player who
  died burning came back still burning. Their commit is kept as written; the fire is now put out with the
  game's own verb, and a drill sets the player alight, kills it and checks the revived body.
- **The cord's plug** ([#50](https://github.com/VOTV-MP/Multivoid/pull/50), its sixth piece): a cord one player
  plugs into a socket is now plugged on every peer, a joiner included. Their commit is kept as written; the
  link was then made the host's (a client claims its own plug and the host answers), and the drill and the
  forged-message checks were added beside it.
- **The coffee machines** ([#50](https://github.com/VOTV-MP/Multivoid/pull/50), its seventh piece): a client's press of
  the coffee vending machine or the coffee maker now takes its points and brews its cup, once, on every peer. Their
  commit is kept as written; the press was then made a request the host performs, the maker's state its record
  published by the host, and the drill and the forged-message checks were added beside it.
- **The Geom Octa** ([#48](https://github.com/VOTV-MP/Multivoid/pull/48)): the creature that falls out of the
  night sky and fires a laser is now the host's, and every peer sees the same one. Their commit is kept as
  written: the Octa on the NPC allowlist, the gamemode's night roll as a spawn source and the dev menu's spawn
  button. The rest of the pull request was rebuilt: the mirror's decisions are parked and the host's attack is
  relayed into each client's own Blueprint, a client's own roll is refused, a joiner mid-attack is replayed the
  attack, and the drill was added beside it.
- **The grappling-hook lane** ([#16](https://github.com/VOTV-MP/Multivoid/pull/16)): the first
  implementation of hook and rope visibility across peers. It could not be merged — a mirror is a
  live `hook_C`, `hook_C` is an `actor_save_C`, and the game's save walk collects by that interface,
  so every mirror standing on a peer would have been written into that player's save. The divergence
  was ours to own: the design it had to match is not in this repository at all, so no outside
  contributor could have read it. Four of its decisions are in the lane
  that shipped, and are credited on the commits: payload validation before anything is applied, a
  connect replay so a joiner is not left without the hooks already standing, the perf-bucket walk
  timer, and a mirror given neither collision nor tick. The `ue_wrap/actors/hook` split beside the
  lane is theirs too — the design names no wrapper at all.
- **ATV seat contention** ([#9](https://github.com/VOTV-MP/Multivoid/pull/9)): a
  peer walking up to an ATV somebody else is already driving is denied at the
  input seam, instead of both engines running vehicle physics and fighting over
  the body.
- **The admin's crematorium order** ([#57](https://github.com/VOTV-MP/Multivoid/pull/57)): an admin can
  send a player to the base cremator from the admin panel or the scoreboard. Their commit and its
  translations are kept with them as author. On top, the cremator became the host's machine: its lever
  run reaches every peer, joiners included, a client's pull of its handle is a request the host judges,
  the burn's hits are dealt in the burning player's own game, and a client's copy burns nothing. The
  order became the `/cremate` command with its own permission and exempt nodes; the host runs the
  machine's own lever instead of igniting the player itself, judges the trip from the player's poses
  and the burn volume, and kicks the cremated player with an end reason of its own.
