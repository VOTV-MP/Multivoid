// coop/voice/voice_capture.cpp -- see coop/voice/voice_capture.h.

#include "coop/voice/voice_capture.h"

#include "coop/session/ini_config.h"  // IsOurWindowForeground -- the cross-process PTT gate
#include "ue_wrap/log.h"

#include <Windows.h>

#include <chrono>
#include <cmath>
#include <cstring>

#include <miniaudio.h>
#include <opus.h>

namespace coop::voice {

namespace {

constexpr int kSampleRate = 48000;
constexpr int kFrameSamples = 960;            // 20 ms
constexpr int kPttReleaseFrames = 5;          // ~100 ms (SVC)
constexpr int kVoiceDeactivationFrames = 25;  // ~500 ms (SVC)
constexpr float kLowestDb = -127.0f;

float PeakDb(const int16_t* s, int n) {
    int peak = 0;
    for (int i = 0; i < n; ++i) {
        const int a = s[i] < 0 ? -s[i] : s[i];
        if (a > peak) peak = a;
    }
    if (peak <= 0) return kLowestDb;
    return 20.0f * std::log10(static_cast<float>(peak) / 32768.0f);
}

int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace

// miniaudio C-style callback (pUserData = the Capture*). The friend keeps a
// plain void* signature so the header stays miniaudio-free; the properly-typed
// trampoline below is what miniaudio actually calls.
void CaptureDataCallback(void* dev, void* /*out*/, const void* in, unsigned int frameCount) {
    auto* device = static_cast<ma_device*>(dev);
    auto* self = static_cast<Capture*>(device->pUserData);
    if (!self || !in) return;
    const int16_t* samples = static_cast<const int16_t*>(in);
    unsigned int remaining = frameCount;
    while (remaining > 0) {
        const unsigned int take =
            remaining < static_cast<unsigned int>(kFrameSamples - self->stagingFill_)
                ? remaining
                : static_cast<unsigned int>(kFrameSamples - self->stagingFill_);
        std::memcpy(self->staging_ + self->stagingFill_, samples, take * sizeof(int16_t));
        self->stagingFill_ += static_cast<int>(take);
        samples += take;
        remaining -= take;
        if (self->stagingFill_ == kFrameSamples) {
            self->stagingFill_ = 0;
            self->ProcessFrame(self->staging_);
        }
    }
}

bool Capture::Start(const CaptureConfig& cfg) {
    if (running_) return true;
    cfg_ = cfg;
    gainDb_.store(cfg.gainDb, std::memory_order_relaxed);
    thresholdDb_.store(cfg.thresholdDb, std::memory_order_relaxed);
    toneMode_ = cfg.testTone;

    int err = 0;
    OpusEncoder* enc = opus_encoder_create(kSampleRate, 1, OPUS_APPLICATION_VOIP, &err);
    if (!enc || err != OPUS_OK) {
        UE_LOGW("voice_capture: opus_encoder_create failed (%d)", err);
        return false;
    }
    // The SVC encoder shape: FEC vs 5% expected loss; bitrate capped so a
    // frame always fits kVoiceMaxOpusBytes (48 kbps * 20 ms = 120 B typical).
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(48000));
    opus_encoder_ctl(enc, OPUS_SET_INBAND_FEC(1));
    opus_encoder_ctl(enc, OPUS_SET_PACKET_LOSS_PERC(5));
    encoder_ = enc;

    if (toneMode_) {
        toneLastMs_ = NowMs();
        toneCarryMs_ = 0.0;
        running_ = true;
        UE_LOGI("voice_capture: TONE mode (440 Hz synth; no device)");
        return true;
    }

    auto* ctx = new ma_context();
    if (ma_context_init(nullptr, 0, nullptr, ctx) != MA_SUCCESS) {
        UE_LOGW("voice_capture: ma_context_init failed");
        delete ctx;
        opus_encoder_destroy(enc);
        encoder_ = nullptr;
        return false;
    }
    context_ = ctx;

    // Device selection by name substring ("" = default).
    ma_device_id chosenId{};
    bool haveId = false;
    if (!cfg.device.empty()) {
        ma_device_info* caps = nullptr;
        ma_uint32 capCount = 0;
        if (ma_context_get_devices(ctx, nullptr, nullptr, &caps, &capCount) == MA_SUCCESS) {
            for (ma_uint32 i = 0; i < capCount; ++i) {
                if (std::strstr(caps[i].name, cfg.device.c_str())) {
                    chosenId = caps[i].id;
                    haveId = true;
                    UE_LOGI("voice_capture: device '%s'", caps[i].name);
                    break;
                }
            }
        }
        if (!haveId)
            UE_LOGW("voice_capture: no capture device matches '%s' -- using default",
                    cfg.device.c_str());
    }

