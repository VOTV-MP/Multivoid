// coop/voice/voice_playback.h -- receive-side voice: a per-slot jitter buffer, PLC and opus decode
// on the game thread, feeding per-slot PCM rings that a miniaudio callback mixes and spatializes. A
// faithful Simple Voice Chat port, summarised in docs/voice-and-chat.md; the numbers that
// make it faithful are a jitter threshold of 3 frames (in order delivers at once, out of order
// sorts, an overflow pops the oldest, a stop marker flushes the buffer and resets lastSeq and the
// decoder), a gap of up to 5 frames covered by opus PLC and a wider one resetting the decoder,
// dupes dropped, ~100 ms of prebuffer (5 frames) before a channel plays again after silence, a
// distance gain of 1 - d/maxDist (a whisper halves the radius; the range is the session's
// voice.distance_cm, 0 = unlimited), a vertical fade of 1 - |dz|/(range * kVerticalFadeRatio), the
// REDUCED-mode pan in [-0.5,0.5] (volume clamp(1 -+ pan*1.4, 0.3, 1)), talking = decoded <250 ms.
// Threading: OnFrame, TickDecode, SetListener and SetSpeaker are GAME THREAD; the miniaudio
// callback touches only the PCM rings (SPSC), the position atomics, the range atomic (distanceCm_)
// and the row's default (DefaultRangeCm(), a constant read). No engine access anywhere, and no
// allocation after Start beyond the lazily created per-slot opus decoders.

#pragma once

