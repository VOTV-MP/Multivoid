// ui/voice_panel.cpp -- see ui/voice_panel.h.

#include "ui/voice_panel.h"

#include "coop/voice/voice_capture.h"
#include "coop/voice/voice_chat.h"
#include "coop/voice/voice_playback.h"
#include "coop/config/config.h"
#include "coop/player/roster.h"
#include "l10n/l10n.h"
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

// Atomic because the panel's open flag is written and read from three threads: Toggle runs on
// the game-window WndProc thread (the V key); Close is reached from there, from the render thread's
// re-fault guard, and from the game thread at session teardown; IsOpen and Render are read on the
// WndProc and render threads both.
std::atomic<bool> g_open{false};

// Device lists are enumerated once per panel-open (and on Rescan), never per frame:
// ma_context_init walks the audio stack. The PTT key label is cached the same way, since
// ReadIniValue is a file read.
bool g_devicesFresh = false;
std::vector<std::string> g_micDevices;
std::vector<std::string> g_outDevices;
char g_micCurrent[160] = {};
char g_outCurrent[160] = {};
char g_pttKey[32] = {};

// Each slider's handle value -- stored on every frame the handle is held, and on the frame a
// typed value (Ctrl+click, Enter) applies, when the item is already inactive -- and whether an
// edit is open (a drag, or a typed value not yet committed). Written by Render and read by the
// release branch and by CommitAbandonedDrag, all on the render thread; Close(), which runs on
// three threads, never touches them. The release branch commits the handle's own value from
// these, and an edit the panel's closing abandons is committed from these.
std::atomic<float> g_pendingThreshold{0.0f};
std::atomic<float> g_pendingGain{0.0f};
std::atomic<float> g_pendingVolume{0.0f};
std::atomic<bool>  g_draggingThreshold{false};
std::atomic<bool>  g_draggingGain{false};
std::atomic<bool>  g_draggingVolume{false};

// The one path from a slider to its row: the release branch and CommitAbandonedDrag. fmt is the
// slider's own format, so the row holds what the handle showed.
void CommitSlider(const coop::config_registry::FloatRow& row, float v, const char* fmt) {
    char text[16];
    std::snprintf(text, sizeof(text), fmt, v);
    coop::config::SetValue(row, text);
}

void RefreshDevices() {
    g_micDevices = coop::voice::Capture::EnumerateDevices();
    g_outDevices = coop::voice::Playback::EnumerateDevices();
    std::snprintf(g_micCurrent, sizeof(g_micCurrent), "%s",
                  coop::config::ResolveString(coop::config_registry::rows::voice_mic_device).c_str());
    std::snprintf(g_outCurrent, sizeof(g_outCurrent), "%s",
                  coop::config::ResolveString(coop::config_registry::rows::voice_output_device).c_str());
    std::snprintf(g_pttKey, sizeof(g_pttKey), "%s",
                  coop::config::ResolveString(coop::config_registry::rows::voice_ptt_key).c_str());
    g_devicesFresh = true;
}

