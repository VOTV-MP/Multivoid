// ui/voice_panel.cpp -- see ui/voice_panel.h.

#include "ui/voice_panel.h"

#include "coop/voice/voice_capture.h"
#include "coop/voice/voice_chat.h"
#include "coop/voice/voice_playback.h"
#include "coop/config/config.h"
#include "ui/scale.h"
#include "ui/voice_icons.h"

#include "imgui.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

namespace ui::voice_panel {
namespace {

namespace VC = coop::voice_chat;

// Atomic: Toggle/Close run on the game-window WndProc thread (the V key) while
// IsOpen/Render read on the render thread (audit 2026-06-12 IMPROVE-2).
std::atomic<bool> g_open{false};

// Device lists are enumerated once per panel-open (and on Rescan) -- never
// per frame (ma_context_init walks the audio stack). The PTT key label is
// cached the same way: ReadIniValue is a file read, not per-frame material
// (audit 2026-06-12 IMPROVE-1).
bool g_devicesFresh = false;
std::vector<std::string> g_micDevices;
std::vector<std::string> g_outDevices;
char g_micCurrent[160] = {};
char g_outCurrent[160] = {};
char g_pttKey[32] = {};

void RefreshDevices() {
    g_micDevices = coop::voice::Capture::EnumerateDevices();
    g_outDevices = coop::voice::Playback::EnumerateDevices();
    std::snprintf(g_micCurrent, sizeof(g_micCurrent), "%s",
                  coop::config::ReadIniValue("voice.mic_device", "").c_str());
    std::snprintf(g_outCurrent, sizeof(g_outCurrent), "%s",
                  coop::config::ReadIniValue("voice.output_device", "").c_str());
    std::snprintf(g_pttKey, sizeof(g_pttKey), "%s",
                  coop::config::ReadIniValue("voice.ptt_key", "G").c_str());
    g_devicesFresh = true;
}

// A device combo: "(system default)" + the enumerated names. Writes the ini
// + requests the GT reopen on selection.
void DeviceCombo(const char* label, const char* iniKey, char* current, size_t currentCap,
                 const std::vector<std::string>& names) {
    const char* shown = current[0] ? current : "(system default)";
    if (ImGui::BeginCombo(label, shown)) {
        if (ImGui::Selectable("(system default)", !current[0])) {
            current[0] = 0;
            coop::config::WriteIniValue(iniKey, "");
            VC::RequestDevicesRestart();
        }
        for (const std::string& n : names) {
            const bool sel = n == current;
            if (ImGui::Selectable(n.c_str(), sel)) {
                std::snprintf(current, currentCap, "%s", n.c_str());
                coop::config::WriteIniValue(iniKey, n.c_str());
                VC::RequestDevicesRestart();
            }
        }
        ImGui::EndCombo();
    }
}

}  // namespace

void Toggle() {
    g_open = !g_open;
    if (g_open) g_devicesFresh = false;  // re-enumerate on each open
}

void Close() { g_open = false; }

bool IsOpen() { return g_open; }

void Render() {
    if (!g_open.load(std::memory_order_relaxed)) return;
    if (!g_devicesFresh) RefreshDevices();

    VC::UiSnapshot s;
    VC::GetUiSnapshot(s);

    const ImGuiIO& io = ImGui::GetIO();
    using ui::scale::S;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.45f),
                            ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(S(380.0f), 0.0f), ImGuiCond_Always);
    // Begin's close-X needs a plain bool*; bridge the atomic through a local
    // and write back the X-close at the end (both early-out and tail paths).
    bool open = true;
    if (ImGui::Begin("Voice chat###coop_voice_panel", &open,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoSavedSettings)) {
        if (!s.enabled) {
            ImGui::TextWrapped("Voice chat is turned off (voice.enabled=0 in votv-coop.ini). "
                               "Set it to 1 and restart the game to use voice.");
            ImGui::End();
            if (!open) g_open.store(false, std::memory_order_relaxed);
            return;
        }
        if (!s.captureOk)
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f),
                               "No microphone -- voice is receive-only.");
        if (!s.playbackOk)
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f),
                               "No output device -- you can't hear voice.");

        // Mic meter row: live level bar + the local state icon.
        {
            const float frac = std::clamp((s.micLevelDb + 60.0f) / 60.0f, 0.0f, 1.0f);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Mic");
            ImGui::SameLine(S(60.0f));
            const ImVec4 barCol = s.transmitting ? ImVec4(0.30f, 0.80f, 0.40f, 1.0f)
                                                 : ImVec4(0.45f, 0.50f, 0.56f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, barCol);
            char db[24];
            std::snprintf(db, sizeof(db), "%.0f dB", s.micLevelDb);
            ImGui::ProgressBar(frac, ImVec2(-S(34.0f), 0.0f), db);
            ImGui::PopStyleColor();
            ImGui::SameLine();
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ui::voice_icons::Draw(ImGui::GetWindowDrawList(),
                                  ImVec2(p.x + S(9.0f), p.y + ImGui::GetTextLineHeight() * 0.6f),
                                  S(16.0f), static_cast<VC::VoiceIcon>(s.localIcon), 1.0f);
            ImGui::Dummy(ImVec2(S(20.0f), ImGui::GetTextLineHeight()));
        }

        bool muted = s.muted != 0;
        if (ImGui::Checkbox("Mute my microphone", &muted)) VC::SetMuted(muted);

        ImGui::Spacing();
        ImGui::SeparatorText("Activation");
        int mode = s.activationMode ? 1 : 0;
        char pttLabel[64];
        std::snprintf(pttLabel, sizeof(pttLabel), "Push-to-talk (key: %s)", g_pttKey);
        if (ImGui::RadioButton(pttLabel, &mode, 0)) {
            coop::config::WriteIniValue("voice.mode", "ptt");
            VC::RequestDevicesRestart();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Hold the key to talk. Change the key via voice.ptt_key\n"
                              "in votv-coop.ini (single letter or a virtual-key number).");
        if (ImGui::RadioButton("Voice activation", &mode, 1)) {
            coop::config::WriteIniValue("voice.mode", "activation");
            VC::RequestDevicesRestart();
        }
        if (mode == 1) {
            float thr = s.thresholdDb;
            if (ImGui::SliderFloat("Threshold", &thr, -100.0f, 0.0f, "%.0f dB"))
                VC::SetThresholdDb(thr);
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                char v[16];
                std::snprintf(v, sizeof(v), "%.0f", thr);
                coop::config::WriteIniValue("voice.threshold_db", v);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Speech louder than this transmits. Watch the mic\n"
                                  "meter: set the slider just above your room's noise.");
            // 0 dB == full scale: nothing ever transmits. The user's round-1 debugging
            // parked it there; warn instead of silently never activating.
            if (thr > -5.0f)
                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f),
                                   "Threshold near 0 dB -- the mic will almost never "
                                   "trigger. Try -50 dB.");
        }

        ImGui::Spacing();
        ImGui::SeparatorText("Levels");
        float gain = s.gainDb;
        if (ImGui::SliderFloat("Mic gain", &gain, -40.0f, 24.0f, "%+.0f dB"))
            VC::SetGainDb(gain);
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            char v[16];
            std::snprintf(v, sizeof(v), "%.0f", gain);
            coop::config::WriteIniValue("voice.mic_gain_db", v);
        }
        float vol = s.masterVolume;
        if (ImGui::SliderFloat("Voice volume", &vol, 0.0f, 3.0f, "%.2fx"))
            VC::SetMasterVolume(vol);
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            char v[16];
            std::snprintf(v, sizeof(v), "%.2f", vol);
            coop::config::WriteIniValue("voice.volume", v);
        }

        ImGui::Spacing();
        ImGui::SeparatorText("Devices");
        DeviceCombo("Microphone", "voice.mic_device", g_micCurrent, sizeof(g_micCurrent),
                    g_micDevices);
        DeviceCombo("Output", "voice.output_device", g_outCurrent, sizeof(g_outCurrent),
                    g_outDevices);
        if (ImGui::SmallButton("Rescan devices")) RefreshDevices();
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Device changes apply immediately (the voice engine\n"
                              "reopens on the game's next tick).");
    }
    ImGui::End();
}

}  // namespace ui::voice_panel
