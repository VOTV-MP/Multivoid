# VOTV_MP voice chat — C++ port design (Simple Voice Chat → coop mod)

Date: 2026-06-12. Status: **IMPLEMENTED 2026-06-12 as protocol v66**; hands-on
round 1 found voice inaudible on a one-PC LAN pair — root causes diagnosed (the
ini reader strips spaces out of device-name values; GetAsyncKeyState PTT is
process-global so both same-PC instances transmit together) and the user
re-directed the keys/UI (PTT default → G; V opens the settings panel instead of
the scoreboard button; wider scoreboard; visible idle mic glyph). The fix batch
+ the voice audit's findings (hud snapshot race, 20 Hz position throttle,
per-slot inbox rings, session_relay extraction) are queued — the verbatim list
lives in memory `project_voice_chat_2026-06-12.md`. Implementation notes below
otherwise stand —
`coop/voice/{voice_chat,voice_capture,voice_playback}` + `coop/net/session_voice` +
`ui/{voice_icons,voice_panel}` + scoreboard/nameplate/HUD integration; libopus
v1.5.2 + miniaudio 0.11.22 vendored. Deliberate divergences from §2: no
host-side distance cull (receiver attenuation zeroes out-of-range; a cull would
need engine positions on the net thread) and senderSlot/distanceCm dropped from
the payload (the header senderSlot + the whisper flag carry both). The UI lives
in the tilde scoreboard (user direction): Voice button → settings panel, per-row
mic icons with local mute/volume. Phase 2 still deferred: RNNoise/AGC/VAD.
RE source: `svc-voice-chat-RE-2026-06-12.md` (byte-level SVC findings).
User-mandated changes: (1) **no additional port** — voice multiplexes over the
existing coop UDP session; (2) **PTT default key = X**.

## MTA precedent (RULE 2026-05-28)

MTA ships voice **multiplexed over the main game connection** — exactly our
shape: `Client/mods/deathmatch/CVoiceRecorder.{h,cpp}` (PortAudio capture →
Speex encode → PTT state machine with VOICESTATE_RECORDING_LAST_PACKET) and
`Server/mods/deathmatch/logic/packets/CVoiceDataPacket.h` (PACKET_LOW_PRIORITY
| PACKET_SEQUENCED, PACKET_ORDERING_VOICE — its own ordering channel on the
shared connection) + `CVoiceEndPacket` (= SVC's empty stop frame). Server
relays; client `CClientPlayerVoice` plays back per-player. We take MTA's
*transport* shape and SVC's *audio pipeline* (opus, VAD/denoise, jitter
buffer, PLC, distance model) — SVC's pipeline is the more modern of the two
(MTA still uses Speex).

## 1. What is kept / dropped vs SVC

KEPT (faithful port): 48 kHz mono s16, 960-sample/20 ms frames; opus VOIP +
FEC(5%); per-burst encoder/decoder reset via empty stop frame; mic state
machine (PTT + voice-activation, deactivation delays 5/25 frames); gain with
50-frame peak limiter; jitter buffer (threshold 3, sorted, stop-marker flush);
seq-gap PLC ≤ 5 frames else decoder reset; 100 ms prebuffer; linear distance
attenuation `1 − d/maxDist`; manual stereo pan (SVC REDUCED formula); talk
indicator 250 ms timeout; whisper (half distance); proximity routing with
host-side distance cull at `maxDist + 1`; per-player volume + master volume.

DROPPED (with reason, RULE №2 — fully, no stubs):
- Separate UDP socket + magic byte + AES-128-GCM secret + authenticate/
  connection-check/keepalive/ping → the GNS session already provides an
  authenticated, AES-GCM-encrypted, connection-managed channel. Voice rides it.
- Secret/voiceHost handshake → no secret needed; voice capability is part of
  the existing join handshake (protocol version bump).
- Groups (NORMAL/OPEN/ISOLATED), categories, spectator modes, MP3 recording,
  proxy support → out of scope (principle 5; 2–4 player proximity coop).
- Java fallback codec paths, dual mic backends → one C++ backend.

PHASE 2 (deliberately deferred, not stubbed): RNNoise denoise + VAD-by-speech-
probability, Speex AGC. Phase 1 activation = peak-dB threshold (−50 dB
default) + PTT; gain = manual with limiter. This is SVC's own fallback path
when natives are unavailable, so phase 1 is still a coherent SVC
configuration, not an invention.

## 2. Transport (the "no extra port" change)

- **`MsgType::VoiceFrame = 64`** — unreliable datagram, fits kMaxPacketBytes
  (256): `PacketHeader(20)` + `VoiceFramePayload`:

