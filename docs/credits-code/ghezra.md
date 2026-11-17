# Ghezra

One contributor's code, from the [credit ledger](../credits.md#the-ledger).

- **A beam with a hole in it** ([#71](https://github.com/VOTV-MP/Multivoid/pull/71)): a remote player's flashlight cone starts inside its head, and the game
  turns shadow casting off on a real player's two body meshes but a puppet kept the class default, so its body
  cut a flickering silhouette out of its own beam on every other screen. The puppet's spawn now switches both off
  through the engine's setter. The commits are kept as written; the beam drill logs the flags, `CastShadow=1` on
  both meshes without the fix and `0` with it.
- **A battery that ran out for one peer only** ([#79](https://github.com/VOTV-MP/Multivoid/pull/79)): at empty the game's tick clears the flashlight bool and
  hides the light without calling a function, so nothing went out and the others' puppet stayed lit. The sender
  now compares the bool with what it last sent. Kept as written; the drill was made to read only its own run's
  signals and to wait on the game instead of fixed delays (the second of two runs in one folder had judged the
  first run's signals), and it fails both directions without the fix.
- **A header for the linker** ([#66](https://github.com/VOTV-MP/Multivoid/pull/66)): `SteamNetworkingIPAddr::ToString`'s inline body lives in
  `isteamnetworkingutils.h`, which `session_receive.cpp` now includes instead of relying on another file to.
- **The desk's signal view** ([#81](https://github.com/VOTV-MP/Multivoid/pull/81)): the two bounds of `remapValue` and `visualizerMode` were polled by
  nobody, so each stayed on the machine that set it and a joiner got none. They now ride the desk input
  lane and the joiner's desk state (build 265), applied through the game's `updRemap` and `setCanvas`.
  Kept as written; its drill was moved onto a shared peer-signal helper that reads only its own run, and its
  joiner now waits for the host's state before reading it (a read one second early had looked like a miss).
- **A dish's slew, heard whole** ([#82](https://github.com/VOTV-MP/Multivoid/pull/82)): a client's dish is a parked copy moved by the host's poses, so it
  played one motor sound where the host's body plays a hum, the motor, a clunk per axis and a fading end. The
  client now plays that sequence from the edges in the stream it already applies. Kept as written; the hum now
  starts on the stream's moving flag, as the host's starts at the slew's order before its random wait, and the
  drill judges the client's sequence against the host's itself.
- **A save every player hears of** ([#69](https://github.com/VOTV-MP/Multivoid/pull/69)): a client's world save is blocked, so the game's SAVED indicator
  showed on the host alone. The host now hears its own world write and the game's refusal during an event,
  and every player gets one chat line per attempt (a new host-only kind, build 266). Kept as written; its
  drill now counts the notices and judges the client's against the host's and against its own steps.
- **A spill every peer sees** ([#74](https://github.com/VOTV-MP/Multivoid/pull/74)): a cup, a beer, a wine or a gas bottle spills through a projectile
  each peer flew on its own, so the stains landed apart or not at all. The host's decal, splash and pour now
  reach every peer (four kinds, build 267). Kept as written; two holes were closed on top: the host's copy of
  a prop another player holds -- a pose-driven hold or the mirror of the item in that player's hand -- no
  longer spills a second stain of its own, and a spill the host makes while no client is connected still
  reaches a client who joins later. The drill was moved onto the shared helpers, its joiner walks to the host
  first, and its rejoin leaves as a quit so the host's absent spill is measured.
- **A vent crawl every peer sees** ([#78](https://github.com/VOTV-MP/Multivoid/pull/78)): the crawler's crawl, doors and keypads already crossed,
  but what its own graph plays -- the vent steps, the crawl's steps, the metal break and burst, the
  signal room door's power and the vent's swing -- played on the host alone. The host now cues each
  peer when its box is entered, and every client plays the sequence itself. Kept as written; on top,
  a joiner is told where a crawl stands (one host-only kind, build 269) and finds the vent at the
  host's own end pose, the host's record of the crawl outlives its last client leaving, and the
  three times cite the blueprint's own delays. The drill was moved onto the shared helpers, walks
  into the box instead of teleporting, and rejoins after the crawl.
- **One holder per prop** ([#68](https://github.com/VOTV-MP/Multivoid/pull/68)): two players who grabbed one prop both streamed it, and every other
  peer ran two drives on one copy. A peer now lets its own grab go when another hold already drives the
  prop, with the deny click and a line naming the holder, and the host keeps the first hold and refuses
  a later one (one host-to-all kind, build 271). Kept as written; the two-peer grab race that
  measures it was moved onto the shared helpers: it names its target and its go instant through the
  drill signals, takes each peer's role from the session, and judges itself, allowing the refused hold
  its one round trip before the host's refusal arrives.
- **A can's lid and contents on every copy** ([#67](https://github.com/VOTV-MP/Multivoid/pull/67)): a can opened or eaten from on one machine stayed
  shut and full on the others until the next join. A can's change now publishes its record, a client's
  through the host, and a record landing is not echoed back (no wire change). Kept as written; on top,
  the lid's flag is read through its own bit, and the drill was moved onto the shared helpers: the
  client walks to the host, every step waits on the can's state, and the host judges both peers' rows.
- **A food's one author** ([#76](https://github.com/VOTV-MP/Multivoid/pull/76)): a food's tick rolled its own temperature and spoilage on every peer and
  burnt it on each. The host is now a world food's one author and a client's copy takes its values; a
  client's bite and its let-go of a held food reach the host, and a held food stays its holder's (four
  kinds, build 272). Kept as written; on top, a client's world food is refused its tick at the script
  gate instead of parked by the tick switch (which the game turned back on), a bite and a let-go are
  judged within reach and bounded, a refused one is answered with the food's own state, the let-go is
  taken at the drop instead of polled, and the drill was moved onto the shared helpers with a
  placed-down leg. The shape follows ElvardFIC's and AquaFishhh's food lane, below.
- **One set of ambient creatures** ([#83](https://github.com/VOTV-MP/Multivoid/pull/83)): each peer's spawners rolled their own deer, walking
  trees, hexahive and sus holes, and beehive branches. A client's spawners lose their tick and its
  beehive timer is paused; the host's deer and trees are mirrored by the npc lane, a tree's size and
  landing cross as one kind, and holes and branches cross by place as another (two kinds, build 273).
  Kept as written, except the host-side sus-hole roll around each client, which was not adopted; on
  top, a client's spawners are refused at the script gate, a sus hole is its player's roll made by the
  host, a branch is bound by its nest's birth key and the join ends the census, the ambient actors end
  at their end of play, a deer's mirror keeps its tick on a refreshed floor, and the drills were moved
  onto the shared helpers with a host verdict.
- **A woodchipper every peer runs the same** ([#88](https://github.com/VOTV-MP/Multivoid/pull/88)): a chipper's fuel and its on/off lived on
  whichever machine touched it, so the host's chipper, the one that eats the props, could stand off and
  empty while a client poured and pressed its own copy. The host's fuel and switch now reach every peer,
  and a client's pour and press go up as requests (two kinds, build 279). Kept as written; on top, a
  client's copy runs no tick and no intake (refused at the script gate), so only the host burns fuel,
  eats a prop and drops a clump. The host's state is sent from the game's own checkActive and button
  instead of polled, and the client's pour and press are read from the game's functions instead of a fuel
  threshold. The host judges each request (reach, rate, a living sender, the gas can in its hand, room in
  the tank, the state wanted) and answers a refusal to the sender alone. A joiner is told which chippers are
  on, and the drill was moved onto the shared helpers with a host verdict. A client cannot yet feed a prop
  to it: measured, the host's chipper did not eat a dropped paper, so that is a tracker line.
- **One state for every plant: the host's** ([#80](https://github.com/VOTV-MP/Multivoid/pull/80)): each peer's plants grew
  on their own water, sun and rain, and the winter bushes were planted on every peer. The host now runs
  every plant and sends its state; a client's player edits go up to it. Kept as written; on top, the
  host plants only the class its own plant table names and only within the sender's reach (a client
  could make it spawn any class anywhere), the family is every class under the plant base, the
  client's own tick, sprinklers, growth areas, fire and spawners are refused at the gate instead of
  parked, the growth area's glow crosses as a cue, an edit is read before a state lands on it, and a
  ripe bush keeps its berry count. The drill moved onto the shared helpers, each arm green and red.
- **A disc full of server data, and a module row bought by the host** ([#86](https://github.com/VOTV-MP/Multivoid/pull/86)): a disc of about 21
  dish rows passed the slot lane's 32 KB ceiling and the save record's, so the box stopped syncing and an
  eject left the host a blank disc; and a client's press on a module row of the upgrade panel paid on its
  own machine alone, until the host's next balance put it back. Both ceilings were raised to one blob, and
  the press became an intent the host runs on its own row (build 282). Two of the PR's three commits are
  kept as written; on top, one ceiling for a disc record, defined once and used by the sender and the
  receive (56,092 B), with a record over it refused at the receive and its size logged. A full box disc at
  32 writes measured 48,982 B and a laptop disc at 31 writes 47,460 B, both under it. The host presses
  a module row only for the laptop's holder and at a rate limit, a refused press is answered to the sender alone with the host's
  balance and levels, and the storeless rows charge their price, as row 23's 30 credits measured. The two drills were
  moved onto the shared helpers, wait on the peers' state, are judged on the host, and have a join arm and
  a red each. The third commit (two discs put in one slot end as a swap) waits: the loss it answers comes
  from the disc slots being read once a second while the game announces both acts, and a swap on top of
  that poll would extend it. The slots move onto the game's insert and eject events first, then the swap
  is re-judged and the request stays open until then. A zip disc (1024 writes) still exceeds the ceiling
  and is refused with its size logged; carrying it needs chunked records, a tracker line.
- **A toolgun's flags, the host's** ([#85](https://github.com/VOTV-MP/Multivoid/pull/85)): the toolgun's prop-properties tool sets a prop static,
  frozen or asleep on the pressing machine alone, so a client's freeze held only on its own screen. The PR
  made a press cross, and the host's copy and every other peer's followed (two new kinds). One of
  the request's four lanes is kept as written, the tool-flags piece; the other three (rest and settle,
  fire, the join sweep) come in their own packages and the request stays open. On top, the tool's own press
  is the seam instead of a watch on every class's init: a client's press is cancelled and goes up as a
  request, and the host judges it like the remover (the toolgun permission, a toolgun in hand, sight, and
  another peer's hold) and publishes the flags it applied, read back from the prop. A static prop reaches
  a joiner through the game's own setter, and classes that override it (a landmine, a TV) keep their own
  state. Measured first: the tool's press froze a prop from 0,0,0 to 0,1,0, and the dumps show only the
  tool and the stacker setting these flags. The drill has five arms (freeze, make-static with a rejoin,
  refused without the permission, refused out of sight, a landmine) and two reds.
- **Room for more message kinds** ([#117](https://github.com/VOTV-MP/Multivoid/pull/117)): the reliable lane named a message's kind in one byte, and
  its 256 values were nearly spent. The kind now takes two bytes in the header's first pad byte, so the header
  stays eight bytes and every existing kind is the same bytes on the wire. Kept as written; on top, every
  per-kind table and check covers the new space, a kind past it is said once instead of vanishing, and the
  CI drill and the kind gate read the wider enum.
- **An opened MRE's contents** ([#108](https://github.com/VOTV-MP/Multivoid/pull/108)): a client's MRE open rolled and
  spawned the contents on that client only. Now the host opens it with the MRE's own spawn, so the contents are the
  host's births on every peer. Kept as written; on top, the slot's granted verb is set and cleared with its reservation.
- **A zip disk dropped in the drone sack** ([#111](https://github.com/VOTV-MP/Multivoid/pull/111)): a client's
  disk dropped into the drone sack was filed on that client only, and the sack's call destroyed the prop,
  taking the host's copy with it unsold. Now the client's put from the sack is cancelled and the host, which
  runs the same overlap, files it. Kept as written; on top, the cancel lists none of its callers: on a
  client every overlap put into a shared container is cancelled, the roomba's own pickup the one named
  exception, and the drone sack drill's arm 2 carries its prop over the sack and says its verdict.
- **One walking mannequin, the host's** ([#84](https://github.com/VOTV-MP/Multivoid/pull/84)): each peer's mannequin spawner rolled its own walkers and
  each copy ran its own brain, so a client's walker drifted from the host's and hunted whoever it liked. The PR froze the
  host's walker on a client's view and steered it toward the nearest player (two kinds). Kept for its authorship and for
  the hit it delivers: a walker's hit lands on the client it strikes, a ragdolled puppet is spared, and the host's copy of
  the hit sound is held back. On top, a client refuses the mannequin spawner at the script gate and its copy runs only its
  begin-play and its destroy, with a nav pawn that takes no controller; the host's walker moves its copy on the drive
  stream, claimed at its network id; a walker hunts one player and keeps it until that player leaves, dies or leaves its
  range; it freezes on its target's gaze only, the host answering the walker's own gaze reads with that client's level;
  its pose and target cross on their edges and to a joiner (two kinds, build 283); and the drills became one drill on the
  shared helpers with a host verdict, seven steps and a red arm for each mechanism.
- **One timer clear for six callers** ([#105](https://github.com/VOTV-MP/Multivoid/pull/105)): six hand-built copies of the engine's timer clear became one helper. Kept as
  written; on top, the helper resolves the engine's function on every call until it
  is found, so no caller can read another caller's failed attempt as its own.
- **A drill keeps its players fed and rested** ([#100](https://github.com/VOTV-MP/Multivoid/pull/100)): a long drill's players fainted from hunger and sleep before it ended. Kept as
  the idea; on top, it became a dev switch of the existing vitals keepalive, so each
  player refills its own food and sleep through the game's vitals, with no second
  helper beside it.
- **A joiner's empty equipment slots** ([#114](https://github.com/VOTV-MP/Multivoid/pull/114)): a joiner's empty equipment slots were written as zeroed structs the game never
  reads as free, so a player could wear nothing new. Kept as written; on top, the
  blank is read from the save object's own defaults rather than typed, and the drill
  fails only on the game's own "Not enough space" answer.
- **A knocked-down pinger frees the desk** ([#97](https://github.com/VOTV-MP/Multivoid/pull/97)): a pinger knocked down or killed at the desk kept holding it. Kept as written; on
  top, a death frees it too, through a life edge that now also fires for a player
  already dead when their copy first appears, once that player's world is ready, and
  the drill decides each arm at the host's reconcile rather than on a timer.
- **An ATV's fuel, modules, controls and horn** ([#89](https://github.com/VOTV-MP/Multivoid/pull/89)): a client's refuel,
  repair, module fit or take-off, its lights, brake and turbo, and its horn changed that
  client's copy of the ATV only, and each peer burnt its own fuel. Now a client's act is
  asked of the host, which judges it, applies it and publishes the result to every peer;
  the driver's controls and horn ride its stream, and the host takes the driver's burn only
  from a block that has seen the host's latest state. Kept as written: the host as the ATV's
  one author, the module refund of a refused fit. On top: every request is answered, a client
  holds the ATV's state until its answer comes, a request from a player still loading waits
  until they are ready, the horn is a level in the stream, and a drill with a red arm for
  each mechanism.
- **A first aid kit a client unpacks** ([#119](https://github.com/VOTV-MP/Multivoid/pull/119)): a client's unpack of a first
  aid kit ran there, so its contents stood on that client alone; now the client's unpack is
  asked of the host, which runs the kit's own spawn on its copy, so the contents are the host's
  on every peer. Kept as written. On top: the kit shares the MRE's unpack path (its
  achievements, its roll tally, the opening heard after the destroy), and the act drill's kit
  step proves every rolled content reaches the client.
- **A client's hovering drone** ([#96](https://github.com/VOTV-MP/Multivoid/pull/96)): a client's copy of the delivery drone ran its
  own physics between the host's poses, so a hovering drone shook and drifted; now the copy
  leaves the physics simulation and plays the host's poses back on the host's clock. Kept as
  written: the pose stamped with its send time, the playback delay. On top: each drone frozen
  on its own, the freeze given back if the engine refuses it, and a drill whose red puts the
  copy back into physics and fails on its drift from the hover.
- **Leaving from the pause menu during an event** ([#124](https://github.com/VOTV-MP/Multivoid/pull/124)): the pause menu would not let a player
  leave while a world event ran; now its leave runs, with the event's gate answering only the
  menu's own call, and the video camera's viewfinder hint follows the camera in hand. Kept as
  written. On top: the leave watch binds its classes once, and drills with a red for each.
- **The base computer's short circuit** ([#95](https://github.com/VOTV-MP/Multivoid/pull/95)): a liquid spilled on the base computer
  shorted it on one peer alone; now the short circuit is the host's on every peer, and a
  joiner sees a desk that is already shorted. Kept as written: the host's flag and its
  publication. On top: the game's own call is watched instead of a poll, a client's short is
  asked of the host and judged (alive, in reach, within a budget) before any desk sparks, and
  a drill that walks to the desk with a red for each side and the join.
- **The sky teleport's, a faucet's and the alien sounds** ([#104](https://github.com/VOTV-MP/Multivoid/pull/104)): the earth teleport's sky sounds, a
  faucet's click and the alien noises played on one peer alone; now each crosses to every
  peer, and a joiner hears the alien noises that are still out. Kept as written: the sounds
  matched by the game's own sound assets, the faucet click on its flip. On top: the assets
  found once per world, the alien replay counted per event, and a drill with a red for each
  sound and the join. Its harvest half waits for the harvest lane.
- **A client's own nightmare** ([#125](https://github.com/VOTV-MP/Multivoid/pull/125)): a nightmare was the host's alone; now a
  client sleeping in a bed rolls its own nightmare, once per rest, from the bed's own odds, and
  its puppet hides on the other peers while it dreams. Kept as written: the game's own wakeup
  and dream calls. On top: the roll and the hide ride the sleep lane's events, and a drill
  with a red for the roll and the hide.
- **The cargo lift** ([#118](https://github.com/VOTV-MP/Multivoid/pull/118)): the cargo lift moved on each peer alone,
  so a client could ride a lift the host never moved; now the lift is the host's, on every
  peer, and a client's press is asked of the host. Kept as written: the host as the lift's one
  author. On top: a refused press answered, a client's copy that never reverses on its own, the
  kinds renumbered onto the two-byte range, and a drill with a red.
- **An anchored hook a client takes back** ([#112](https://github.com/VOTV-MP/Multivoid/pull/112)): an anchored hook's
  client copy had its collision off, so a client could not take its own hook back; now the client's
  pickup is asked of the host, which takes the hook and gives the item to the one who asked. Kept as
  written. On top, the take-back has its own message kind and build, a joiner loses a hook taken
  while it loaded, and the drill proves each case.
- **A prop's nametag** ([#113](https://github.com/VOTV-MP/Multivoid/pull/113)): a nametag put on a prop showed only on the peer that clicked it; now a client's tag is asked of the host, which puts it on its copy and tells every peer. Kept as written; on top, the tag has its own message kind and build, a joiner gets the tags put on while it loaded, and the drill proves each case.
- **A fuse or an upgrade a client installs from its hand** ([#110](https://github.com/VOTV-MP/Multivoid/pull/110)): a client's fuse insert or generator upgrade from its hand was refused and rolled back above the easiest difficulties, because the host only took an install against a spend it saw in the world; now the host takes it against the hand the client holds. Kept as written; on top, the two time windows became the hand lane's own facts (the last held item and one use per hold, judged as the request arrives, the judge the ATV already used), an item pays for one install only, and the drills prove a forged and a doubled install refused.
- **The refiner's cap label** ([#106](https://github.com/VOTV-MP/Multivoid/pull/106)): the refiner's cap label (the cap button's tag and the screen's
  level) showed a different level on each peer; now every peer repaints its own label from the
  synced cap level and module, a joiner included. Kept as written: the repaint at the cap press
  and the module plug. On top: an owed repaint runs at the desk's own BeginPlay instead of a poll,
  the label is repainted once when the lane starts, and a drill with a red for the repaint.
- **A backrooms trip is one player's** ([#126](https://github.com/VOTV-MP/Multivoid/pull/126)): a clip through the walls sent the whole
  session to the backrooms; now the trip is the clipping player's alone: the map loads below the
  running world for that player, the others keep playing, and the host can close the backrooms
  for everyone. Kept as written: the map loaded below the world, the host's switch, the visit
  keeping its controller, view and dropped item, the dark sky. On top: the visitor's copy hidden
  by its own pose (a joiner included), one owner for a copy's hidden flag, the host's switch held
  at the game's own clip check, the visit's exit guards for the whole visit, and a drill with a
  red for each mechanism.
- **The black orb is the host's** ([#127](https://github.com/VOTV-MP/Multivoid/pull/127)): the orb, its encounters and its laps ran on each peer apart, so a
  client met it only after a rejoin; now the host runs it and every peer, a joiner included, shows the host's.
  Kept as written. On top: the orb keeps its laps instead of looking them up again, a catch it cannot revive
  is spared, and a drill that moves no player, with a red for the encounter's start.
- **The ambient UFOs are the host's** ([#128](https://github.com/VOTV-MP/Multivoid/pull/128)): the UFOs crossing the sky were each peer's own; now the host's
  fly on every screen. Kept as written. On top: a UFO's path follows its spawn instead of a rescan, a joiner
  meets the UFOs a leave had drained, and a drill with a join arm and a red.
- **The earth teleport's sky** ([#129](https://github.com/VOTV-MP/Multivoid/pull/129)): the teleport turned the sky only for the player who used it; now
  every player's sky turns. Kept as written; on top, a drill with a join arm and a red.
- **The robot cleaner is the host's** ([#130](https://github.com/VOTV-MP/Multivoid/pull/130)): the cleaner ran on each peer apart; now the host's runs on
  every peer and a client's switch press is asked of the host. Kept as written. On top: the switch is watched
  instead of polled, a press made before the host's copy exists is held, the overlap check does not exempt
  the cleaner, and a drill with a join arm and two reds.
- **A selftest every tick** ([#163](https://github.com/VOTV-MP/Multivoid/pull/163)): the life-edge selftest's caller is the install pump, which runs every tick, so
  its result was logged 61 times a second, four lines in five of a session's log; the checks are pure, so it runs
  once and keeps its answer. Kept as written.
- **The log's date** ([#164](https://github.com/VOTV-MP/Multivoid/pull/164)): a line's stamp is the local time of day alone, so the log now says its date and UTC
  offset in its header and again where either changes. Kept as written; on top, the change is also seen at a
  daylight-saving fall-back, whose hour repeats.
- **The woodchipper eats a client's prop** ([#98](https://github.com/VOTV-MP/Multivoid/pull/98)): a prop a client held or threw into a
  woodchipper was refused, because the host's copy of a prop a client drives does not simulate; now the host's chipper
  takes it as the game's would. Kept as written. On top: no client message is needed (the host sets its copy simulating
  for the chipper's own intake check, and back if it is not taken), and a drill with a held arm, a thrown arm and a red.
- **An ATV's riders** ([#92](https://github.com/VOTV-MP/Multivoid/pull/92)): a remote player driving an ATV did not sit on it on the other
  screens, and the host's kerfur that rides along was not on the ATV on every peer. Now the driver's body sits at
  the seat the game itself uses on every other screen, the kerfur rides on every peer, the same ATV the host
  names, and a joiner mid-ride sees both. Kept as written: the host running the ATV's own kerfur steps for a
  client's seat, the AnimBP seat pose, the kerfur helpers. On top: the seat and the rider are the host's and
  are announced when they change, the body and the kerfur are attached once instead of placed every frame, the
  host names the rider in the ATV's stamp instead of each peer guessing the nearest ATV, a client's game never
  invites or unseats a host-owned kerfur, and a drill with a red arm for each mechanism.
- **Containers and the hacksaw are the host's** ([#93](https://github.com/VOTV-MP/Multivoid/pull/93),
  [#94](https://github.com/VOTV-MP/Multivoid/pull/94)): a container a client broke or sawed broke only on its own screen,
  a container inside another lost its contents, a signal's photo did not travel, and a garbage container showed each
  peer its own contents. Now the host breaks it and spills its contents, the nested contents and the photo travel with
  the container, and every peer sees the host's garbage. Kept as written. On top: a client's cut is asked of the host
  before the saw finishes and runs only when granted (a refused cut shows the game's own hint), the garbage is
  announced instead of polled, a record too big is refused whole instead of cut, and drills with a red for each piece.
- **An ATV's battery** ([#140](https://github.com/VOTV-MP/Multivoid/pull/140)): a battery a client fitted into an ATV, or unscrewed from it, changed
  that client's copy only, and the host's copy, the one the save keeps, stayed as it was. Now the host judges the fit
  and the unscrew, and every peer's ATV shows the host's battery. Kept as written: the fit and the unscrew as requests,
  the battery in the host's ATV state, the game's own repaint. On top: the host spawns the battery an unscrew hands back,
  at its own charge, so no client makes one; a refused fit gives its battery back; a joiner gets the battery; and a
  drill with a red for each mechanism.

- **A can's state, dropped and held** ([#136](https://github.com/VOTV-MP/Multivoid/pull/136)): a can a client opened or ate from and then dropped came
  back closed and full on every other peer, and an item in another player's hand showed its default state. Now a dropped
  can keeps its lid and contents, and a held item's record goes from its holder to the host, which relays it to every
  other peer's view of that hand. Kept as written: the can's record carried at the drop and adopted by the new actor,
  the record applied to the hand's view. On top: the host judges each record (its class against the holder's hand,
  every class it names resolving, size and field bounds, finite numbers) and paces it, a joiner gets every held item's
  record, the record is read when the game changes the hold rather than polled, and a drill with a red for each
  mechanism.

- **A glowstick that stays stuck** ([#162](https://github.com/VOTV-MP/Multivoid/pull/162)): a glowstick a client stuck to a wall, the floor or a prop hung
  free on every other screen, and the host's copy, the one the save keeps, never stuck. Now the stick goes to the host,
  which sticks its own copy where the client struck and every peer shows it. Kept as written: the stick sent as the
  glowstick's record, the host's copy stuck from it. On top: the host judges each stick (the player's own last hold,
  its reach, a surface under a stick to the world, the struck prop's frame for a stick to a prop) and answers a refused
  one with its own record so the client's copy comes free; a stick to a prop follows the prop; a stuck glowstick taken
  again is freed on the host first; a joiner gets every stick, including one made while it loads; and a drill with a
  red for each mechanism.

- **Sleep is everyone's** ([#142](https://github.com/VOTV-MP/Multivoid/pull/142)): the host's sleep sped the world
  up for players still awake. Now the host chooses: only when everyone sleeps (the default), whenever the host sleeps, or
  never. Kept as written: the rule that no awake player is sped up. On top: the three-way host setting, its speed declared
  on every sleep, wake, join, leave and setting change rather than read each tick, and a drill with a red for each mode.
