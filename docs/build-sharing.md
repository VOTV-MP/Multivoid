# Taking the host's build

## Purpose

A joiner whose Multivoid build differs from the host's can download the host's build over the game
connection and use it from the next start of the game, so a group stays on one build without
passing zips around. A build is native code with the run of the player's machine, so whose build a
joiner takes is the joiner's decision, made on a proved identity.

## How it works

Two builds that differ cannot talk. Another build number ends the connection at the first packet;
the same number over different code connects and misbehaves. So this exchange runs beside the
protocol (`coop/net/build_share`), in frames of its own: they begin with `MVUP`, a magic no build's
packet header carries, so a build without the module drops them unread, and their layout does not
change with the build number.

1. The host tells each new connection its build: the build number, the file's SHA-256, its size, the
   game version it is made for. A connection whose first packet names another build number is not
   closed at once: it is held for five seconds for the joiner to ask.
2. A joiner whose own hash differs, on the same game version, and whose setting allows it, answers
   with a random nonce.
3. The host answers with an offer signed by its identity key over that nonce and the build it
   names. The joiner checks the signature against the identity the transport holds for the
   connection, never one a frame names, so the offer is that host's and is not a replay.
4. The joiner decides (below). On a yes the host deflates its `main.dll` on a worker thread and
   sends it in frames paced by what the connection still has queued. A joiner on the same build
   number is seated and loading the world on that connection, so the host sends it nothing until
   it stands in the world, and then in small frames into a nearly empty queue, behind the game's
   own messages.
5. The joiner inflates the download into exactly the offered size, checks it against the offered
   hash, writes it beside its running `main.dll`, renames the running file to `main.dll.old` (a
   loaded module can be renamed, not overwritten) and puts the new one in its place. A release's
   `.sig` travels with the offer and is written too; otherwise a stale one is removed.

The new build loads at the next start of the game. On another build number the host then closes the
connection for the mismatch, as it always did, and the prompt tells the player to restart; on the
same build number the join carries on with the old build until the player restarts.

`[V]` `bs_test.sh` (the port's evidence folder), two installs on one machine with the prompt
answered by `VOTVCOOP_BUILD_UPDATE_AUTO=1`: a client on another build number, and one on the same
number with different code, each ended with the host's file under its own name, hash for hash, its
own kept as `main.dll.old`, and reached the host's world after a restart. With the switch off:
`net.build_updates=off` was closed for the mismatch at once, `trusted` refused an unlisted host by
name, and a host on the trusted list was installed with no prompt. The prompt itself and a build
that verifies under a release key have not been run `[RD]`.

## Whose build a joiner takes

| the offered build | `net.build_updates=ask` (default) | `trusted` | `off` |
|---|---|---|---|
| verifies under a release key, for this game version | installed, no prompt | installed, no prompt | never |
| from a host on the player's trusted list | installed, no prompt | installed, no prompt | never |
| any other | the prompt | refused | never |

The prompt names the build, its size and the host's identity, says in plain words that an
unofficial build is whatever the host built and runs with full access, and offers Install, Install
and always trust this host, and No. Trusting writes the host's identity to
`multivoid_trusted_hosts.txt` in the mod's data folder, one identity a line; deleting a line
revokes it. With `off`, a browser join to a host on another build number is refused before dialling,
as before.

A host shares its build unless `net.share_build=0`.

## Known limits

- The game is not restarted for the player and the join is not repeated: the prompt says to quit,
  start again and join.
- Only `main.dll` and its `.sig` travel. The pak files and the tools folder stay the joiner's.
- A build made for another version of the game is never taken.
- A mod manager is not told: it still lists the version it installed.
- The first build with this module has to be installed by hand; a build without it shows the
  mismatch message it always did.
- A held connection sends at the transport's default rate, about a quarter of a megabyte a second.
- A host cannot be made to send its build more than once at a time per connection, but any
  connection may ask: the file is the public mod, and the connection cap bounds the askers.

## Code map

| part | code |
|---|---|
| the exchange, the trust list and the install | `coop/net/build_share` |
| where the frames are read | `coop/net/session_receive` (pending and seated), `coop/net/session_status` (the pending sweep) |
| the prompt | `ui/build_update_dialog` |
| release signatures | `coop/build_trust` |
