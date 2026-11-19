# Props in motion with no hand on them

A prop nobody holds can still move: a hook pulls it, a broom pushes it, a body walks into it, it is born falling, or a hand
lets go of it. This page is how the host streams such a prop to rest. The holding, throwing and dropping that precede it are
in [props.md](props.md).

## The set of driven props

A prop nobody is holding can still be in motion: a hook's constraint pulls it along behind the player, an anchored
hook ties it to something that moves, and after the hook lets go it slides until it rests. The held-prop stream never
sees it, since that stream is sourced from the player's grab slot alone, so the host keeps a set of the props the
hooks on its machine are tied to -- its own, the anchored ones it adopted, its mirrors of the clients' hooks, and the
save's and the level's own -- fed by the hook lane rather than by a per-frame walk, and streams each one's pose while
it moves (`coop/props/prop_drive_host`). Every hook's constraint exists on the host and nowhere else
([deployables.md](deployables.md)), so this one set is every prop any hook can move. A broom's push is the set's
second feeder: every stroke is the host's ([piles.md](piles.md)), and each prop a stroke pushes coasts under the
stream until it rests, while a prop a verb already holds stays that verb's (`coop/items/broom_push`). The trash a
stroke knocks out of a dispenser pile coasts too, from its spawn: the host catches the finish of each spawn the pile's
own `broomed` body makes, and the streams open after the tick's spawns are named, so each has its id by then. The
third feeder is the host's own births: a prop it spawns simulating -- a spawn-menu barrel in mid-air, a forage drop, a
prop built from a client's intent -- coasts from its birth until it rests, since every peer would otherwise simulate
its own fall and the copies came to rest apart; at most 32 props (`prop_drive_rules::kMaxCoasting`) coast at once, and
a burst past that falls locally, the refusals counted in a line (`prop_drive_host::CoastCapReached`). The fourth is a
let-go: a prop a hand releases without a throw, a client's or the host's own, coasts on the host from the release
until it rests, under the same cap (`prop_drive_host::CoastLetGo`). A fire extinguisher's runaway thrust is the host's
as well: a client's copy is refused its own, and the host claims one that goes off for as long as it thrusts
(`coop/props/fireext_thrust_sync`); a client sees the flight without the thrust's particles and sound. A prop a
player's body walks into is the fifth, and the player who bumps it syncs it, as MTA's push does: a client's push is a
claim (`PropSyncReason::BodyPush`) the host judges like a throw's election, so its copy streams and every other copy
is driven, while the host's own body makes the host coast a prop no row streams, under the same cap, and a client's
claim takes such a coasting row over; a claim the host refuses is answered with the host's pose, so the client's copy
parks where the host's prop stands. A puppet keeps its capsule's physics shape on the peer that shows it, so a prop that leans on a player rests on the puppet on every peer; the push itself is read from the pusher's own body on its own peer only. A claim costs the host no pose read, so the cap never refuses it,
and a client's rows of a push meet the throw's step test and cost trust, never display. Each peer reads the props
within a runner's second of its own player's body (11.5 m, the nearest 64) ten times a second, and takes a move of over 3 cm that is
more sideways than down, with a body within 1.5 m, for a push, unless a hand, a vehicle someone drives or a row
already has the prop (`coop/props/body_push`). It is a poll, under the rule's exception that nothing announces the
change: with a box dropped on the pushed woodchipper as a known hit and its body's hit-notify flag read on,
ReceiveHit counted 0 on the prop and on both pawns (an event its class does not implement never reaches the
dispatch hook), the prop's own hit handler counted 0 (the game binds it to the hit event only while its own
distance-sleep flag is set, never beside a player), OnComponentHit is a multicast no seam of ours sees and NotifyHit is
native `[V]` (the body-push drill's HITS lines, on both peers). A coast that rests while a peer is joining needs no
resend: the joiner's snapshot is cut after the rest and carries it `[V]` (the coast drill's join arm: the joiner's copy
rests within 1 cm of the host's with no End sent to it). A push or a runaway thrust in flight when a peer joins is the
same stream, which every drive row of the prop joins the peer to, so nothing more is owed `[?]` until a join arm reads
it. A prop that a pushed prop knocks, or whose support is taken away, is the settle watch's below. Every later correction of a copy's resting place is logged with its distance as
`[PROP-CORRECT]`, each one and a 30 s summary (`coop/props/prop_wire_parity`). A receiver parks the prop on the first
pose and follows the stream the way a carried trash clump is followed -- the fixed-delay interpolation, frozen at the
last pose across a gap -- where a held prop snaps to each pose; a reliable end edge carries the final pose, the host's
physics flags and the velocity once the prop has rested or a hand has taken it, and closes the stream's generation so
a pose still in flight cannot park the prop again (`coop/props/prop_drive_stream`). An end that carries no velocity leaves
the copy asleep at its pose, on the host after a client's row as on every receiver, so no second simulation settles it a
centimetre away `[?]` until the body-push drill's rest compare reads it. The author sends that End only when its own
body has stood still and is asleep, read from the engine's awake flag for that one prop while its row is open (the
engine's sleep notification is a multicast no seam of ours sees), so a body still creeping on a support keeps streaming
and no timer forces an End. The cost is bounded: an awake prop in contact with a pawn costs its syncer one 1 Hz beat
(the row stays open), since a prop leaning on a player is not at rest, its support moves with the player; the row ends when
the body sleeps. The body-push drill has the pusher step back 150 cm after each End before it marks the rest, so a prop is
not held awake by the pawn it was measured against; a dev row (`open_row_trace`, default off) says each open row's awake flag
and pose delta once a second. A hand always wins: a claimed prop
somebody grabs leaves the set and rides the held-prop stream, and a receiver yields at once to its own player's grab,
which no stream from the host knows of yet, handing back the physics its park took.

## A prop that moves on its own, and a copy that rests apart

The sixth feeder is the settle: a prop that moves with no hand, no row and no host body on it -- the plate on a chair a
client picked up, a can a thrown box knocked, a drive left on a table a client dragged. Before it every peer's copy fell by
its own physics: the plate came to rest 55 cm and 110 degrees from the host's, and the drives on a dragged table 29 to 122
cm from it, on both clients. The host's body-push list takes the props near every player's puppet as well as its own (64
per body), and a move there over 3 cm, or a fall anywhere in it, that no hand, row or host body explains is a Settle coast
(`prop_drive_host::CoastCause::Settle`), which opens a row only where none stands, as a push does, under the same cap; a
move a client's own body makes near its puppet is left to that client's push claim. The robots the host's brain drives,
a kerfus that is on and every Omega, are bodies of the list too: a prop one knocks with no player in reach was streamed by
no one, and each copy rested where its own physics left it -- with the watch off for them (`VOTVCOOP_ROBOT_SETTLE_OFF=1`),
the six boxes a kerfus hit with every player 11 to 40 m away ended 32 cm to 7.3 m off on both clients; with it, the six
such hits of two runs ended within 0.2 cm, and every box the kerfus hit within 2 cm `[V]` (`coop/dev/kwheel_probe`). A robot nearer a moved prop than a puppet makes it the host's settle, not that
client's claim, and a client does not claim a push its copy of a robot made. The prop accuracy drill
(`VOTVCOOP_PROP_ACC`, with the drift census `VOTVCOOP_PROP_DRIFT`; `VOTVCOOP_SETTLE_OFF=1` is its red arm) drags that
table from a client: with the watch every item ends within 1 cm of the host's on both clients `[V]`.

Some copies still rest apart with no stream to correct them: a joiner's world settles as it loads (110 of 1100 props near
the base over 5 cm off, a kerfur 49 cm), a thin prop an End teleports onto a table top can be pushed through it (a drive
fell 100 cm to the floor on one client), and a client's own body can move its copy alone. The host is the truth of where a
prop rests (`coop/props/prop_rest_sync`): 30 s after a joiner's world is ready the host sends it the rest pose of every
keyed prop at rest, in no hand and under no row (PropRestPose, nine to a message, 150 a second: about 2300 props in 15 s),
and a client asks for a prop by eid (PropRestQuery) once its own copy has moved with no stream on it and rested a second --
one its body-push list saw move, and any copy that moves over 2 cm or 3 degrees in the 6 s after an End put it at the host's
pose. A client puts a resting copy that is over 2 cm or 3 degrees from a row where the row says, with the host's static,
frozen and sleep, asleep, and leaves alone one it corrected three times in a minute. In the drill's session the observing
client's worst resting prop ends 1.5 cm from the host's, p95 0.12 cm `[V]`; props that fell out of the world (two glowsticks
under the floor) and a kerfur are what the other client still holds apart.

