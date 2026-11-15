# TV links

## Purpose

A web video a player puts on a television plays on that set for every player, and the remote's play,
stop and playback slider act on everyone's copy. Before this lane a television was each player's
own: the set crossed as a prop, and what it showed did not.

## How it works

The game's television (`prop_tv2_C` and its children: the mini TV, the floor sets, the plasma)
plays through one of the gamemode's sixteen media channels and opens a video with its own
`openLink`, which takes a name, a link and whether the link is a web address. A web address goes to
the engine's media player, which opens a file served whole over http or https: one stream holding
picture and sound.

Five post watches make the lane (`coop/interactables/tv_link_sync`), one message carries it
(`TvLink`), and a set is named on the wire by its prop key:

- `prop_tv2_C::openLink`. When a set on this machine is asked for a web address by the game's own
  path (a row of the remote's list, which the game reads from `Assets\tv\online.txt`), the address
  is sent. A file of the player's own folder is not sent: the other players do not have it.
- The search box of the set's remote. A text entered there with Enter that starts with `http://`
  or `https://` is a link: it is sent and played, and the box is emptied so the list shows again.
  Any other text is the game's list filter, as before.
- The remote's play button, stop button and playback slider. After the game has run one on this
  machine's set, a resume, a pause or a seek is sent with the play position, and each receiver calls
  its own media player's `Play`, `Pause` or `Seek`.

A receiver finds its copy of the set by key and calls the set's own `openLink` with the address, so
the game writes Loading, Successful or Error on the remote and starts the play as it does for its
own list. A set with no media channel is given the first free one. A link that names a set this
peer has not loaded yet waits up to three minutes for it.

At a join the host reads each set of its world: one whose player is playing a web address is sent
to the joiner with its play position, and the joiner's set seeks there once its own player has
started. The host remembers nothing between sessions; what a set plays is read from the set.

`[V]` `coop/dev/tv_link_drill` (`VOTVCOOP_TV_LINK_DRILL=[typed:|set:|client:|clienttyped:|control:]<address>`,
both peers), each arm with a 10 second sample file over https on a spawned mini TV: the lane's
entry, the remote's search box, the set's own `openLink` and a client as the asker each ended with
both peers' sets playing to the clip's end, the client's about half a second behind; `control:`
paused the client's set at the host's position and resumed it. A rejoining client's set opened the
host's video and sought to its position; with `net.tv_links_players=0` a client's link was refused
on its own machine with its line; an address of 222 characters played on the asker's set alone.

### YouTube addresses

A YouTube page is not a file, so a YouTube address (`youtube.com`, `youtu.be`) is resolved first
(`coop/interactables/tv_link_resolver`): the yt-dlp program, shipped as `tools\yt-dlp.exe` beside
the mod when the package was built with `-YtDlp`, is run without a window from the mod's real folder
(under a mod manager the folder a module reports is mapped for reads and starts no program) and asked for the address
of one stream with picture and sound, an mp4 first. YouTube names such a stream only to a client
that has solved its script challenge; to one that has not it offers picture and sound apart and an
HLS playlist, and the engine's player (Windows Media Foundation) opens neither as one video. So the
package built with `-QuickJs` ships QuickJS-NG as `tools\qjs.exe`, and the tool is told to solve the
challenge with it (`--js-runtimes`). A run takes about eight seconds. YouTube ties that address to the machine that
asked, so the address that crosses the wire is the one the player typed and every peer resolves it
for itself. An answer is kept for twenty minutes, so a replay, a second set or a joiner's catch-up
does not run the tool again. How a run ended is one log line: the Windows error when it could not
start, else its exit code and what it printed. `[V]` (`tv_link_drill` with a YouTube address, both
peers: asked by the host, typed on the host's remote, and `clienttyped:`, typed on the client's
remote): each peer ran the tool for about nine seconds and its set played a 640x360 picture, read
from the set's screen material and channel texture. A live stream has no such stream and is refused
with the tool's line. A rejoin during a YouTube video has not been measured `[RD]`.

## What a player sees

Lines in the chat feed, to the player they concern: a YouTube link being fetched and one that could
not be played, a link too long to share, a set with no power or no free channel, an address that
cannot be used, and once a session a link another player sent that this save's Custom Content rule
refused.

## What a player needs

- The save's Custom Content game rule on. The game's `openLink` returns at once without it and
  shows its own "Custom Content is disabled!" hint to the player who typed the link; the lane does
  not write the rule.
- A set with power. `openLink` does nothing on a set whose cord is out.
- `tv_links=1` in `multivoid.ini` (the default). With `0` this peer opens no address another player
  chose and sends none.

## Wire messages

| kind | direction | payload |
|---|---|---|
| `TvLink` | any peer to all, a client's through the host's relay; host to a joiner | the set's key, the op (play, pause, resume, seek), the play position, and for a play the address as typed (at most 186 ASCII characters) |

An address is checked before it is sent, on receipt, and before it reaches the tool's command
line: http or https, printable ASCII, no quote, space or backslash. A peer sends at most one
address in 1.5 seconds and one pause, resume or seek in a quarter of a second.

## Known limits

- Each peer starts its own play, so two screens can be seconds apart, more when one resolves a
  YouTube address slower than the other. The channel, the volume and the play mode are each
  player's own.
- A seek on a web video takes the engine's player several seconds to settle, on the machine that
  asked as well.
- An address over 186 characters is not sent; it plays on the machine that typed it only, and that
  player is told.
- Every player's game fetches the video itself: the host of the video sees each player's address.
  A player who puts a link on a set chooses what every other player's game downloads. `tv_links=0`
  is the per-player off switch; the host's `net.tv_links_players=0` (Let players put videos on TVs)
  keeps links and controls to the host: a client's mod sends none and the host ignores one sent.
- The stream yt-dlp can name as one file is 360p on YouTube. A yt-dlp older than YouTube's last
  change stops resolving; the program is replaced by a newer package.

## Code map

| part | code |
|---|---|
| the television's and the remote's verbs | `ue_wrap/devices/tv` |
| the lane | `coop/interactables/tv_link_sync` |
| address checks, the yt-dlp run and its cache | `coop/interactables/tv_link_resolver` |
| the Custom Content rule's read | `ue_wrap/world/game_rules` |
| drills | `coop/dev/tv_link_drill`, `coop/dev/media_url_probe` |
