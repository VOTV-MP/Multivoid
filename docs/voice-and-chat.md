# Voice and chat

## Purpose

Text chat and proximity voice: who commits a chat line and why it is the host, what a joiner
gets of the conversation it missed, the bubbles and the feed, and how voice rides the same
connection as everything else.

## How it works

### Chat

T opens the bar; Enter sends; Escape closes without sending and falls through to the game. A
client's line reaches the host as an intent, the host commits it to the lobby's record with a
monotone sequence number and broadcasts a speaker line and the text to every ready client, the
origin included (`coop/comms/chat_sync`, `coop/comms/chat_log`). The host authors chat rather
than relaying it because of a threading fact: a relay fires on the net thread as the packet
arrives, before the inbox drains on the game thread, so there is no point on a relay path where
an order exists; the commit and the broadcast have to be one act, at one authority, on one
thread.

The record is the lobby's chat and is identical for everyone. The feed is each peer's own view
(`coop/comms/chat_feed`): chat lines beside that player's own notices (a skin change, a connect
in progress), in two tiers, live with a fade and then retained as the history the bar reveals.
Which lines retire into history is a class the pusher names, because no rule over the text could
tell "connecting to a host" from a line about someone else. A nickname's colour is resolved once
when the line is composed and frozen into it, so a two-hour-old message never repaints when its
slot is recycled (`coop/comms/chat_nick_color`). The last line from each peer floats above its
nameplate as a bubble that rides the nameplate's own visibility
(`coop/comms/chat_bubbles`). Shared-world actions a peer performs (an email deleted) are
announced as feed lines rendered locally from the lane that already carries the event, with the
actor's own nickname as the subject for everyone including the actor
(`coop/comms/peer_action_feed`). Chat is UTF-8 end to end, with the overlay's own fonts.

### Voice

Voice multiplexes over the session, with no second port. The shape is Simple Voice Chat's,
ported: mono at 48 kilohertz in twenty-millisecond Opus frames with in-band correction for a
few percent of loss, captured on the audio device's own callback, gated by push-to-talk on G by
default or by a level threshold, with a gain and a rolling-peak limiter
(`coop/voice/voice_capture`). Frames stream unreliable through the host, sequence-stamped
(`coop/voice/voice_chat`). On receive, a per-slot jitter buffer delivers in-order frames at once
and sorts the rest, a small gap is concealed by the decoder and a large one resets it, a channel
prebuffers about a hundred milliseconds after silence, and a playback callback mixes the slots
with linear distance attenuation, a vertical fade and a stereo pan
(`coop/voice/voice_playback`). Whispering halves the radius. The voice range is the host's
setting (`voice.distance_cm`, in centimetres: `/set` or F1's Administration > Server settings,
or the host's multivoid.ini); 0 is unlimited,
and a whisper stays local `[?]`. A peer is drawn as talking when a
frame of theirs decoded within the last quarter second; mute, disabled and whisper are presence
states relayed on change. A developer flag replaces the microphone with a test tone.

### The camcorder

The camcorder (`prop_vidcam_C`) records through the gamemode's camera manager, on the MSC_Record
plugin: the frames as images and the engine's master mix as a wav, heard at the camera, which
the plugin's merger then joins into the mp4. The manager turns the player's own microphone into
that mix while it records, when the audio settings allow it. Voice chat plays on its own device,
outside the engine's mix, so no recording held it. While this peer's manager records and is not
paused, the voice mixer also mixes every channel as heard at the camera, with voice chat's own
distance gain, vertical fade and pan and each player's volume, and the track streams to a file
beside the captures. This peer's own voice chat joins it from where the player stands when the
game's microphone does not record. The merger is launched from inside the manager's stop, while
the engine still writes the wav: the launch waits up to five seconds for the wav to close, adds
the track into its front left and right channels, and goes on (`coop/voice/camcorder_voice`, the
launch watch `ue_wrap/core/process_launch`) `[V]` (`coop/dev/vidcam_probe`,
`VOTVCOOP_VIDCAM_PROBE=host`, with `VOTVCOOP_CAMVOICE_KEEP` keeping the merged wav: a client's
test tone beside the camera ran through the whole wav's front pair at about two thirds of full
scale; stepped 24 metres away, it fell to about a third; the recording player's own tone, its
game microphone off, was in it; a three-second pause left the wav and the track both seven
seconds long).

## Who owns what

| State | Owner | Shape |
|---|---|---|
| a chat line | the host's record | a client's line is an intent; the host commits and broadcasts |
| the feed | each peer | its own view, two tiers |
| a voice frame | the speaker | an unreliable stream through the host |
| voice presence | each peer | state edges, relayed and replayed to a joiner |

## Wire messages

| Kind | Direction | Carries |
|---|---|---|
| `ChatMessage` | a client to the host | the typed line, as an intent |
| `ChatSpeaker`, `ChatLine` | the host to all | who the following line is from; the committed line with its sequence |
| `VoiceFrame` (stream) | the speaker, relayed by the host | one twenty-millisecond frame, sequenced |
| `VoiceState` | each peer, relayed | muted, disabled, whispering |

## Late join

The joiner receives the lobby's record one line per message at its ready edge, landing retained
rather than replayed across the screen: the feed is clear on arrival and the history is there
under T. Live chat toward a slot starts only after its seed has been sent, and the applied range
is contiguous so a seed can never be discarded as old. Voice presence states are replayed.

## Known limits

| Limit | Evidence |
|---|---|
| Walls do not muffle voice; attenuation is distance and height only | `[V]` `coop/voice/voice_playback` |
| No noise suppression or automatic gain; the level threshold and the manual gain are what ships | `[V]` `coop/voice/voice_capture` |
| A peer's own notices in the feed are that peer's alone; only the chat subsequence is identical across peers | `[V]` `coop/comms/chat_feed` |
| A camcorder's voice track is mixed on the recording peer, from the voices that reached it; a wav the engine has not closed five seconds after the merger's launch keeps the game's sound alone, logged | `[V]` `coop/voice/camcorder_voice` |
| The final mp4's sound was not decoded (the bundled ffmpeg has no mp4 reader); the merged wav was. A real microphone and a camcorder on a tripod were not run | `[?]` `coop/dev/vidcam_probe` |

## Code map

| Concept | Files |
|---|---|
| chat | `coop/comms/chat_sync`, `coop/comms/chat_log`, `coop/comms/chat_feed`, `coop/comms/chat_bubbles`, `coop/comms/chat_nick_color`, `coop/comms/peer_action_feed`, `ui/chat_input`, `ui/chat_view`, `ui/hud` |
| voice | `coop/voice/voice_chat`, `coop/voice/voice_capture`, `coop/voice/voice_playback`, `ui/voice_panel`, `ui/voice_icons`, `third_party/opus`, `third_party/miniaudio` |
| the camcorder's voice track | `coop/voice/camcorder_voice`, `ue_wrap/core/process_launch`; `coop/dev/vidcam_probe` |
| text | `coop/text/utf8_codec`, `coop/text/repertoire` |
| tests | the `chathistory` and `chatseed` scenarios, each with a must-fail injection |
