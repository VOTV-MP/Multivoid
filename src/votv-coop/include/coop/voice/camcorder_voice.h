// coop/voice/camcorder_voice.h -- the players' voices in a camcorder recording, as heard at the camera.
//
// The camcorder (prop_vidcam_C) records through the gamemode's camera manager (rcm_manager, on the MSC_Record plugin):
// the frames as images and the engine's master mix as a wav, which the plugin's merger (MSC_Merger.exe, running the
// bundled ffmpeg) joins into the mp4 and then deletes. Voice chat plays on its own device, outside the engine's mix, so
// the wav never holds it. While this peer's camera manager records and is not paused, the voice mixer also mixes every
// channel as heard at the camera (the same distance gain, vertical fade and pan as voice chat itself), and this lane
// streams that mix to a raw file beside the captures; when the plugin launches its merger, the wav it names is complete
// and unread, and the voice track is added into its front left and right channels before the launch goes on.
//
// This peer's own voice is in the game's own recording when its microphone records (the manager turns the player's mic
// component on when the audio settings allow it); otherwise its outgoing voice frames are decoded into the camera mix
// too, heard from where the player stands.
//
// Game thread, from voice_chat::Tick; the merge runs on the thread that launches the merger.

#pragma once

#include "coop/net/protocol.h"

namespace coop::voice { class Playback; }

namespace coop::voice::camcorder_voice {

// The merger launch watch (once). Idempotent.
void Install();

// Per voice tick: the recorder's state read, the camera's place given to the mixer, the camera mix streamed to the
// track. `localSlot` is this peer's slot (or kPeerIdUnknown); `loopback` the dev loopback, which already plays this
// peer's own voice through its channel.
void Tick(coop::voice::Playback& playback, int localSlot, bool loopback);

// Whether this peer's own outgoing frames go to the camera mix now (voice_chat then hands each to OnSelfFrame).
bool WantsSelf();

}  // namespace coop::voice::camcorder_voice
