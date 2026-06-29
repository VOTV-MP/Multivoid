// coop/voice/voice_playback.h -- receive-side voice: per-slot jitter buffer
// + PLC + opus decode (game thread) feeding per-slot PCM rings that a
// miniaudio playback callback mixes + spatializes. Faithful SVC port
// (svc-voice-chat-RE-2026-06-12.md SS5):
//   - jitter buffer threshold 3 (in-order delivers immediately; out-of-order
//     sorts; overflow pops oldest; the stop marker flushes the whole buffer);
//   - seq gaps <= 5 frames -> opus PLC (decode(null)); bigger -> decoder
//     reset; dupes dropped; stop marker resets lastSeq + decoder;
//   - ~100 ms prebuffer (5 frames) before a channel starts playing after
//     silence/underrun;
//   - linear distance attenuation gain = 1 - d/maxDist (whisper halves the
//     radius), vertical fade 1 - |dz|/3200 cm, and the SVC REDUCED-mode
//     stereo pan (pan in [-0.5,0.5], volume = clamp(1 -+ pan*1.4, 0.3, 1));
//   - TalkCache: talking = a frame decoded < 250 ms ago (drives the icons).
//
// Threading: OnFrame/TickDecode/SetListener/SetSpeaker are GAME THREAD; the
// miniaudio callback reads PCM rings (SPSC: GT produces, callback consumes)
// and position atomics only. No engine access anywhere; no allocation after
// Start except the lazily-created per-slot opus decoders.

#pragma once

#include "coop/net/protocol.h"
#include "coop/player/players_registry.h"  // kMaxPeers

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace coop::voice {

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

    // Game thread: a peer left -- drop its channel state.
    void ResetSlot(int slot);

    // Icon surface (any thread). True if a frame decoded < 250 ms ago.
    bool IsTalking(int slot, bool* whispering = nullptr) const;

    // Live-tunable (dev menu).
    void SetMasterVolume(float v) { masterVolume_.store(v, std::memory_order_relaxed); }
    float MasterVolume() const { return masterVolume_.load(std::memory_order_relaxed); }
    void SetSlotVolume(int slot, float v);
    float SlotVolume(int slot) const;

    static std::vector<std::string> EnumerateDevices();

private:
    struct Channel {
        // Jitter buffer (GT only).
        coop::net::VoiceFramePayload buf[16];
        int   bufCount = 0;
        bool  flushing = false;
        int64_t lastSeq = -1;
        void* decoder = nullptr;  // OpusDecoder* (lazy)

        // PCM ring: GT produces (TickDecode), audio callback consumes. The
        // monotonic counters are 64-bit (audit M-1): 32-bit ones wrap after
        // ~24.8 h of continuous talk on one channel.
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
    std::atomic<float> masterVolume_{1.0f};
    float distanceCm_ = 4800.0f;
    int jitterThreshold_ = 3;
    int prebufferFrames_ = 5;

    bool running_ = false;
    void* device_ = nullptr;   // ma_device*
    void* context_ = nullptr;  // ma_context*

    friend void PlaybackDataCallback(void*, void*, const void*, unsigned int);
};

}  // namespace coop::voice
