// coop/voice/voice_chat.cpp -- see coop/voice/voice_chat.h.

#include "coop/voice/voice_chat.h"

#include "ui/input_focus.h"  // IsOurWindowForeground -- the cross-process key gate
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/voice/voice_capture.h"
#include "coop/voice/voice_playback.h"

#include "coop/config/config.h"

#include "ue_wrap/engine.h"
#include "ue_wrap/log.h"

#include <Windows.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace coop::voice_chat {
namespace {

namespace CFG = coop::config;
using Clock = std::chrono::steady_clock;

std::atomic<coop::net::Session*> g_session{nullptr};

coop::voice::Capture g_capture;
coop::voice::Playback g_playback;

// Atomic: Enabled() is read from the RENDER thread (hud::IsActive each Present)
// while the game thread writes at Install/OnDisconnect (audit I-1). All other
// access is game-thread; the implicit seq_cst ops are fine on these paths.
std::atomic<bool> g_enabled{false};
std::atomic<bool> g_started{false};  // devices opened this session
bool g_loopback = false;
int  g_muteVk = 0;
bool g_muteKeyWasDown = false;
uint32_t g_sendSeq = 0;

// Display-only per-slot wire state (VoiceState).
struct WireState {
    bool micMuted = false;
    bool voiceDisabled = false;
    bool known = false;
};
WireState g_peerState[coop::net::kMaxPeers];

// Our last-broadcast state (edge detector).
bool g_sentMuted = false;
bool g_sentDisabled = false;
bool g_sentOnce = false;

// Session-start grace for the HUD "disconnected" icon (SVC's 5 s startup grace).
Clock::time_point g_installAt{};

// Render-thread surface.
std::mutex g_uiMutex;
UiSnapshot g_uiSnap;
bool g_captureOk = false;
bool g_playbackOk = false;
std::atomic<bool> g_restartRequested{false};

int ParseKey(const std::string& s, int def) {
    if (s.empty()) return def;
    if (s.size() == 1) return std::toupper(static_cast<unsigned char>(s[0]));
    return static_cast<int>(std::strtol(s.c_str(), nullptr, 0));
}

float ParseF(const std::string& s, float def) {
    if (s.empty()) return def;
    return static_cast<float>(std::atof(s.c_str()));
}

bool EnvOrIniBool(const char* env, const char* iniKey, bool def) {
    const std::string e = CFG::ReadEnv(env);
    if (!e.empty()) return e != "0";
    const std::string v = CFG::ReadIniValue(iniKey, def ? "1" : "0");
    return v != "0";
}

void SendLocalState(coop::net::Session* s) {
    coop::net::VoiceStatePayload p{};
    p.micMuted = g_capture.Muted() ? 1 : 0;
    p.voiceDisabled = g_enabled ? 0 : 1;
    s->SendReliable(coop::net::ReliableKind::VoiceState, &p, sizeof(p));
    g_sentMuted = p.micMuted != 0;
    g_sentDisabled = p.voiceDisabled != 0;
    g_sentOnce = true;
}

// Open (or reopen) both devices from the current voice.* config. Game thread
// only -- Install + the panel-requested restart path.
void StartDevices() {
    coop::voice::CaptureConfig cc;
    // Mode + threshold take env overrides like the other voice keys (the
    // autonomous tone smoke must force activation mode + a permissive
    // threshold regardless of what the user's ini currently holds).
    std::string mode = CFG::ReadEnv("VOTVCOOP_VOICE_MODE");
    if (mode.empty()) mode = CFG::ReadIniValue("voice.mode", "ptt");
    cc.activationMode = mode == "activation";
    // Default PTT 'G' (user 2026-06-12; was X -- clashed with VOTV binds). An
    // explicit voice.ptt_key in the ini still wins.
    cc.pttVk = ParseKey(CFG::ReadIniValue("voice.ptt_key", "G"), 'G');
    cc.whisperVk = ParseKey(CFG::ReadIniValue("voice.whisper_key", ""), 0);
    const std::string thrEnv = CFG::ReadEnv("VOTVCOOP_VOICE_THRESHOLD_DB");
    cc.thresholdDb = ParseF(!thrEnv.empty() ? thrEnv
                                            : CFG::ReadIniValue("voice.threshold_db", "-50"),
                            -50.0f);
    cc.gainDb = ParseF(CFG::ReadIniValue("voice.mic_gain_db", "0"), 0.0f);
    cc.device = CFG::ReadIniValue("voice.mic_device", "");
    cc.testTone = EnvOrIniBool("VOTVCOOP_VOICE_TEST_TONE", "voice.test_tone", false);
    g_muteVk = ParseKey(CFG::ReadIniValue("voice.mute_key", ""), 0);
    g_loopback = EnvOrIniBool("VOTVCOOP_VOICE_LOOPBACK", "voice.loopback", false);

    coop::voice::PlaybackConfig pc;
    pc.device = CFG::ReadIniValue("voice.output_device", "");
    pc.volume = ParseF(CFG::ReadIniValue("voice.volume", "1.0"), 1.0f);
    pc.distanceCm = ParseF(CFG::ReadIniValue("voice.distance_cm", "4800"), 4800.0f);
    pc.jitterThreshold =
        static_cast<int>(ParseF(CFG::ReadIniValue("voice.jitter_threshold", "3"), 3.0f));
    pc.prebufferFrames =
        static_cast<int>(ParseF(CFG::ReadIniValue("voice.prebuffer_frames", "5"), 5.0f));

    g_playbackOk = g_playback.Start(pc);
    g_captureOk = g_capture.Start(cc);
    UE_LOGI("voice_chat: devices (capture=%s playback=%s loopback=%d tone=%d)",
            g_captureOk ? "ok" : "OFF", g_playbackOk ? "ok" : "OFF",
            g_loopback ? 1 : 0, cc.testTone ? 1 : 0);
}

void PublishUiSnapshot(coop::net::Session* s) {
    UiSnapshot snap;
    auto& reg = coop::players::Registry::Get();
    snap.localSlot = reg.LocalPeerId();
    for (int i = 0; i < coop::players::kMaxPeers; ++i) {
        snap.icons[i] = static_cast<uint8_t>(
            i == snap.localSlot ? LocalHudIcon() : IconForSlot(i));
        snap.slotVolume[i] = g_playback.SlotVolume(i);
    }
    snap.localIcon = static_cast<uint8_t>(LocalHudIcon());
    snap.enabled = g_enabled ? 1 : 0;
    snap.started = g_started ? 1 : 0;
    snap.muted = g_capture.Muted() ? 1 : 0;
    snap.activationMode = g_capture.ActivationMode() ? 1 : 0;
    snap.captureOk = g_captureOk ? 1 : 0;
    snap.playbackOk = g_playbackOk ? 1 : 0;
    snap.transmitting = g_capture.IsTransmitting() ? 1 : 0;
    snap.micLevelDb = g_capture.MicLevelDb();
    snap.masterVolume = g_playback.MasterVolume();
    snap.thresholdDb = g_capture.ThresholdDb();
    snap.gainDb = g_capture.GainDb();
    (void)s;
    std::lock_guard<std::mutex> lk(g_uiMutex);
    g_uiSnap = snap;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_started) return;  // idempotent per session (Install is the session-start edge)

