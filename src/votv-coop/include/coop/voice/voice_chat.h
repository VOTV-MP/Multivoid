// coop/voice/voice_chat.h -- the voice-chat subsystem facade (v66).
//
// USER ASK (2026-06-12): proximity voice chat, the Simple-Voice-Chat port per
// research/findings/votv-voice-chat-port-design-2026-06-12.md. User-mandated:
// voice multiplexes over the existing coop session (NO second port) and PTT
// defaults to 'X'. MTA precedent: CVoiceDataPacket rides the main connection
// the same way.
//
// Wiring (the subsystems registry shape):
//   Install -- read voice.* config, open capture+playback, reset state.
//   Tick    -- tone-mode synth; drain encoded mic frames -> seq-stamp ->
//              Session::SendVoiceFrame (+ optional local loopback); drain the
//              session voice inbox -> per-slot jitter buffers; decode; snap
//              listener/speaker positions for the mixer; mute-key edge;
//              VoiceState edges.
//   OnVoiceState / ReplayPeerStatesToSlot / OnDisconnectSlot / OnDisconnect.
//
// The icon surfaces (nameplate / scoreboard / HUD) read PeerVoiceStateFor +
// LocalVoiceState -- everything is derived client-side from decoded frames
// (the SVC TalkCache) + the display-only VoiceState edges.
//
// Game thread throughout (the audio threads live inside capture/playback and
// see only POD snapshots).

#pragma once

#include "coop/net/protocol.h"
#include "coop/player/players_registry.h"  // kMaxPeers

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::voice_chat {

// The one-icon-per-surface priority enum (SVC RenderEvents port; the design
// doc SS3.1). Shared by nameplate, scoreboard and the local HUD.
enum class VoiceIcon : uint8_t {
    None = 0,
    Talking,
    Whispering,
    Disconnected,  // voice transport down (local HUD only for now)
    Disabled,      // peer turned the module off
    MicMuted,
};

struct PeerVoiceState {
    bool talking = false;
    bool whispering = false;
    bool micMuted = false;       // from VoiceState
    bool voiceDisabled = false;  // from VoiceState
};

void Install(coop::net::Session* session);
void Tick();

// Wire ingest (display-only state).
void OnVoiceState(const coop::net::VoiceStatePayload& p, uint8_t senderSlot);

// Host: replay known peer voice states to a late joiner (T2-4 shape).
void ReplayPeerStatesToSlot(int peerSlot);

void OnDisconnectSlot(int slot);
void OnDisconnect();

// ---- UI surface (game thread) ----
PeerVoiceState PeerVoiceStateFor(int slot);
VoiceIcon IconForSlot(int slot);   // remote-player priority chain
VoiceIcon LocalHudIcon();          // local priority chain (PTT shows no muted icon)
bool  Enabled();
bool  Muted();
void  SetMuted(bool m);
float MicLevelDb();
float MasterVolume();
void  SetMasterVolume(float v);

// ---- render-thread surface (scoreboard column / voice panel / HUD) ----
// A POD snapshot rebuilt each game tick under a small mutex (the roster
// snapshot pattern); the volume/gain/threshold/mute setters write atomics and
// are safe from the render thread directly. Device/mode changes need a device
// reopen -- the panel writes the ini then calls RequestDevicesRestart(); the
// next game tick performs the reopen (never the render thread).
struct UiSnapshot {
    uint8_t icons[coop::players::kMaxPeers] = {};       // VoiceIcon per slot (self = local chain)
    float   slotVolume[coop::players::kMaxPeers] = {};  // local per-player volume (0 = muted for me)
    uint8_t localSlot = 0xFF;
    uint8_t localIcon = 0;
    uint8_t enabled = 0, started = 0, muted = 0, activationMode = 0;
    uint8_t captureOk = 0, playbackOk = 0, transmitting = 0;
    float   micLevelDb = -127.0f;
    float   masterVolume = 1.0f;
    float   thresholdDb = -50.0f;
    float   gainDb = 0.0f;
};
void GetUiSnapshot(UiSnapshot& out);
void  SetSlotVolume(int slot, float v);
float SlotVolume(int slot);
void  SetThresholdDb(float db);
void  SetGainDb(float db);
void  RequestDevicesRestart();

}  // namespace coop::voice_chat
