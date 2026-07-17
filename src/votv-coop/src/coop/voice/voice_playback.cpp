// coop/voice/voice_playback.cpp -- see coop/voice/voice_playback.h.

#include "coop/voice/voice_playback.h"

#include "ue_wrap/core/log.h"

#include <chrono>
#include <cmath>
#include <cstring>

#include <miniaudio.h>
#include <opus.h>

namespace coop::voice {

namespace {

constexpr int kSampleRate = 48000;
constexpr int kFrameSamples = 960;       // 20 ms
constexpr int kMaxPlcFrames = 5;         // SVC output_buffer_size: gap compensation cap
constexpr int64_t kTalkTimeoutMs = 250;  // SVC TalkCache
constexpr float kVerticalFadeCm = 3200.0f;  // SVC's 32-block vertical fade
constexpr float kPi = 3.14159265358979f;

int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace

// miniaudio playback callback (pUserData = the Playback*); f32 stereo out.
void PlaybackDataCallback(void* dev, void* out, const void* /*in*/, unsigned int frameCount) {
    auto* device = static_cast<ma_device*>(dev);
    auto* self = static_cast<Playback*>(device->pUserData);
    if (!self || !out) return;
    self->MixOutput(static_cast<float*>(out), frameCount);
}

bool Playback::Start(const PlaybackConfig& cfg) {
    if (running_) return true;
    masterVolume_.store(cfg.volume, std::memory_order_relaxed);
    distanceCm_ = cfg.distanceCm;
    jitterThreshold_ = cfg.jitterThreshold;
    prebufferFrames_ = cfg.prebufferFrames;

    auto* ctx = new ma_context();
    if (ma_context_init(nullptr, 0, nullptr, ctx) != MA_SUCCESS) {
        UE_LOGW("voice_playback: ma_context_init failed");
        delete ctx;
        return false;
    }
    context_ = ctx;

    ma_device_id chosenId{};
    bool haveId = false;
    if (!cfg.device.empty()) {
        ma_device_info* outs = nullptr;
        ma_uint32 outCount = 0;
        if (ma_context_get_devices(ctx, &outs, &outCount, nullptr, nullptr) == MA_SUCCESS) {
            for (ma_uint32 i = 0; i < outCount; ++i) {
                if (std::strstr(outs[i].name, cfg.device.c_str())) {
                    chosenId = outs[i].id;
                    haveId = true;
                    UE_LOGI("voice_playback: device '%s'", outs[i].name);
                    break;
                }
            }
        }
        if (!haveId)
            UE_LOGW("voice_playback: no output device matches '%s' -- using default",
                    cfg.device.c_str());
    }

    ma_device_config dc = ma_device_config_init(ma_device_type_playback);
    dc.playback.format = ma_format_f32;
    dc.playback.channels = 2;
    dc.sampleRate = kSampleRate;
    dc.dataCallback = [](ma_device* d, void* o, const void* i, ma_uint32 n) {
        PlaybackDataCallback(d, o, i, n);
    };
    dc.pUserData = this;
    if (haveId) dc.playback.pDeviceID = &chosenId;

    auto* dev = new ma_device();
    if (ma_device_init(ctx, &dc, dev) != MA_SUCCESS) {
        UE_LOGW("voice_playback: ma_device_init(playback) failed -- voice muted locally");
        delete dev;
        ma_context_uninit(ctx);
        delete ctx;
        context_ = nullptr;
        return false;
    }
    if (ma_device_start(dev) != MA_SUCCESS) {
        UE_LOGW("voice_playback: ma_device_start(playback) failed");
        ma_device_uninit(dev);
        delete dev;
        ma_context_uninit(ctx);
        delete ctx;
        context_ = nullptr;
        return false;
    }
    device_ = dev;
    running_ = true;
    UE_LOGI("voice_playback: output running (48 kHz stereo f32, radius %.0f cm, "
            "jitter %d, prebuffer %d)",
            distanceCm_, jitterThreshold_, prebufferFrames_);
    return true;
}

void Playback::Stop() {
    if (!running_) return;
    running_ = false;
    if (device_) {
        auto* dev = static_cast<ma_device*>(device_);
        ma_device_uninit(dev);  // joins the callback; rings are safe to reset after
        delete dev;
        device_ = nullptr;
    }
    if (context_) {
        auto* ctx = static_cast<ma_context*>(context_);
        ma_context_uninit(ctx);
        delete ctx;
        context_ = nullptr;
    }
    for (int i = 0; i < coop::players::kMaxPeers; ++i) ResetSlot(i);
}

void Playback::ResetSlot(int slot) {
    if (slot < 0 || slot >= coop::players::kMaxPeers) return;
    Channel& ch = channels_[slot];
    ch.bufCount = 0;
    ch.flushing = false;
    ch.lastSeq = -1;
    if (ch.decoder) {
        opus_decoder_destroy(static_cast<OpusDecoder*>(ch.decoder));
        ch.decoder = nullptr;
    }
    ch.ringWrite.store(0);
    ch.ringRead.store(0);
    ch.primed.store(false);
    ch.lastFrameMs.store(0);
    ch.whispering.store(false);
    ch.posValid.store(false);
}

void Playback::SetListener(float x, float y, float z, float yawDeg) {
    listenerX_.store(x, std::memory_order_relaxed);
    listenerY_.store(y, std::memory_order_relaxed);
    listenerZ_.store(z, std::memory_order_relaxed);
    listenerYaw_.store(yawDeg, std::memory_order_relaxed);
}

void Playback::SetSpeaker(int slot, float x, float y, float z, bool valid) {
    if (slot < 0 || slot >= coop::players::kMaxPeers) return;
    Channel& ch = channels_[slot];
    ch.posX.store(x, std::memory_order_relaxed);
    ch.posY.store(y, std::memory_order_relaxed);
    ch.posZ.store(z, std::memory_order_relaxed);
    ch.posValid.store(valid, std::memory_order_relaxed);
}

void Playback::SetSlotVolume(int slot, float v) {
    if (slot < 0 || slot >= coop::players::kMaxPeers) return;
    channels_[slot].volume.store(v, std::memory_order_relaxed);
}

float Playback::SlotVolume(int slot) const {
    if (slot < 0 || slot >= coop::players::kMaxPeers) return 1.0f;
    return channels_[slot].volume.load(std::memory_order_relaxed);
}

bool Playback::IsTalking(int slot, bool* whispering) const {
    if (slot < 0 || slot >= coop::players::kMaxPeers) return false;
    const Channel& ch = channels_[slot];
    const int64_t last = ch.lastFrameMs.load(std::memory_order_relaxed);
    if (last == 0 || NowMs() - last >= kTalkTimeoutMs) return false;
    if (whispering) *whispering = ch.whispering.load(std::memory_order_relaxed);
    return true;
}

// ---- jitter buffer (the SVC AudioPacketBuffer port; GT only) ----

void Playback::OnFrame(int slot, const coop::net::VoiceFramePayload& f) {
    if (slot < 0 || slot >= coop::players::kMaxPeers || !running_) return;
    Channel& ch = channels_[slot];

    if (jitterThreshold_ <= 0) {
        DeliverInOrder(ch, f);
        return;
    }
    // Stop marker: flag a full drain (deliver everything buffered, in order,
    // then the stop itself resets the stream state).
    if ((f.flags & coop::net::kVoiceFlagStop) != 0) ch.flushing = true;

    // In-order fast path: empty buffer + exactly-next (or first) seq.
    if (ch.bufCount == 0 && !ch.flushing &&
        (ch.lastSeq < 0 || static_cast<int64_t>(f.seq) == ch.lastSeq + 1)) {
        DeliverInOrder(ch, f);
        return;
    }
    // Insert sorted by seq (drop if full pre-pop fails -- shouldn't happen).
    if (ch.bufCount >= 16) {
        DeliverInOrder(ch, ch.buf[0]);  // pop oldest (accept the gap)
        std::memmove(&ch.buf[0], &ch.buf[1], sizeof(ch.buf[0]) * (--ch.bufCount));
    }
    int at = ch.bufCount;
    while (at > 0 && ch.buf[at - 1].seq > f.seq) --at;
    std::memmove(&ch.buf[at + 1], &ch.buf[at], sizeof(ch.buf[0]) * (ch.bufCount - at));
    ch.buf[at] = f;
    ++ch.bufCount;

    // Past the threshold (or flushing): deliver from the front.
    while (ch.bufCount > 0 && (ch.flushing || ch.bufCount > jitterThreshold_)) {
        coop::net::VoiceFramePayload head = ch.buf[0];
        std::memmove(&ch.buf[0], &ch.buf[1], sizeof(ch.buf[0]) * (--ch.bufCount));
        DeliverInOrder(ch, head);
    }
    if (ch.bufCount == 0) ch.flushing = false;
}

void Playback::TickDecode() {
    if (!running_) return;
    // The jitter buffer delivers eagerly from OnFrame; this drain covers the
    // threshold residue when a burst pauses (deliver aged frames so a short
    // utterance under the threshold still plays out).
    for (int slot = 0; slot < coop::players::kMaxPeers; ++slot) {
        Channel& ch = channels_[slot];
        if (ch.bufCount == 0) continue;
        const int64_t last = ch.lastFrameMs.load(std::memory_order_relaxed);
        // No new frame for ~100 ms but residue buffered: drain it.
        if (last != 0 && NowMs() - last > 100) {
            while (ch.bufCount > 0) {
                coop::net::VoiceFramePayload head = ch.buf[0];
                std::memmove(&ch.buf[0], &ch.buf[1], sizeof(ch.buf[0]) * (--ch.bufCount));
                DeliverInOrder(ch, head);
            }
            ch.flushing = false;
        }
    }
}

// ---- seq handling + decode (the SVC AudioChannel port; GT only) ----

void Playback::DeliverInOrder(Channel& ch, const coop::net::VoiceFramePayload& f) {
    if ((f.flags & coop::net::kVoiceFlagStop) != 0 || f.opusLen == 0) {
        // Burst end: reset stream state; the decoder starts the next burst fresh.
        ch.lastSeq = -1;
        if (ch.decoder)
            opus_decoder_ctl(static_cast<OpusDecoder*>(ch.decoder), OPUS_RESET_STATE);
        return;
    }
    if (!ch.decoder) {
        int err = 0;
        ch.decoder = opus_decoder_create(kSampleRate, 1, &err);
        if (!ch.decoder || err != OPUS_OK) {
            ch.decoder = nullptr;
            return;
        }
    }
    const int64_t seq = static_cast<int64_t>(f.seq);
    if (ch.lastSeq >= 0 && seq <= ch.lastSeq) {
        // A HUGE backward jump is the u32 wire seq wrapping (or a sender stream
        // reset that lost its stop marker) -- a new burst, not a late duplicate
        // (audit M-4). Reset the stream state and decode this frame fresh.
        if (ch.lastSeq - seq <= (int64_t{1} << 30)) return;  // dupe / too-late reorder
        ch.lastSeq = -1;
        opus_decoder_ctl(static_cast<OpusDecoder*>(ch.decoder), OPUS_RESET_STATE);
    }
    if (ch.lastSeq >= 0) {
        const int64_t gap = seq - (ch.lastSeq + 1);
        if (gap > 0 && gap <= kMaxPlcFrames) {
            // Opus PLC for the missing frames.
            int16_t plc[kFrameSamples];
            for (int64_t i = 0; i < gap; ++i) {
                const int n = opus_decode(static_cast<OpusDecoder*>(ch.decoder), nullptr, 0,
                                          plc, kFrameSamples, 0);
                if (n > 0) PushPcm(ch, plc, n);
            }
        } else if (gap > kMaxPlcFrames) {
            opus_decoder_ctl(static_cast<OpusDecoder*>(ch.decoder), OPUS_RESET_STATE);
        }
    }
    ch.lastSeq = seq;

    int16_t pcm[kFrameSamples];
    const int n = opus_decode(static_cast<OpusDecoder*>(ch.decoder), f.opus, f.opusLen,
                              pcm, kFrameSamples, 0);
    if (n <= 0) return;
    PushPcm(ch, pcm, n);

    ch.whispering.store((f.flags & coop::net::kVoiceFlagWhisper) != 0,
                        std::memory_order_relaxed);
    ch.lastFrameMs.store(NowMs(), std::memory_order_relaxed);
}

void Playback::PushPcm(Channel& ch, const int16_t* samples, int count) {
    const uint64_t write = ch.ringWrite.load(std::memory_order_relaxed);
    const uint64_t read = ch.ringRead.load(std::memory_order_acquire);
    const uint64_t free = Channel::kRingSamples - (write - read);
    if (static_cast<uint64_t>(count) > free) return;  // overrun: drop (jitter-buffer-grade loss)
    for (int i = 0; i < count; ++i)
        ch.ring[(write + i) % Channel::kRingSamples] = samples[i];
    ch.ringWrite.store(write + count, std::memory_order_release);
    // Prebuffer: start the mixer only once ~100 ms is queued (SVC's silence
    // pre-fill equivalent -- absorbs network jitter without mid-word gaps).
    if (!ch.primed.load(std::memory_order_relaxed) &&
        (write + count - read) >= static_cast<uint64_t>(prebufferFrames_ * kFrameSamples)) {
        ch.primed.store(true, std::memory_order_release);
    }
}

// ---- the mixer (miniaudio callback thread) ----

void Playback::MixOutput(float* out, uint32_t frameCount) {
    std::memset(out, 0, sizeof(float) * 2 * frameCount);
    const float lx = listenerX_.load(std::memory_order_relaxed);
    const float ly = listenerY_.load(std::memory_order_relaxed);
    const float lz = listenerZ_.load(std::memory_order_relaxed);
    const float yawRad = listenerYaw_.load(std::memory_order_relaxed) * kPi / 180.0f;
    const float master = masterVolume_.load(std::memory_order_relaxed);

    for (int slot = 0; slot < coop::players::kMaxPeers; ++slot) {
        Channel& ch = channels_[slot];
        if (!ch.primed.load(std::memory_order_acquire)) continue;
        const uint64_t write = ch.ringWrite.load(std::memory_order_acquire);
        const uint64_t read = ch.ringRead.load(std::memory_order_relaxed);
        const uint64_t avail = write - read;
        if (avail == 0) {
            // Underrun: re-prebuffer before this channel speaks again.
            ch.primed.store(false, std::memory_order_release);
            continue;
        }

        // Spatial params once per callback block (~10 ms).
        float gainL = 1.0f, gainR = 1.0f;
        if (ch.posValid.load(std::memory_order_relaxed)) {
            const float dx = ch.posX.load(std::memory_order_relaxed) - lx;
            const float dy = ch.posY.load(std::memory_order_relaxed) - ly;
            const float dz = ch.posZ.load(std::memory_order_relaxed) - lz;
            const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            const float maxDist =
                ch.whispering.load(std::memory_order_relaxed) ? distanceCm_ * 0.5f
                                                              : distanceCm_;
            float att = maxDist > 0 ? 1.0f - dist / maxDist : 0.0f;  // AL_LINEAR_DISTANCE
            if (att < 0) att = 0;
            if (att > 1) att = 1;
            // Vertical fade (SVC: 1 - |dy_up| / 32 blocks).
            float vfade = 1.0f - std::fabs(dz) / kVerticalFadeCm;
            if (vfade < 0) vfade = 0;
            // SVC REDUCED-mode pan: lateral/forward in listener space (UE:
            // yaw 0 = +X, right = (-sin yaw, cos yaw)).
            const float fwd = dx * std::cos(yawRad) + dy * std::sin(yawRad);
            const float lat = -dx * std::sin(yawRad) + dy * std::cos(yawRad);
            float pan = 0.0f;
            if (dist > 1.0f) {
                pan = std::atan2(lat, fwd) / kPi;  // [-1, 1]
                if (pan > 0.5f) pan = 0.5f;        // SVC clamps to +-0.5
                if (pan < -0.5f) pan = -0.5f;
            }
            float volL = 1.0f - pan * 1.4f;
            float volR = 1.0f + pan * 1.4f;
            if (volL > 1) volL = 1;
            if (volL < 0.3f) volL = 0.3f;
            if (volR > 1) volR = 1;
            if (volR < 0.3f) volR = 0.3f;
            const float g = att * vfade;
            gainL = g * volL;
            gainR = g * volR;
        }
        const float vol = master * ch.volume.load(std::memory_order_relaxed);
        gainL *= vol;
        gainR *= vol;

        const uint32_t take =
            avail < frameCount ? static_cast<uint32_t>(avail) : frameCount;
        for (uint32_t i = 0; i < take; ++i) {
            const float s =
                static_cast<float>(ch.ring[(read + i) % Channel::kRingSamples]) / 32768.0f;
            out[2 * i + 0] += s * gainL;
            out[2 * i + 1] += s * gainR;
        }
        ch.ringRead.store(read + take, std::memory_order_release);
        // (take < frameCount leaves silence for the tail -- the underrun
        // re-prime triggers on the next callback when avail hits 0.)
    }

    // Soft clip.
    const uint32_t n = 2 * frameCount;
    for (uint32_t i = 0; i < n; ++i) {
        if (out[i] > 1.0f) out[i] = 1.0f;
        else if (out[i] < -1.0f) out[i] = -1.0f;
    }
}

std::vector<std::string> Playback::EnumerateDevices() {
    std::vector<std::string> names;
    ma_context ctx;
    if (ma_context_init(nullptr, 0, nullptr, &ctx) != MA_SUCCESS) return names;
    ma_device_info* outs = nullptr;
    ma_uint32 outCount = 0;
    if (ma_context_get_devices(&ctx, &outs, &outCount, nullptr, nullptr) == MA_SUCCESS) {
        names.reserve(outCount);
        for (ma_uint32 i = 0; i < outCount; ++i) names.emplace_back(outs[i].name);
    }
    ma_context_uninit(&ctx);
    return names;
}

}  // namespace coop::voice