    g_enabled = EnvOrIniBool("VOTVCOOP_VOICE_ENABLED", "voice.enabled", true);
    g_installAt = Clock::now();
    for (auto& st : g_peerState) st = WireState{};
    g_sendSeq = 0;
    g_sentOnce = false;

    if (!g_enabled) {
        UE_LOGI("voice_chat: disabled (voice.enabled=0)");
        return;
    }
    StartDevices();
    g_started = true;
}

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || !g_started || !g_enabled) return;

    // Panel-requested device reopen (ini already rewritten by the panel).
    if (g_restartRequested.exchange(false, std::memory_order_acq_rel)) {
        g_capture.Stop();
        g_playback.Stop();
        StartDevices();
    }

    // Mute-key toggle edge (GT poll, GetAsyncKeyState -- foreground-gated: the key is
    // global across processes, same hazard as the capture-thread PTT gate). Also
    // text-capture-gated (2026-07-09): no mute toggle while typing the key into chat.
    if (g_muteVk != 0) {
        const bool down = ui::input_focus::IsOurWindowForeground() &&
                          !ui::input_focus::IsOverlayCapturingText() &&
                          (GetAsyncKeyState(g_muteVk) & 0x8000) != 0;
        if (down && !g_muteKeyWasDown) g_capture.SetMuted(!g_capture.Muted());
        g_muteKeyWasDown = down;
    }

    // Tone mode synthesizes on the game thread.
    g_capture.TickTone();

    auto& reg = coop::players::Registry::Get();
    const uint8_t localSlot = reg.LocalPeerId();

    // Outbound: drain encoded frames, stamp the voice seq, send (+loopback).
    coop::voice::EncodedFrame ef;
    while (g_capture.PopFrame(ef)) {
        coop::net::VoiceFramePayload f{};
        f.flags = ef.flags;
        f.opusLen = ef.len;
        f.seq = g_sendSeq++;
        if (ef.len) std::memcpy(f.opus, ef.opus, ef.len);
        if (s->connected()) s->SendVoiceFrame(f);
        if (g_loopback)
            g_playback.OnFrame(localSlot != coop::players::kPeerIdUnknown ? localSlot : 0, f);
    }

    // Inbound: session voice inbox -> jitter buffers. One batch drain = one
    // lock per tick (audit I-3/M-3); the buffer covers every ring full.
    coop::net::Session::VoiceFrameMsg
        inbox[coop::net::kMaxPeers * coop::net::Session::kVoiceRingPerSlot];
    const int got = s->DrainVoiceFrames(inbox, static_cast<int>(coop::net::kMaxPeers) *
                                                   coop::net::Session::kVoiceRingPerSlot);
    for (int i = 0; i < got; ++i) {
        const auto& vm = inbox[i];
        if (vm.senderSlot >= 0 && vm.senderSlot < coop::net::kMaxPeers &&
            vm.senderSlot != localSlot)  // own relayed echo never plays (host never relays back, belt+braces)
            g_playback.OnFrame(vm.senderSlot, vm.frame);
    }
    g_playback.TickDecode();

    // Listener/speaker positions for the spatial mixer, throttled to ~20 Hz (audit
    // I-2): the block costs 2 reflected PE calls + up to kMaxPeers-1 head reads, and
    // sub-50ms position freshness is inaudible in the attenuation/pan math (voice
    // frames themselves arrive at 20 ms). The mixer interpolates nothing -- it just
    // reads the latest atomics.
    static Clock::time_point s_lastPosAt{};
    const auto now = Clock::now();
    if (now - s_lastPosAt >= std::chrono::milliseconds(50)) {
        s_lastPosAt = now;
        // Listener = the local player (actor pos ~ head at voice ranges; yaw =
        // actor yaw -- VOTV is first-person, the body follows the camera).
        void* local = reg.Local();
        if (local) {
            const ue_wrap::FVector p = ue_wrap::engine::GetActorLocation(local);
            const ue_wrap::FRotator r = ue_wrap::engine::GetActorRotation(local);
            g_playback.SetListener(p.X, p.Y, p.Z, r.Yaw);
            if (g_loopback && localSlot != coop::players::kPeerIdUnknown)
                g_playback.SetSpeaker(localSlot, p.X, p.Y, p.Z, true);
        }
        // Speakers = the peer puppets' heads.
        for (int slot = 0; slot < coop::net::kMaxPeers; ++slot) {
            if (slot == localSlot) continue;
            coop::RemotePlayer* rp = reg.Puppet(static_cast<uint8_t>(slot));
            if (rp && rp->GetActor()) {
                const ue_wrap::FVector hp = rp->GetHeadPosition();
                g_playback.SetSpeaker(slot, hp.X, hp.Y, hp.Z, true);
            } else {
                g_playback.SetSpeaker(slot, 0, 0, 0, false);
            }
        }
    }

    // VoiceState edges (display-only; first connected tick sends the baseline).
    // voiceDisabled is part of the edge (audit I-4): today it is constant inside this
    // gate (Tick early-returns when disabled), but the edge must stay correct if a
    // runtime enable toggle ever lands.
    if (s->connected()) {
        const bool muted = g_capture.Muted();
        const bool disabled = !g_enabled;
        if (!g_sentOnce || muted != g_sentMuted || disabled != g_sentDisabled)
            SendLocalState(s);
    }

    PublishUiSnapshot(s);
}