    ma_device_config dc = ma_device_config_init(ma_device_type_capture);
    dc.capture.format = ma_format_s16;
    dc.capture.channels = 1;
    dc.sampleRate = kSampleRate;
    dc.dataCallback = [](ma_device* d, void* out, const void* in, ma_uint32 n) {
        CaptureDataCallback(d, out, in, n);
    };
    dc.pUserData = this;
    if (haveId) dc.capture.pDeviceID = &chosenId;

    auto* dev = new ma_device();
    if (ma_device_init(ctx, &dc, dev) != MA_SUCCESS) {
        UE_LOGW("voice_capture: ma_device_init(capture) failed -- voice runs receive-only");
        delete dev;
        ma_context_uninit(ctx);
        delete ctx;
        context_ = nullptr;
        opus_encoder_destroy(enc);
        encoder_ = nullptr;
        return false;
    }
    if (ma_device_start(dev) != MA_SUCCESS) {
        UE_LOGW("voice_capture: ma_device_start(capture) failed");
        ma_device_uninit(dev);
        delete dev;
        ma_context_uninit(ctx);
        delete ctx;
        context_ = nullptr;
        opus_encoder_destroy(enc);
        encoder_ = nullptr;
        return false;
    }
    device_ = dev;
    running_ = true;
    UE_LOGI("voice_capture: capture running (48 kHz mono, %s mode, ptt=0x%X)",
            cfg.activationMode ? "activation" : "PTT", cfg.pttVk);
    return true;
}

void Capture::Stop() {
    if (!running_) return;
    running_ = false;
    if (device_) {
        auto* dev = static_cast<ma_device*>(device_);
        ma_device_uninit(dev);  // joins the callback thread; safe to free state after
        delete dev;
        device_ = nullptr;
    }
    if (context_) {
        auto* ctx = static_cast<ma_context*>(context_);
        ma_context_uninit(ctx);
        delete ctx;
        context_ = nullptr;
    }
    if (encoder_) {
        opus_encoder_destroy(static_cast<OpusEncoder*>(encoder_));
        encoder_ = nullptr;
    }
    transmitting_.store(false, std::memory_order_relaxed);
    whispering_.store(false, std::memory_order_relaxed);
    levelDb_.store(kLowestDb, std::memory_order_relaxed);
    stagingFill_ = 0;
    wasActive_ = false;
    releaseCountdown_ = 0;
    ringHead_.store(0);
    ringTail_.store(0);
}

bool Capture::PopFrame(EncodedFrame& out) {
    const uint32_t head = ringHead_.load(std::memory_order_relaxed);
    if (head == ringTail_.load(std::memory_order_acquire)) return false;
    out = ring_[head % kRingSize];
    ringHead_.store(head + 1, std::memory_order_release);
    return true;
}

void Capture::PushFrame(const EncodedFrame& f) {
    const uint32_t tail = ringTail_.load(std::memory_order_relaxed);
    if (tail - ringHead_.load(std::memory_order_acquire) >= kRingSize) return;  // full: drop
    ring_[tail % kRingSize] = f;
    ringTail_.store(tail + 1, std::memory_order_release);
}

void Capture::ProcessFrame(const int16_t* samples) {
    // Gain (manual dB) + the SVC 50-frame rolling-peak limiter: the applied
    // multiplier never pushes the window's peak past full scale.
    int16_t buf[kFrameSamples];
    const float gain = std::pow(10.0f, gainDb_.load(std::memory_order_relaxed) / 20.0f);
    float framePeak = 0.0f;
    for (int i = 0; i < kFrameSamples; ++i) {
        const float a = std::fabs(static_cast<float>(samples[i]));
        if (a > framePeak) framePeak = a;
    }
    peakWindow_[peakIdx_] = framePeak;
    peakIdx_ = (peakIdx_ + 1) % 50;
    float windowPeak = 1.0f;
    for (float p : peakWindow_)
        if (p > windowPeak) windowPeak = p;
    float mult = gain;
    const float maxMult = 32767.0f / windowPeak;
    if (mult > maxMult) mult = maxMult;
    for (int i = 0; i < kFrameSamples; ++i) {
        float v = static_cast<float>(samples[i]) * mult;
        if (v > 32767.0f) v = 32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        buf[i] = static_cast<int16_t>(v);
    }

    const float db = PeakDb(buf, kFrameSamples);
    levelDb_.store(db, std::memory_order_relaxed);

    // Activation. Whisper key acts as a second PTT that flags frames.
    //
    // Foreground gate (2026-06-12, voice round 1): GetAsyncKeyState is GLOBAL across
    // processes -- two same-PC instances BOTH transmitted on one PTT press (the freecam
    // precedent, freecam.cpp InputDriverThread). Gate every key read -- and activation-
    // mode capture too (no hot mic while alt-tabbed) -- on "the foreground window is
    // OURS" (cheap user32 calls, safe on this mic thread). Tone mode is exempt: the
    // headless autonomous smoke has no focused window at all.
    const bool foreground = toneMode_ || coop::ini_config::IsOurWindowForeground();
    const bool whisperHeld =
        foreground && cfg_.whisperVk != 0 && (GetAsyncKeyState(cfg_.whisperVk) & 0x8000) != 0;
    bool wantActive = false;
    if (!muted_.load(std::memory_order_relaxed) && foreground) {
        if (cfg_.activationMode) {
            wantActive = db >= thresholdDb_.load(std::memory_order_relaxed) || whisperHeld;
        } else {
            wantActive = (GetAsyncKeyState(cfg_.pttVk) & 0x8000) != 0 || whisperHeld;
        }
    }
    if (wantActive) {
        releaseCountdown_ = cfg_.activationMode ? kVoiceDeactivationFrames : kPttReleaseFrames;
    } else if (releaseCountdown_ > 0) {
        --releaseCountdown_;
    }
    const bool active = wantActive || releaseCountdown_ > 0;

    transmitting_.store(active, std::memory_order_relaxed);
    whispering_.store(active && whisperHeld, std::memory_order_relaxed);

    if (!active) {
        if (wasActive_) EmitStop();
        wasActive_ = false;
        return;
    }
    wasActive_ = true;

    EncodedFrame f{};
    if (whisperHeld) f.flags |= coop::net::kVoiceFlagWhisper;
    const opus_int32 n = opus_encode(static_cast<OpusEncoder*>(encoder_), buf, kFrameSamples,
                                     f.opus, coop::net::kVoiceMaxOpusBytes);
    if (n <= 0) {
        static bool s_warned = false;
        if (!s_warned) {
            s_warned = true;
            UE_LOGW("voice_capture: opus_encode failed (%d)", static_cast<int>(n));
        }
        return;
    }
    f.len = static_cast<uint16_t>(n);
    PushFrame(f);
}

void Capture::EmitStop() {
    // SVC's burst terminator: an EMPTY frame, then encoder reset -- the
    // receiver flushes its jitter buffer + resets its decoder on it.
    EncodedFrame f{};
    f.flags = coop::net::kVoiceFlagStop;
    f.len = 0;
    PushFrame(f);
    if (encoder_) opus_encoder_ctl(static_cast<OpusEncoder*>(encoder_), OPUS_RESET_STATE);
}

void Capture::TickTone() {
    if (!running_ || !toneMode_) return;
    const int64_t now = NowMs();
    double elapsed = static_cast<double>(now - toneLastMs_) + toneCarryMs_;
    toneLastMs_ = now;
    if (elapsed > 200.0) elapsed = 200.0;  // hitch guard: cap the catch-up burst
    while (elapsed >= 20.0) {
        elapsed -= 20.0;
        int16_t buf[kFrameSamples];
        for (int i = 0; i < kFrameSamples; ++i) {
            buf[i] = static_cast<int16_t>(8192.0 * std::sin(tonePhase_));  // ~-12 dBFS
            tonePhase_ += 2.0 * 3.14159265358979 * 440.0 / kSampleRate;
        }
        if (tonePhase_ > 2.0 * 3.14159265358979)
            tonePhase_ = std::fmod(tonePhase_, 2.0 * 3.14159265358979);
        ProcessFrame(buf);
    }
    toneCarryMs_ = elapsed;
}

std::vector<std::string> Capture::EnumerateDevices() {
    std::vector<std::string> names;
    ma_context ctx;
    if (ma_context_init(nullptr, 0, nullptr, &ctx) != MA_SUCCESS) return names;
    ma_device_info* caps = nullptr;
    ma_uint32 capCount = 0;
    if (ma_context_get_devices(&ctx, nullptr, nullptr, &caps, &capCount) == MA_SUCCESS) {
        names.reserve(capCount);
        for (ma_uint32 i = 0; i < capCount; ++i) names.emplace_back(caps[i].name);
    }
    ma_context_uninit(&ctx);
    return names;
}

}  // namespace coop::voice
