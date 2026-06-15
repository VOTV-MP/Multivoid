# Simple Voice Chat (Minecraft) — full reverse-engineering findings

Date: 2026-06-12. Source: fresh clone at `reference/VoiceChatMC/simple-voice-chat`
(Java, GPL-ish licensed per repo `license`; we port concepts + algorithms, original
implementation in C++). Companion design doc:
`votv-voice-chat-port-design-2026-06-12.md`.

Method: 4 parallel deep-read agents (UDP protocol / signaling / mic pipeline /
playback pipeline) + manual verification of the load-bearing claims
(`Secret.java`, `AudioPacketBuffer.java` read line-by-line and confirmed).

Repo layout: `common/` (server + shared voice protocol), `common-client/`
(mic capture, decode, playback), `api/` (plugin API — irrelevant to the port),
`fabric|forge|neoforge|quilt|bukkit|paper|...` (loader shims — irrelevant).

---

## 1. Transport architecture (two channels)

SVC runs **two parallel channels**:

1. **Signaling** over Minecraft's reliable TCP plugin-message channel
   (`common/.../net/*Packet.java`): handshake (secret exchange), player states,
   groups, categories.
2. **Voice** over a **separate UDP socket** (default port 24454), with its own
   magic byte, AES-128-GCM encryption, authentication, keepalive and ping
   machinery (`common/.../voice/common/*`, `voice/server/*`).

The entire reason channel 2 has auth/crypto/keepalive is that it is a *separate,
unauthenticated* socket. (Our port multiplexes voice over the already-
authenticated, already-encrypted GNS session, so ALL of §1's plumbing —
magic byte, secret, AES, authenticate/ack, connection-check, keepalive, ping —
is dropped, not ported. See design doc §2.)

### 1.1 UDP datagram wire format

`voice/common/NetworkMessage.java`:

```
[magic 0xFF (1B)] [player UUID (16B, MSB-first then LSB, big-endian)]
[AES-GCM payload: IV (12B) || ciphertext || GCM tag (16B)]
```

Decrypted plaintext: `[packet type (1B)] [packet fields...]`.
Max voice-chat packet size 2048 B. All integers big-endian (Java); varints are
Minecraft's LEB128-style 7-bit-per-byte, max 5 bytes. UUIDs always 16 B.

### 1.2 Packet type registry (UDP)

| ID | Packet | Fields after type byte |
|----|--------|------------------------|
| 0x1 | MicPacket | opus data (varint-len), seq (i64), whispering (1B bool) |
| 0x2 | PlayerSoundPacket | channelId UUID, sender UUID, opus (varint-len), seq i64, distance f32, flags (bit0 whisper, bit1 has-category), [category str] |
| 0x3 | GroupSoundPacket | channelId UUID, sender UUID, opus, seq i64, flags, [category] |
| 0x4 | LocationSoundPacket | channelId UUID, sender UUID, x f64, y f64, z f64, opus, seq i64, distance f32, flags, [category] |
| 0x5 | AuthenticatePacket | player UUID, secret (16B) |
| 0x6 | AuthenticateAckPacket | — |
| 0x7 | PingPacket | ping UUID, timestamp i64 ms |
| 0x8 | KeepAlivePacket | — |
| 0x9 | ConnectionCheckPacket | — |
| 0xA | ConnectionCheckAckPacket | — |

MicPacket TTL 500 ms (server drops stale queued packets).

### 1.3 Encryption (VERIFIED against Secret.java)

- `AES/GCM/NoPadding`, **128-bit key** (16-byte random secret, `SecureRandom`),
  12-byte random IV per message prepended, 128-bit auth tag appended.
- No KDF: the 16-byte secret IS the AES key. Secret generated server-side per
  player, delivered via the reliable channel (SecretPacket).

### 1.4 Server socket + threading

`voice/server/Server.java`:
- Daemon recv thread ("VoiceChatServerThread") → blocking queue → processing
  daemon thread ("VoiceChatPacketProcessingThread", 10 ms poll).
- Keepalive: server sends KeepAlive every `keep_alive` (default 1000 ms);
  client echoes; timeout at `keepAlive * 10` (10 s) → drop connection, regen
  secret, re-handshake.
- Endpoint association: connections matched by source `SocketAddress`.
- Auth flow: Authenticate(uuid,secret) → validate → unchecked map → Ack →
  client sends ConnectionCheck → moved to connected map → CheckAck.

---

## 2. Signaling layer (reliable channel)

`common/.../net/`:

- **RequestSecretPacket** (client→server on world join): `compatibilityVersion i32`.
  Mismatch → human-readable incompatible message, no init.