void OnVoiceState(const coop::net::VoiceStatePayload& p, uint8_t senderSlot) {
    if (senderSlot >= coop::net::kMaxPeers) return;
    g_peerState[senderSlot].micMuted = p.micMuted != 0;
    g_peerState[senderSlot].voiceDisabled = p.voiceDisabled != 0;
    g_peerState[senderSlot].known = true;
}

void ReplayPeerStatesToSlot(int peerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    for (int slot = 0; slot < coop::net::kMaxPeers; ++slot) {
        if (slot == peerSlot || !g_peerState[slot].known) continue;
        coop::net::VoiceStatePayload p{};
        p.micMuted = g_peerState[slot].micMuted ? 1 : 0;
        p.voiceDisabled = g_peerState[slot].voiceDisabled ? 1 : 0;
        s->SendReliableToSlot(peerSlot, coop::net::ReliableKind::VoiceState, &p, sizeof(p),
                              static_cast<uint8_t>(slot));
    }
    // Our own current state too (the host is slot 0).
    if (g_started && g_enabled) {
        coop::net::VoiceStatePayload p{};
        p.micMuted = g_capture.Muted() ? 1 : 0;
        p.voiceDisabled = 0;
        s->SendReliableToSlot(peerSlot, coop::net::ReliableKind::VoiceState, &p, sizeof(p), 0);
    }
}