#include "coop/config/config_registry.h"
#include "coop/net/protocol.h"
#include "coop/player/players_registry.h"  // kMaxPeers

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace coop::voice {

// The vertical fade scales with the range: 3200 cm of fade at the 4800 cm default (SVC's 32-block
// fade over its 48-block range).
constexpr float kVerticalFadeRatio = 3200.0f / 4800.0f;

// The row's default range, read where it lives.
inline float DefaultRangeCm() {
    return ::coop::config_registry::rows::voice_distance_cm.row->defF;
}

struct PlaybackConfig {
    std::string device;          // output device name substring ("" = system default)
    float volume = 1.0f;         // master voice volume 0..3 (SVC range)
    float distanceCm = 4800.0f;  // proximity radius (whisper = half)
    int   jitterThreshold = 3;   // 0 disables buffering
    int   prebufferFrames = 5;   // ~100 ms latency floor
};

class Playback {
public:
    bool Start(const PlaybackConfig& cfg);
    void Stop();
    bool running() const { return running_; }

    // Game thread: one wire frame for `slot` (jitter-buffer insert).
    void OnFrame(int slot, const coop::net::VoiceFramePayload& f);

    // Game thread: drain jitter buffers -> decode/PLC -> PCM rings.
    void TickDecode();

    // Game thread: position snapshots for the mixer (atomics).
    void SetListener(float x, float y, float z, float yawDeg);
    void SetSpeaker(int slot, float x, float y, float z, bool valid);

    // Game thread: drop a channel's stream state -- when its peer left, and for every slot at Stop,
    // which also runs at a device reopen while everyone stays. The per-person volume is not stream
    // state: voice_chat resets it where the person leaves.
    void ResetSlot(int slot);

    // Icon surface (any thread). True if a frame decoded < 250 ms ago.
    bool IsTalking(int slot, bool* whispering = nullptr) const;

    // Live-tunable (dev menu).
    void SetMasterVolume(float v) { masterVolume_.store(v, std::memory_order_relaxed); }
    float MasterVolume() const { return masterVolume_.load(std::memory_order_relaxed); }
    // The proximity range (cm; 0 = unlimited): the session's voice.distance_cm, applied live by
    // the row's subscriber; the mixer thread reads it.
    void SetDistanceCm(float cm) { distanceCm_.store(cm, std::memory_order_relaxed); }
    float DistanceCm() const { return distanceCm_.load(std::memory_order_relaxed); }
    void SetSlotVolume(int slot, float v);
    float SlotVolume(int slot) const;

    static std::vector<std::string> EnumerateDevices();

    // ---- the camcorder's voice track (coop/voice/camcorder_voice) ----
    // Game thread: while `on`, the mixer also mixes every channel as heard at the camera (x, y, z, yaw) -- the same
    // distance gain, vertical fade and pan as for the listener, each slot's own volume, not the master volume -- into
    // a stereo ring the game thread drains. `selfSlot` (or -1) names the channel that carries this peer's own voice
    // for the camera alone: it is never played. Turning the camera off resets that channel.
    void SetCamera(bool on, float x, float y, float z, float yawDeg, int selfSlot);
    // Game thread: this peer's own outgoing frame, decoded into channel `slot` for the camera mix only.
    void OnSelfFrame(int slot, const coop::net::VoiceFramePayload& f);
    // Game thread: the camera mix since the last drain, interleaved stereo s16 at 48 kHz, appended to `out`. Returns
    // the frames the ring dropped since Start (the game thread fell a second behind).
    uint64_t DrainCamera(std::vector<int16_t>& out);

private:
    struct Channel {
        // Jitter buffer (GT only).
        coop::net::VoiceFramePayload buf[16];
        int   bufCount = 0;
        bool  flushing = false;
        int64_t lastSeq = -1;
        void* decoder = nullptr;  // OpusDecoder* (lazy)

        // PCM ring: the game thread produces in TickDecode, the audio callback consumes. The
        // monotonic counters are 64-bit because 32-bit ones wrap after ~24.8 h of continuous talk
        // on one channel.
        static constexpr uint32_t kRingSamples = 48000;  // 1 s
        int16_t ring[kRingSamples];
        std::atomic<uint64_t> ringWrite{0};
        std::atomic<uint64_t> ringRead{0};
        std::atomic<bool> primed{false};

        // TalkCache + per-frame whisper state (icons + attenuation radius).
        std::atomic<int64_t> lastFrameMs{0};
        std::atomic<bool> whispering{false};

        // Mixer position (GT writes, callback reads; per-component atomics --
        // a torn read across components misplaces one 10 ms block, inaudible).
        std::atomic<float> posX{0}, posY{0}, posZ{0};
        std::atomic<bool> posValid{false};

        std::atomic<float> volume{1.0f};
    };

    void DeliverInOrder(Channel& ch, const coop::net::VoiceFramePayload& f);
    void PushPcm(Channel& ch, const int16_t* samples, int count);
    void MixOutput(float* out, uint32_t frameCount);

    Channel channels_[coop::players::kMaxPeers];

    std::atomic<float> listenerX_{0}, listenerY_{0}, listenerZ_{0}, listenerYaw_{0};

    // The camera mix: the position the camera hears from, the self channel, and a stereo SPSC ring (the callback
    // produces, the game thread drains), allocated at Start.
    static constexpr uint32_t kCamRingFrames = 48000;   // 1 s
    static constexpr uint32_t kCamBlockFrames = 4096;   // the scratch block; a longer callback is mixed in pieces
    std::atomic<bool> camOn_{false};
    std::atomic<float> camX_{0}, camY_{0}, camZ_{0}, camYaw_{0};
    std::atomic<int> camSelf_{-1};
    std::vector<float> camRing_;      // 2 * kCamRingFrames
    std::vector<float> camScratch_;   // 2 * kCamBlockFrames
    std::atomic<uint64_t> camWrite_{0}, camRead_{0}, camDropped_{0};
    std::atomic<float> masterVolume_{1.0f};
    std::atomic<float> distanceCm_{4800.0f};
    int jitterThreshold_ = 3;
    int prebufferFrames_ = 5;

    bool running_ = false;
    void* device_ = nullptr;   // ma_device*
    void* context_ = nullptr;  // ma_context*

    friend void PlaybackDataCallback(void*, void*, const void*, unsigned int);
};

}  // namespace coop::voice