- **SecretPacket** (server→client): secret 16B, serverPort i32, player UUID,
  codec enum (VOIP/AUDIO/RESTRICTED_LOWDELAY), mtuSize i32 (default 1275),
  voiceChatDistance f64, keepAlive i32, groupsEnabled bool, voiceHost str,
  allowRecording bool.
- **PlayerStatePacket / PlayerStatesPacket** (server→client broadcast/bulk):
  PlayerState = {disabled bool, disconnected bool, uuid, name str, hasGroup
  bool, [group UUID]}.
- **UpdateStatePacket** (client→server): `disabled bool` — toggling the whole
  voice module; broadcast back out as PlayerStatePacket.
- Group packets (Join/Create/Leave/Joined/Add/Remove), category packets —
  group types NORMAL (group hears proximity, proximity doesn't hear group),
  OPEN (bidirectional), ISOLATED (group only). Password max 24 chars,
  non-persistent groups auto-delete when empty.
- **voiceHost** indirection: SecretPacket's host string overrides the UDP
  target (port-only / host:port). Exists purely because voice is a second
  socket. Irrelevant for our port.
- `muted` (mic off, client-local, not broadcast) vs `disabled` (module off,
  broadcast) vs `disconnected` (UDP dead, server-set, broadcast).

---

## 3. Mic capture → encode → send pipeline (client)

`common-client/.../voice/client/MicThread.java` + `MicrophoneProcessor`,
`VoiceMicrophoneProcessor`, `PTTMicrophoneProcessor`, `VolumeManager`,
`Denoiser`, `OpusManager`.

**Audio constants** (`AudioUtils.java`): SAMPLE_RATE 48000 Hz, FRAME_SIZE 960
samples (20 ms), mono s16, MAX_OPUS_PAYLOAD_SIZE 1275 B, LOWEST_DB −127.

**MicThread loop (20 ms cadence, daemon thread):**
1. `pollMic()` — read 960 samples; if `< 960` available, sleep 5 ms and retry.
   Backends: OpenAL capture (`ALC_EXT_CAPTURE`, float32→s16) preferred on
   Win/Linux; javax.sound.sampled fallback / macOS.
2. `process(audio)`:
   a. **Denoise** — RNNoise (rnnoise4j) in-place if enabled (default ON);
      also yields **speech probability** 0..1 (used for VAD).
   b. **Gain** — either Speex AGC (default ON, target −5 dB, only adapts when
      speech probability ≥ 0.95) or manual gain: `mult = 10^(dB/20)` with a
      50-frame rolling-peak limiter clamping `mult ≤ 32767/peak`. Gain range
      −40..+24 dB, default 0.
   c. **Activation**:
      - VOICE mode: VAD (speechProb ≥ 0.5) if denoiser available + vad config
        ON, else peak-dB threshold (`20*log10(|peak|/32768)`, default −50 dB).
        Deactivation delay 25 frames (~500 ms).
      - PTT mode (default): key held (+ 5-frame ≈100 ms release delay).
      - **Whisper**: separate activator on its own key; sets whispering flag.
3. If transmitting: `opus_encode` → `MicPacket(data, whispering, seq++)`,
   seq is a per-client AtomicLong.
4. On transition to silence: **stop packet** = MicPacket with EMPTY data
   (0 bytes), seq++, then `encoder.resetState()`.

**Opus encoder**: 48 kHz mono, application from server codec config (default
VOIP), in-band FEC enabled with expected packet loss 5%, max payload = mtu
(1275). Reset between talk bursts.

---

## 4. Server voice routing

`voice/server/Server.java::onMicPacket`:

1. Resolve player + state, plugin hooks.
2. **In group** → GroupSoundPacket to all members except sender; group type
   OPEN additionally falls through to proximity.
3. **Proximity** → distance = whisper ? `whisper_distance` (24.0) :
   `max_voice_distance` (48.0). Spectator special cases (possession →
   GroupSoundPacket to the spectated player; interaction → LocationSoundPacket
   at eye pos).
4. Build **PlayerSoundPacket**(channelId=sender uuid, sender uuid, data, seq,
   whisper, distance) — i.e. the Mic data + seq are passed through verbatim,
   distance attached so the *client* does the attenuation.
5. Broadcast set = players within `broadcast_range` (default −1 →
   `distance + 1`) by 3D euclidean distance from sender; skip same-group
   members (already covered), skip ISOLATED-group receivers, skip
   disabled/disconnected receivers.

Server NEVER touches the audio bytes — pure relay + routing decision.

---

## 5. Receive → decode → playback pipeline (client)

`common-client/.../voice/client/ClientVoicechat.java`, `AudioChannel.java`,
`AudioPacketBuffer.java` (VERIFIED line-by-line), `speaker/*`.

**Channel model**: one `AudioChannel` daemon thread per `channelId` (=sender
uuid), created on first packet, killed after **30 s** without packets.

**Jitter buffer** (`AudioPacketBuffer`, threshold default 3, 0 disables):
- In-order packet (`seq == last+1` or first) → deliver immediately.
- Out-of-order → insert into list sorted by seq, deliver nothing yet.
- Buffer grows past threshold → pop oldest (accept the gap).
- Empty-data packet (stop marker) enters buffer → set flushing flag: drain
  whole buffer in order, then resume normal mode.
- Poll cadence 5–10 ms.

**Sequence handling in AudioChannel**:
- Drop `seq <= lastSeq` (dupes/reordered-too-late).
- Gap `packetsToCompensate = seq - (last+1)`:
  - gap ≤ `output_buffer_size` (default 5) → **opus PLC**: decode(null) for
    the missing frames (multi-frame decode: PLC×(n−2), then FEC-style decode
    with data, then normal decode).
  - gap > buffer size → skip compensation entirely, `decoder.resetState()`.
- Empty packet (stop marker) → reset lastSeq=−1, clear buffer,
  `decoder.resetState()`.

**Playback (OpenAL)**, `speaker/ALSpeakerBase.java`:
- 32 queued mono s16 buffers per source, one 960-sample buffer per packet.
- On (re)start after underrun: pre-fill `output_buffer_size` (default 5)
  silence buffers → ~100 ms latency floor.
- Overrun (≥32 queued) → jump ahead via AL_SAMPLE_OFFSET.
- **Distance model: AL_LINEAR_DISTANCE** → `gain = 1 − dist/maxDist`
  (clamped), maxDist = packet's distance field, reference distance maxDist/2.
- Listener pos/orientation set from the game camera **on every 20 ms packet
  write** (not per render frame); no interpolation between packets.
- Three output modes: NORMAL (true OpenAL 3D mono source), REDUCED
  (`FakeALSpeaker`: stereo buffers, manual pan), OFF (mono, manual gain only).
- **Manual stereo pan formula** (REDUCED, `PositionalAudioUtils`):
  `angle = atan2(dx, dz) − cameraYaw`; vertical fade `1 − |dy|/32`;
  pan clamped to ±0.5, scaled 1.4, min channel volume 0.3.
- **Volume chain**: master `voice_chat_volume` (0..3, >1 only with
  AL_SOFT_gain_clamp_ex) × per-player volume × category volume ("other"
  fallback) × death fade (`(20−deathTime)/20`) × distance attenuation.
- GroupSoundPacket → non-positional (source-relative, origin); PlayerSound →
  position of the entity with that uuid (or non-positional if it's yourself /
  camera entity); LocationSound → fixed world position from packet.

**Talk indicators** (`TalkCache.java`): per-uuid `{timestamp, whispering,
peak dB}` updated on every decoded frame; `isTalking = now − ts < 250 ms`.

**Recording** (`AudioRecorder`, LAME mp3 320kbps, per-sender files) — noted,
not ported.

---

## 6. Config defaults (consolidated)

Server: port 24454, max_voice_distance 48.0, whisper_distance 24.0,
broadcast_range −1 (=dist+1), keep_alive 1000 ms, codec VOIP, mtu 1275,
groups on, allow_recording on, login_timeout 10 s.

Client: activation PTT (no default key bound; mute key 'M'), threshold −50 dB,
vad on, denoiser on, agc on, mic gain 0 dB (−40..+24), voice deactivation
delay 25 frames, ptt release delay 5 frames, output_buffer_size 5 (1..16),
audio_packet_threshold 3 (0..16), voice_chat_volume 1.0 (0..3),
audio type NORMAL, muted-on-join true.

---

## 7. What does NOT port (Java/MC-specific)

- Separate UDP socket + everything that secures it (magic, AES-GCM secret,
  authenticate, connection-check, keepalive, ping, voiceHost) — replaced by
  the existing GNS session (already AES-GCM encrypted per-connection).
- Minecraft plugin-message channel → our ReliableKind lane.
- javax.sound / LWJGL OpenAL / opus4j / rnnoise4j JNI wrappers → native
  libopus (+ optionally rnnoise) + our own audio I/O.
- Velocity/Bungee proxy shims, Bukkit/Forge/Fabric loader shims, plugin API,
  volume categories, spectators, MP3 recording.
- Groups: deliberately SKIPPED for the coop port (2–4 player proximity coop;
  principle 5 minimum-viable-subset). Whisper IS kept (cheap, useful).