void OnDisconnectSlot(int slot) {
    if (slot < 0 || slot >= coop::net::kMaxPeers) return;
    g_playback.ResetSlot(slot);
    g_peerState[slot] = WireState{};
}

void OnDisconnect() {
    g_capture.Stop();
    g_playback.Stop();
    g_started = false;
    g_sentOnce = false;
    g_sendSeq = 0;
    for (auto& st : g_peerState) st = WireState{};
}

// ---- UI surface ----

PeerVoiceState PeerVoiceStateFor(int slot) {
    PeerVoiceState out;
    if (slot < 0 || slot >= coop::net::kMaxPeers) return out;
    bool whisper = false;
    out.talking = g_playback.IsTalking(slot, &whisper);
    out.whispering = whisper;
    out.micMuted = g_peerState[slot].micMuted;
    out.voiceDisabled = g_peerState[slot].voiceDisabled;
    return out;
}

VoiceIcon IconForSlot(int slot) {
    // The SVC nameplate priority chain (design SS3.1), groups skipped; the
    // MicMuted extension is our deliberate divergence (VoiceState carries it).
    const PeerVoiceState st = PeerVoiceStateFor(slot);
    if (st.whispering) return VoiceIcon::Whispering;
    if (st.talking) return VoiceIcon::Talking;
    if (st.voiceDisabled) return VoiceIcon::Disabled;
    if (st.micMuted) return VoiceIcon::MicMuted;
    return VoiceIcon::None;
}

VoiceIcon LocalHudIcon() {
    if (!g_enabled) return VoiceIcon::Disabled;
    if (!g_started) return VoiceIcon::None;
    auto* s = g_session.load(std::memory_order_acquire);
    const bool sessionUp = s && s->running();
    if (sessionUp && !s->connected() &&
        Clock::now() - g_installAt > std::chrono::seconds(5))
        return VoiceIcon::Disconnected;  // transport down past the startup grace
    if (g_capture.Muted()) return VoiceIcon::MicMuted;  // (PTT distinction lives in the renderer)
    if (g_capture.IsWhispering()) return VoiceIcon::Whispering;
    if (g_capture.IsTransmitting()) return VoiceIcon::Talking;
    return VoiceIcon::None;
}

bool Enabled() { return g_enabled && g_started; }
bool Muted() { return g_capture.Muted(); }
void SetMuted(bool m) { g_capture.SetMuted(m); }
float MicLevelDb() { return g_capture.MicLevelDb(); }
float MasterVolume() { return g_playback.MasterVolume(); }
void SetMasterVolume(float v) { g_playback.SetMasterVolume(v); }

void GetUiSnapshot(UiSnapshot& out) {
    std::lock_guard<std::mutex> lk(g_uiMutex);
    out = g_uiSnap;
}
void SetSlotVolume(int slot, float v) { g_playback.SetSlotVolume(slot, v); }
float SlotVolume(int slot) { return g_playback.SlotVolume(slot); }
void SetThresholdDb(float db) { g_capture.SetThresholdDb(db); }
void SetGainDb(float db) { g_capture.SetGainDb(db); }
void RequestDevicesRestart() { g_restartRequested.store(true, std::memory_order_release); }

}  // namespace coop::voice_chat