// A device combo: "(system default)" + the enumerated names. Sets the row on selection; the row's
// subscriber reopens the devices.
void DeviceCombo(const char* text, const char* id, const coop::config_registry::StringRow& row,
                 char* current, size_t currentCap, const std::vector<std::string>& names) {
    const char* shown = current[0] ? current : l10n::T("(system default)");
    if (ImGui::BeginCombo(l10n::Label(text, id), shown)) {
        if (ImGui::Selectable(l10n::Label(l10n::T("(system default)"), "system_default"), !current[0])) {
            current[0] = 0;
            coop::config::SetValue(row, "");
        }
        for (const std::string& n : names) {
            const bool sel = n == current;
            if (ImGui::Selectable(n.c_str(), sel)) {
                std::snprintf(current, currentCap, "%s", n.c_str());
                coop::config::SetValue(row, n.c_str());
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

void CommitAbandonedDrag() {
    if (g_open.load(std::memory_order_relaxed)) return;
    if (g_draggingThreshold.exchange(false))
        CommitSlider(::coop::config_registry::rows::voice_threshold_db, g_pendingThreshold.load(),
                     "%.0f");
    if (g_draggingGain.exchange(false))
        CommitSlider(::coop::config_registry::rows::voice_mic_gain_db, g_pendingGain.load(), "%.0f");
    if (g_draggingVolume.exchange(false))
        CommitSlider(::coop::config_registry::rows::voice_volume, g_pendingVolume.load(), "%.2f");
}

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
    // Begin's close-X needs a plain bool*, so the atomic is bridged through a local and written
    // back by a destructor rather than at each exit. Written back by hand, it has to be repeated on
    // every path out of this function, and the one path that forgets it is invisible: with voice
    // enabled -- the normal case, and the only one a player ever sees -- the X cleared the local,
    // nothing consumed it, g_open stayed true, and the next frame reopened the window with a fresh
    // open=true. V kept working throughout, because V drives g_open directly. A destructor cannot
    // be forgotten by a branch someone adds later.
    bool open = true;
    struct CloseOnX {
        const bool& open;
        ~CloseOnX() {
            if (!open) g_open.store(false, std::memory_order_relaxed);
        }
    } closeOnX{open};
    if (ImGui::Begin(l10n::Label(l10n::T("Voice chat"), "coop_voice_panel"), &open,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoSavedSettings)) {
        if (!s.enabled) {
            char off[512];
            l10n::Fmt(off, sizeof(off),
                      l10n::T("Voice chat is turned off (%s=0 in multivoid.ini). "
                              "Set it to 1 and restart the game to use voice."),
                      "voice.enabled");
            ImGui::TextWrapped("%s", off);
            ImGui::End();
            return;  // closeOnX writes the X back on the way out
        }
        if (!s.captureOk)
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "%s",
                               l10n::T("No microphone -- voice is receive-only."));
        if (!s.playbackOk)
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s",
                               l10n::T("No output device -- you can't hear voice."));

        // Mic meter row: live level bar + the local state icon.
        {
            const float frac = std::clamp((s.micLevelDb + 60.0f) / 60.0f, 0.0f, 1.0f);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(l10n::T("Mic"));
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
        if (ImGui::Checkbox(l10n::Label(l10n::T("Mute my microphone"), "mute_my_microphone"), &muted))
            VC::SetMuted(muted);

        ImGui::Spacing();
        ImGui::SeparatorText(l10n::Label(l10n::T("Activation"), "activation"));
        int mode = s.activationMode ? 1 : 0;
        char pttLabel[256];
        l10n::Fmt(pttLabel, sizeof(pttLabel), l10n::T("Push-to-talk (key: %s)"), g_pttKey);
        if (ImGui::RadioButton(l10n::Label(pttLabel, "push_to_talk"), &mode, 0))
            coop::config::SetValue(::coop::config_registry::rows::voice_mode, "ptt");
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            char tip[512];
            l10n::Fmt(tip, sizeof(tip),
                      l10n::T("Hold the key to talk. Change the key via %s\n"
                              "in multivoid.ini (single letter or a virtual-key number)."),
                      "voice.ptt_key");
            ImGui::SetTooltip("%s", tip);
        }
        if (ImGui::RadioButton(l10n::Label(l10n::T("Voice activation"), "voice_activation"), &mode, 1))
            coop::config::SetValue(::coop::config_registry::rows::voice_mode, "activation");
        // The three sliders: the drag previews live through the voice_chat setters, and the
        // release commits the row (CommitSlider), whose subscriber is the apply. The pending atomic
        // holds the handle's value from every active frame and from the frame a typed value is
        // applied (SliderFloat returns true then, the item already inactive), never the frame's
        // snapshot float. The ranges are the panel's own, narrower than the rows'; AlwaysClamp
        // holds a typed value (Ctrl+click) to them.
        // MTA keeps the in-memory setting as the live value, saved on OK and reverted on Cancel
        // (CSettings.cpp:5568-5575, :3888-3889, :3904-3916).
        // Source's cvarslider has no live preview and writes the cvar at drag end or on the
        // dialog's OK, discarding on Close (cvarslider.cpp:310-333, tf_controls.cpp:746-755).
        // Ours previews live because SetValue writes the ini, logs `config: SET` and notifies on
        // every set (config_runtime.cpp), so the row cannot be MTA's per-frame in-memory store;
        // the row is set once, on release, and a drag the panel's closing abandons is committed
        // rather than reverted because this panel has no OK/Cancel and every other control
        // applies at once.
        if (mode == 1) {
            float thr = s.thresholdDb;
            if (ImGui::SliderFloat(l10n::Label(l10n::T("Threshold"), "threshold"), &thr, -100.0f, 0.0f,
                                   "%.0f dB", ImGuiSliderFlags_AlwaysClamp)) {
                VC::SetThresholdDb(thr);
                g_pendingThreshold.store(thr);
            }
            if (ImGui::IsItemActive()) {
                g_pendingThreshold.store(thr);
                g_draggingThreshold.store(true);
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                g_draggingThreshold.exchange(false);
                CommitSlider(::coop::config_registry::rows::voice_threshold_db,
                             g_pendingThreshold.load(), "%.0f");
            } else if (ImGui::IsItemDeactivated()) {
                g_draggingThreshold.exchange(false);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", l10n::T("Speech louder than this transmits. Watch the mic\n"
                                                "meter: set the slider just above your room's noise."));
            // 0 dB is full scale, so nothing would ever transmit: warn rather than let the panel
            // sit there silently never activating.
            if (thr > -5.0f)
                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "%s",
                                   l10n::T("Threshold near 0 dB -- the mic will almost never "
                                           "trigger. Try -50 dB."));
        }

        ImGui::Spacing();
        ImGui::SeparatorText(l10n::Label(l10n::T("Levels"), "levels"));
        float gain = s.gainDb;
        if (ImGui::SliderFloat(l10n::Label(l10n::T("Mic gain"), "mic_gain"), &gain, -40.0f, 24.0f,
                               "%+.0f dB", ImGuiSliderFlags_AlwaysClamp)) {
            VC::SetGainDb(gain);
            g_pendingGain.store(gain);
        }
        if (ImGui::IsItemActive()) {
            g_pendingGain.store(gain);
            g_draggingGain.store(true);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            g_draggingGain.exchange(false);
            CommitSlider(::coop::config_registry::rows::voice_mic_gain_db, g_pendingGain.load(),
                         "%.0f");
        } else if (ImGui::IsItemDeactivated()) {
            g_draggingGain.exchange(false);
        }
        float vol = s.masterVolume;
        if (ImGui::SliderFloat(l10n::Label(l10n::T("Voice volume"), "voice_volume"), &vol, 0.0f, 3.0f,
                               "%.2fx", ImGuiSliderFlags_AlwaysClamp)) {
            VC::SetMasterVolume(vol);
            g_pendingVolume.store(vol);
        }
        if (ImGui::IsItemActive()) {
            g_pendingVolume.store(vol);
            g_draggingVolume.store(true);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            g_draggingVolume.exchange(false);
            CommitSlider(::coop::config_registry::rows::voice_volume, g_pendingVolume.load(), "%.2f");
        } else if (ImGui::IsItemDeactivated()) {
            g_draggingVolume.exchange(false);
        }
        // The range is the host's in a session; read-only here. The two roster facts are read
        // on the render thread each frame the panel draws (the roster refreshes them on a
        // throttle, so the label may lag a session's start or end by a fraction of a second).
        {
            coop::roster::Snapshot rs;
            coop::roster::GetSnapshot(rs);
            // The range as a value, then the sentence that carries it: one msgid per whose
            // range it is, so a translation orders the whole line.
            char rangeBuf[256];
            const char* range = rangeBuf;
            if (s.distanceCm > 0.0f)
                l10n::Fmt(rangeBuf, sizeof(rangeBuf), l10n::T("%.0f cm"), s.distanceCm);
            else
                range = l10n::T("unlimited");
            char line[256];
            if (!rs.inSession)
                l10n::Fmt(line, sizeof(line), l10n::T("Voice range: %s"), range);
            else if (coop::roster::LocalIsHost())
                l10n::Fmt(line, sizeof(line),
                          l10n::T("Voice range: %s (your server's; multivoid.ini until the settings page)"),
                          range);
            else
                l10n::Fmt(line, sizeof(line), l10n::T("Voice range: %s (set by the host)"), range);
            ImGui::TextDisabled("%s", line);
        }

        ImGui::Spacing();
        ImGui::SeparatorText(l10n::Label(l10n::T("Devices"), "devices"));
        DeviceCombo(l10n::T("Microphone"), "microphone", coop::config_registry::rows::voice_mic_device,
                    g_micCurrent, sizeof(g_micCurrent), g_micDevices);
        DeviceCombo(l10n::T("Output"), "output", coop::config_registry::rows::voice_output_device,
                    g_outCurrent, sizeof(g_outCurrent), g_outDevices);
        if (ImGui::SmallButton(l10n::Label(l10n::T("Rescan devices"), "rescan_devices")))
            RefreshDevices();
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", l10n::T("Device changes apply immediately (the voice engine\n"
                                            "reopens on the game's next tick)."));
    }
    ImGui::End();
}

}  // namespace ui::voice_panel