```cpp
#pragma pack(push,1)
struct VoiceFramePayload {        // MsgType::VoiceFrame body
    uint8_t  senderSlot;          // filled by host on relay; sender sets own
    uint8_t  flags;               // bit0 = whisper, bit1 = stop-marker
    uint16_t opusLen;             // 0 for stop-marker
    uint32_t seq;                 // per-sender monotonic (SVC i64 -> u32 ample:
                                  //  50/s = 994 days; reset on session)
    float    distanceCm;          // host-stamped voice radius (attenuation max)
    uint8_t  opus[/*<=210*/];     // opus 48k mono 20ms frame
};
#pragma pack(pop)
```

  Encoder capped `OPUS_SET_BITRATE(48000)` + max payload 210 B → 120 B
  typical frame; total datagram ≈ 150 B, 50/s while talking ≈ 7.5 KB/s per
  speaker. No protocol.h size-cap changes needed (EntityPose precedent for
  bigger exists but is unnecessary).
- **Routing**: star topology as today. Client → host; host applies locally +
  relays to every other ready slot whose puppet is within `distance + 1` of
  the speaker (SVC broadcast_range), stamping `senderSlot` and `distanceCm`
  (whisper → 2400 cm else 4800 cm, see §5 units). Host's own mic frames go
  out the same way. Echo-suppression: never relay back to origin slot.
- **Reliable lane**: one new `ReliableKind::VoiceState` (Lane::Normal,
  client-relayable) carrying `{slot, micMuted, voiceDisabled}` for roster/
  scoreboard display only — routing never depends on it (host drops frames
  from a muted sender anyway since none arrive). No keepalive/secret kinds.
- Protocol version bump (v65 → next free at implementation time).

## 3. Module layout (principle 7 + 800-LOC rule)

All gameplay/network logic in `coop/`, engine access via existing
`players::Registry` / `RemotePlayer::GetHeadPosition()` / `ue_wrap::engine`
getters — no new engine surface expected.

- `coop/voice/voice_chat.{h,cpp}` — subsystem facade (Install/TickConnect/
  Tick/DisconnectSlot/DisconnectAll per subsystems.h registry), VoiceFrame
  send/recv/relay, VoiceState, config wiring. ~400 LOC.
- `coop/voice/voice_capture.cpp` — mic thread: 20 ms loop, device capture,
  gain+limiter, activation state machine (PTT X / threshold), opus encode,
  stop frame + encoder reset. ~350 LOC.
- `coop/voice/voice_playback.cpp` — per-slot channel: jitter buffer (verbatim
  AudioPacketBuffer port), seq/PLC/reset logic (AudioChannel port), opus
  decode, 100 ms prebuffer, mix + spatialize into output stream. ~450 LOC.
- `ui/` additions: dev-menu voice panel (device picker, mode, threshold, gain,
  volumes, live mic level meter); scoreboard + nameplate talk indicator
  (250 ms TalkCache port). Plain-English labels + `(?)` tooltips (WP10).

### 3.1 Voice state icons (SVC RenderEvents.java port)

SVC renders ONE icon per surface by strict priority. Both chains port verbatim
(`common-client/.../RenderEvents.java:61-77` HUD, `:148-158` nameplate).

