// coop/voice/voice_capture.h -- mic capture, then gain and limiter, then activation, then opus
// encode. A port of Simple Voice Chat's mic thread onto miniaudio, where the device callback IS
// the mic thread, because miniaudio owns it and a 20 ms cadence loop of our own would only add
// latency. The configuration is SVC's natives-unavailable one: push-to-talk or peak-dB threshold
// activation (-50 dB default), plus manual gain with the 50-frame rolling-peak limiter.
// RNNoise and AGC are not ported.
//
// The audio constants are SVC's exactly: 48 kHz mono s16, 960-sample (20 ms) frames, opus VOIP
// with inband FEC at 5% expected loss, and an encoder reset between talk bursts through the empty
// stop frame.
//
// Threading: the miniaudio callback produces ENCODED frames into a fixed SPSC ring and the game
// thread (voice_chat::Tick) consumes them. No engine access, no session access, no allocation
// after Start. Test-tone mode skips the device entirely -- the game thread synthesizes and
// encodes at the same cadence -- which is the path on a machine with no microphone.

#pragma once

#include "coop/net/protocol.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace coop::voice {

struct CaptureConfig {
    bool        activationMode = false;  // false = PTT (SVC default), true = voice-activation
    int         pttVk = 'G';             // matches the voice.ptt_key default; always overwritten
                                         // by voice_chat's config read before capture opens
    int         whisperVk = 0;           // 0 = whisper disabled
    float       thresholdDb = -50.0f;    // activation-mode peak threshold
    float       gainDb = 0.0f;           // manual mic gain (-40..+24)
    std::string device;                  // capture device name substring ("" = system default)
    bool        testTone = false;        // dev: 440 Hz sine instead of a mic (no device opened)
};

// One encoded frame leaving the capture side (the VoiceFramePayload minus seq
// -- the facade stamps seq so loopback and wire share one counter).
struct EncodedFrame {
    uint8_t  flags = 0;   // kVoiceFlag*
    uint16_t len = 0;     // 0 for the stop marker
    uint8_t  opus[coop::net::kVoiceMaxOpusBytes];
};

class Capture {
public:
    // Open the device (or arm tone mode) + create the encoder. False on
    // device/encoder failure (voice then runs receive-only; logged).
    bool Start(const CaptureConfig& cfg);
    void Stop();
    bool running() const { return running_; }

    // Game thread: pop one encoded frame (SPSC consumer). False when empty.
    bool PopFrame(EncodedFrame& out);

    // Tone mode only: synthesize+encode pending 20 ms frames (20 ms cadence
    // accumulator). No-op when a real device is running.
    void TickTone();

    // UI surface (any thread; atomics).
    bool  IsTransmitting() const { return transmitting_.load(std::memory_order_relaxed); }
    bool  IsWhispering() const { return whispering_.load(std::memory_order_relaxed); }
    float MicLevelDb() const { return levelDb_.load(std::memory_order_relaxed); }
    void  SetMuted(bool m) { muted_.store(m, std::memory_order_relaxed); }
    bool  Muted() const { return muted_.load(std::memory_order_relaxed); }

    // Live-tunable from the voice panel (atomics; the callback reads per frame).
    void SetGainDb(float db) { gainDb_.store(db, std::memory_order_relaxed); }
    void SetThresholdDb(float db) { thresholdDb_.store(db, std::memory_order_relaxed); }
    float GainDb() const { return gainDb_.load(std::memory_order_relaxed); }
    float ThresholdDb() const { return thresholdDb_.load(std::memory_order_relaxed); }
    bool  ActivationMode() const { return cfg_.activationMode; }

    // Capture-device names for the menu picker (one-shot enumeration; safe
    // before/after Start).
    static std::vector<std::string> EnumerateDevices();

private:
    void ProcessFrame(const int16_t* samples);  // one 960-sample frame through the state machine
    void EmitStop();
    void PushFrame(const EncodedFrame& f);

    // Device-callback accumulation (callback delivers arbitrary counts).
    int16_t staging_[960] = {};
    int     stagingFill_ = 0;

    // Activation state machine.
    int  releaseCountdown_ = 0;  // frames left to keep transmitting after the key/level dropped
    bool wasActive_ = false;

    // 50-frame rolling peak window (the SVC limiter).
    float peakWindow_[50] = {};
    int   peakIdx_ = 0;

    // Tone mode.
    bool   toneMode_ = false;
    double tonePhase_ = 0.0;
    int64_t toneLastMs_ = 0;
    double  toneCarryMs_ = 0.0;

    // SPSC ring (callback producer -> game-thread consumer).
    static constexpr uint32_t kRingSize = 16;  // power of two
    EncodedFrame ring_[kRingSize];
    std::atomic<uint32_t> ringHead_{0};  // consumer
    std::atomic<uint32_t> ringTail_{0};  // producer

    std::atomic<bool>  transmitting_{false};
    std::atomic<bool>  whispering_{false};
    std::atomic<bool>  muted_{false};
    std::atomic<float> levelDb_{-127.0f};
    std::atomic<float> gainDb_{0.0f};
    std::atomic<float> thresholdDb_{-50.0f};

    CaptureConfig cfg_{};
    bool running_ = false;
    void* encoder_ = nullptr;   // OpusEncoder*
    void* device_ = nullptr;    // ma_device* (heap; null in tone mode)
    void* context_ = nullptr;   // ma_context* (heap; device-name selection)

    friend void CaptureDataCallback(void*, void*, const void*, unsigned int);
};

}  // namespace coop::voice
