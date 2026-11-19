# petmav

One contributor's code, from the [credit ledger](../credits.md#the-ledger).


- **The joiner's stutter** ([#157](https://github.com/VOTV-MP/Multivoid/pull/157)): after the loading screen dropped, the load-tail census and the NPC and kerfur
  twin scans walked the whole object array, 30 to 40 ms a pass, several passes a second; they now read the object
  index's per-class lists, and the array only while the index trails the engine. Kept as written; on top, a scan
  that may spawn a mirror for a missing twin brings the index fully up to date first.
- **Why a signal was refused** ([#158](https://github.com/VOTV-MP/Multivoid/pull/158)): the signal lanes now say why a saved signal or a deck selection was refused,
  a signal row's identity does not depend on the case two peers' engines spell its names in, and a dev census
  counts what a peer receives by message kind and bytes. Kept as written; on top, the census is a dev setting like
  its sister and counts every message kind.
- **More than four players** ([#152](https://github.com/VOTV-MP/Multivoid/pull/152)): the player cap went from four to eight, the per-player ack arrays of the
  power panel, grid and fuse towers with it, and the master's lobby seats to match. Kept as written (its 0.10.0-only
  scope text left out); on top, it became the start of the player capacity work: the ceiling sixteen, the host's own
  limit (two by default), a slot-set type for every per-player mask, and a load-peer tool that proved all sixteen
  seats; its automatic second ATV was not taken (a cheat in the single-player world).
- **Slower hunger in larger groups** ([#154](https://github.com/VOTV-MP/Multivoid/pull/154)): a host rule, off by default, that slows each player's hunger as the
  session grows (74% for two, 54% for four), written through the game's own hunger multiplier. Kept as written; on top,
  it follows the roster and the vaccine effect's end instead of a once-a-second poll, and says its rate in the log.
- **Cheat panels in F1** ([#146](https://github.com/VOTV-MP/Multivoid/pull/146)): the host got its tools in the F1 menu (spawn any
  listed prop by name, points, events, entities, snow, clock, vitals, freecam, Unstuck) and players got theirs as requests the
  host checks, behind a host setting that is off by default. Kept as written; on top, a player's cheats are also a right the
  host can withhold from one player, players may fire only safe non-story events and spawn no explosive, a request costs its
  count, teleports land on the NavMesh, the item list builds when the game says it is filled, and a drill with a red for each
  rule.
- **A prop at rest alike on every screen** ([#156](https://github.com/VOTV-MP/Multivoid/pull/156)): a prop a player dropped, set down or let go of without a
  throw settled by each player's own physics, so it came to rest in a different place on every screen, and a damaged
  extinguisher could run away on one screen only. Now the host flies a let-go prop to its rest and every copy follows,
  and the host owns a runaway extinguisher's flight. Kept as written: the host's coast of a let-go, the runaway's host
  claim, and the log of every correction. On top: the cap shared by every kind of coast, a client's own extinguisher
  kept from running away, and a drill with a red for each mechanism.
- **An oven's screws and lid** ([#150](https://github.com/VOTV-MP/Multivoid/pull/150)): the first steps of an oven's repair, four
  screws and the lid, stood on one player's screen only. Now each step reaches every peer and a joiner. Kept as written: the mask
  of steps, raised and never cleared, and the connect replay. On top: a step is read when the game repaints its screen, ovens come
  from the oven channel's own index instead of a scan, a mask waits for an oven whose screen does not exist yet, and a drill with a
  red. The rest of this request (pries, the ATV's wheels and spare) is adopted part by part.
- **An ATV's wheels and spare** ([#150](https://github.com/VOTV-MP/Multivoid/pull/150)): a client unbolting, fitting or stowing a
  wheel, or taking the spare, changed the ATV on its own screen only. Now each is an ask the host judges (the sender's reach, the
  tool or wheel in its hand, the ATV's state) and runs on its own copy. Kept as written: the asks and the judge. On top: the battery
  half is left to the battery lane already on main, the hand is read the way every judge reads it (the tool by its class), a
  refused part comes back only when nothing of the act ran, and a drill with a red for each judge.
- **A pried plank, radiator or security TV** ([#150](https://github.com/VOTV-MP/Multivoid/pull/150)): a client's crowbar pry
  freed the board on its own screen only. His commit sent the pry to every peer; on top, a client's pry is an ask the host
  judges (reach, the crowbar, not yet pried), the fall is the host's physics streamed to every copy, a radiator's or a TV's
  four joints break in the host's order on every peer, and a joiner takes the host's joints with the prop's row.