**Local HUD icon** (16×16, SVC: configurable corner; ours: bottom-left,
`voice.hud_icon=1` to toggle). First match wins:
1. voice transport down → `disconnected` (suppressed for the first 5 s after
   session start — SVC's startup grace, RenderEvents.java:61,84-87);
2. voice module disabled → `speaker_off`;
3. mic muted AND activation==VOICE → `microphone_off` (PTT mode shows no
   muted icon — faithful);
4. whispering → `microphone_whisper`;
5. talking → `microphone`;
6. else nothing.

**Remote player icon — to the RIGHT of the nameplate** (SVC places it at
`textWidth/2 + 2` right of the name, 10×10, plus a see-through alpha-127
pass; ours: right edge of the plate background + 2 px, scaled by the plate's
existing `DistanceScale`/`DistanceAlpha`). Priority:
1. whispering → `speaker_whisper`;
2. talking → `speaker`;
3. peer's voice disconnected → `disconnected`;
4. (SVC: other-group → `group` — SKIPPED with groups);
5. peer's voice disabled → `speaker_off`;
6. EXTENSION (divergence, deliberate): peer mic muted → `microphone_off`.
   SVC never broadcasts `muted` (client-local there); our VoiceState already
   carries it and in a 2–4 player coop "he can't hear-- can't talk right now"
   is exactly what the icon column is for.
7. else nothing.

Talking/whispering derive client-side from decoded frames (TalkCache port:
`now − lastFrame < 250 ms`, whisper flag cached per frame) — zero extra
packets. Disconnected/disabled/muted come from `ReliableKind::VoiceState`.

**Plumbing**: `nameplate::Plate` gains `uint8_t voiceIcon` (enum VoiceIcon:
None/Talking/Whispering/Disconnected/Disabled/MicMuted) computed in
`nameplate::Update()` on the game thread from voice_chat's per-slot state —
the render thread keeps reading the published POD snapshot, no new locks.
Scoreboard gets the same enum as a mic column. Same enum drives the local
HUD icon (slot = self).

**Icon art**: NOT SVC's PNGs (their license; plus we ship no assets) — icons
are drawn as ImGui draw-list vector primitives (mic capsule + stand, speaker
wedge + arcs, slash overlay for off/muted, broken-link glyph for
disconnected, small "air puff" arcs for whisper variants), tinted white,
alpha-multiplied by plate alpha. One `ui/voice_icons.{h,cpp}` helper
(~150 LOC) shared by nameplate, scoreboard and HUD.

**Fallback (user-approved 2026-06-12)**: if the vector icons read poorly at
nameplate scale, switch to emoji glyphs (🎤 talking, 🤫 whisper, 🔇 muted,
🔊/🚫 disabled, ⛓️‍💥/❌ disconnected) by merging a system emoji font into the
ImGui atlas (`C:\Windows\Fonts\seguiemj.ttf`, U+1F300–1FAFF + misc ranges).
Note: stock ImGui rasterizer renders them MONOCHROME; color needs the
imgui_freetype backend flag — monochrome white-tinted is fine for our HUD
style, so no new dependency either way. Decide visually during phase 6.

Audio I/O backend: **miniaudio** (single header, public-domain/MIT, WASAPI
underneath) vendored under `third_party/` — capture device + playback device
+ device enumeration in one lib, no OpenAL Soft dependency. Spatialization is
ours (SVC REDUCED-mode pan + linear attenuation), so no 3D audio lib needed.
Codec: **libopus** vendored (BSD, the same lib SVC wraps via opus4j).
RNNoise (BSD) joins in phase 2.

## 4. Threading

- **Mic thread** (ours, new): capture → encode → push encoded frames to an
  SPSC queue. Never touches engine or session.
- **Playback**: miniaudio callback thread pulls mixed PCM from per-slot ring
  buffers; per-frame spatial params (gain L/R) computed from atomically
  snapshotted positions. Never touches engine.
- **Game thread** (subsystem Tick): drains mic queue → Session send (≤2
  frames/tick at 30+ fps; the receiver jitter buffer absorbs the ≤33 ms send
  jitter); drains session inbox → feeds per-slot jitter buffers; snapshots
  local camera pos/yaw (`GetActorLocation` + `GetControlRotation`) and each
  `RemotePlayer::GetHeadPosition()` into atomics for the audio side; PTT poll
  `GetAsyncKeyState('X')`.
- Engine objects are read only on the game thread (existing rule); audio
  threads see only copied POD snapshots.

## 5. Units / tuning

UE cm. SVC 48 m voice radius → **4800 cm default**, whisper 2400 cm; vertical
pan fade constant 32 blocks → 3200 cm. Defaults reviewed against VOTV map
scale at implementation (base interior ~ tens of meters → 48 m is "hearable
across the facility yard", feels right; config-exposed regardless).

Config (`votv-coop.ini`, harness/config.cpp pattern):
`voice.enabled=1, voice.mode=ptt|activation (default ptt),
voice.ptt_key=X, voice.threshold_db=-50, voice.mic_gain_db=0,
voice.mic_device= (default system), voice.output_device= (default system),
voice.volume=1.0, voice.distance_cm=4800, voice.jitter_threshold=3,
voice.prebuffer_frames=5`.

## 6. Implementation order (queued — tree currently owned by other instance)

1. Vendor libopus + miniaudio (CMake) — BLOCKED until tree is free.
2. protocol.h: MsgType::VoiceFrame, ReliableKind::VoiceState, payload structs,
   version bump; session_lanes.h relay whitelist entry.
3. voice_playback (jitter+PLC+mix) with a loopback self-test scenario (host
   hears own mic via local loop) — testable single-machine.
4. voice_capture + activation + PTT(X).
5. Host relay + distance cull; 2-peer LAN smoke (mp_host/mp_client launchers).
6. UI (dev menu panel, talk indicators) + config persistence.
7. Audits per CLAUDE.md: hot-path table (mic thread cadence, per-frame
   allocs banned — preallocate all buffers; zero per-frame heap in capture/
   playback paths), file-size check, then hands-on.

Open items for implementation time: confirm Session unreliable-send game-
thread affinity (else mic thread could send directly — GNS itself is
thread-safe); pick the actual free MsgType bit + ReliableKind id against the
then-current protocol.h; check X key clash with VOTV bindings (X is also the
game's "use battery/flashlight"-adjacent bind? verify in-game before locking
default).
