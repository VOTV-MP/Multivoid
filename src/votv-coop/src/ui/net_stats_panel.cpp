// ui/net_stats_panel.cpp -- see ui/net_stats_panel.h.

#include "ui/net_stats_panel.h"

#include "coop/net/net_stats.h"
#include "harness/config.h"
#include "ui/scale.h"

#include "imgui.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace ui::net_stats_panel {
namespace {

using ui::scale::S;
namespace NS = coop::net::net_stats;

std::atomic<bool> g_enabled{false};
std::once_flag g_prefOnce;

void LoadPrefOnce() {
    std::call_once(g_prefOnce, [] {
        g_enabled.store(harness::config::ReadIniValue("ui.netstats", "0") == "1",
                        std::memory_order_relaxed);
    });
}

// ---- formatting (auto units) --------------------------------------------------

// "999 B/s" -> "123.4 KB/s" -> "12.34 MB/s". Wire rates never reach GB/s here.
void FmtRate(double bps, char* out, size_t n) {
    if (bps < 1000.0)            std::snprintf(out, n, "%.0f B/s", bps);
    else if (bps < 1000.0 * 1024) std::snprintf(out, n, "%.1f KB/s", bps / 1024.0);
    else                          std::snprintf(out, n, "%.2f MB/s", bps / (1024.0 * 1024.0));
}

// "999 B" -> "123.4 KB" -> "123.4 MB" -> "1.23 GB".
void FmtBytes(uint64_t b, char* out, size_t n) {
    const double d = static_cast<double>(b);
    if (b < 1000)                          std::snprintf(out, n, "%llu B", static_cast<unsigned long long>(b));
    else if (d < 1000.0 * 1024)            std::snprintf(out, n, "%.1f KB", d / 1024.0);
    else if (d < 1000.0 * 1024 * 1024)     std::snprintf(out, n, "%.1f MB", d / (1024.0 * 1024.0));
    else                                   std::snprintf(out, n, "%.2f GB", d / (1024.0 * 1024.0 * 1024.0));
}

// ---- the 60 s rate ring (render thread only; sampled ~1 Hz off the snapshot) ---

constexpr int kRing = 60;
float  g_rxRing[kRing] = {};
float  g_txRing[kRing] = {};
int    g_ringHead = 0;      // next write slot; ring[head-1] = newest
double g_lastSampleAt = 0.0;

void SampleRing(const NS::Snapshot& s) {
    const double now = ImGui::GetTime();
    if (now - g_lastSampleAt < 1.0) return;
    g_lastSampleAt = now;
    g_rxRing[g_ringHead] = s.connected ? s.inBps : 0.f;
    g_txRing[g_ringHead] = s.connected ? s.outBps : 0.f;
    g_ringHead = (g_ringHead + 1) % kRing;
}

// Palette: rx = the roster sky, tx = the host amber (the slot-color family the
// chat/scoreboard already use), so "down is blue, up is orange" reads instantly.
constexpr ImU32 kRxCol   = IM_COL32(110, 205, 255, 255);
constexpr ImU32 kTxCol   = IM_COL32(255, 179, 64, 255);
constexpr ImU32 kDim     = IM_COL32(160, 165, 175, 255);
constexpr ImU32 kBright  = IM_COL32(236, 238, 242, 255);

ImU32 WithAlpha(ImU32 col, float a) {
    return (col & 0x00FFFFFFu) | (static_cast<ImU32>(std::clamp(a, 0.f, 1.f) * 255.f) << 24);
}

// Small solid direction triangle beside each rate row (down = receive, up = send).
// Drawn, not a glyph: every embedded font family renders it identically.
void DirTriangle(ImDrawList* dl, ImVec2 center, float half, ImU32 col, bool down) {
    if (down)
        dl->AddTriangleFilled(ImVec2(center.x - half, center.y - half * 0.8f),
                              ImVec2(center.x + half, center.y - half * 0.8f),
                              ImVec2(center.x, center.y + half), col);
    else
        dl->AddTriangleFilled(ImVec2(center.x - half, center.y + half * 0.8f),
                              ImVec2(center.x + half, center.y + half * 0.8f),
                              ImVec2(center.x, center.y - half), col);
}

// One rate row: triangle, the live rate, and the session total right-aligned.
void RateRow(ImDrawList* dl, float rowW, ImU32 col, bool down, double bps, uint64_t total) {
    char rate[32], tot[32];
    FmtRate(bps, rate, sizeof(rate));
    FmtBytes(total, tot, sizeof(tot));

    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float rowH = ImGui::GetTextLineHeight();
    DirTriangle(dl, ImVec2(pos.x + S(5.f), pos.y + rowH * 0.52f), S(4.5f), col, down);

    ImGui::SetCursorScreenPos(ImVec2(pos.x + S(16.f), pos.y));
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::TextUnformatted(rate);
    ImGui::PopStyleColor();

    ImFont* font = ImGui::GetFont();
    const float totW = font->CalcTextSizeA(ImGui::GetFontSize(), 3.4e38f, 0.f, tot).x;
    dl->AddText(ImVec2(pos.x + rowW - totW, pos.y), kDim, tot);
}

// The 60 s sparkline: rx as a filled area, tx as a line on top. Shared max
// normalization (honest relative magnitudes) with a 2 KB/s floor so an idle
// link doesn't render noise as mountains.
void Sparkline(ImDrawList* dl, float w, float h) {
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(w, h));
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), IM_COL32(255, 255, 255, 10), S(3.f));

    float maxV = 2.f * 1024.f;
    for (int i = 0; i < kRing; ++i) maxV = std::max({maxV, g_rxRing[i], g_txRing[i]});

    const float dx = w / (kRing - 1);
    auto yOf = [&](float v) { return p0.y + h - (std::min(v, maxV) / maxV) * (h - S(2.f)); };
    auto at  = [&](const float* ring, int i) { return ring[(g_ringHead + i) % kRing]; };

    for (int i = 0; i + 1 < kRing; ++i) {   // rx: filled area + edge line
        const float x0 = p0.x + dx * i, x1 = p0.x + dx * (i + 1);
        const float y0 = yOf(at(g_rxRing, i)), y1 = yOf(at(g_rxRing, i + 1));
        dl->AddQuadFilled(ImVec2(x0, y0), ImVec2(x1, y1),
                          ImVec2(x1, p0.y + h), ImVec2(x0, p0.y + h), WithAlpha(kRxCol, 0.18f));
        dl->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), WithAlpha(kRxCol, 0.9f), S(1.2f));
    }
    for (int i = 0; i + 1 < kRing; ++i)     // tx: line only, on top
        dl->AddLine(ImVec2(p0.x + dx * i, yOf(at(g_txRing, i))),
                    ImVec2(p0.x + dx * (i + 1), yOf(at(g_txRing, i + 1))),
                    WithAlpha(kTxCol, 0.9f), S(1.2f));

    char peak[32];
    FmtRate(maxV, peak, sizeof(peak));
    dl->AddText(ImVec2(p0.x + S(4.f), p0.y + S(1.f)), WithAlpha(kDim, 0.75f), peak);
}

}  // namespace

bool Enabled() {
    LoadPrefOnce();
    return g_enabled.load(std::memory_order_relaxed);
}

void SetEnabled(bool on) {
    LoadPrefOnce();
    g_enabled.store(on, std::memory_order_relaxed);
    harness::config::WriteIniValue("ui.netstats", on ? "1" : "0");
}

void Render() {
    if (!Enabled()) return;
    NS::Snapshot s;
    NS::Get(s);
    SampleRing(s);

    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - S(14.f), S(14.f)),
                            ImGuiCond_Always, ImVec2(1.f, 0.f));
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, S(8.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(10.f), S(8.f)));
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(255, 255, 255, 18));
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize;
    if (ImGui::Begin("##coop_netstats", nullptr, flags)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float rowW = S(196.f);

        // Header: status dot + role/peers/ping.
        {
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            const float rowH = ImGui::GetTextLineHeight();
            dl->AddCircleFilled(ImVec2(pos.x + S(5.f), pos.y + rowH * 0.52f), S(3.5f),
                                s.connected ? IM_COL32(120, 230, 130, 255)
                                            : IM_COL32(120, 125, 135, 255));
            ImGui::SetCursorScreenPos(ImVec2(pos.x + S(16.f), pos.y));
            char hdr[64];
            if (!s.connected)
                std::snprintf(hdr, sizeof(hdr), "NET  offline");
            else if (s.pingMaxMs >= 0)
                std::snprintf(hdr, sizeof(hdr), "NET  %d peer%s  %d ms",
                              s.peers, s.peers == 1 ? "" : "s", s.pingMaxMs);
            else
                std::snprintf(hdr, sizeof(hdr), "NET  %d peer%s",
                              s.peers, s.peers == 1 ? "" : "s");
            ImGui::PushStyleColor(ImGuiCol_Text, kBright);
            ImGui::TextUnformatted(hdr);
            ImGui::PopStyleColor();
            // Establish the fixed content width (the totals right-align against it).
            ImGui::SameLine(rowW);
            ImGui::Dummy(ImVec2(1.f, 1.f));
        }

        RateRow(dl, rowW, kRxCol, /*down*/true, s.inBps, s.bytesRecv);
        RateRow(dl, rowW, kTxCol, /*down*/false, s.outBps, s.bytesSent);
        ImGui::Spacing();
        Sparkline(dl, rowW, S(30.f));

        char pk[64];
        std::snprintf(pk, sizeof(pk), "pkt/s  %.0f in   %.0f out", s.inPktps, s.outPktps);
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted(pk);
        ImGui::PopStyleColor();
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

void RenderMenuPref() {
    bool on = Enabled();
    if (ImGui::Checkbox("Show network stats overlay", &on)) SetEnabled(on);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("A compact live panel in the top-right corner: receive/send rate right\n"
                          "now (wire-level, includes protocol overhead), total downloaded/uploaded\n"
                          "this session, packets/s, peers + ping, and a 60 s rate graph.\n"
                          "Works for host and clients. Saved to votv-coop.ini (ui.netstats).");
    ImGui::TextDisabled("Off by default; persists across sessions.");

    // Live readout -- doubles as a preview while the overlay itself is off.
    NS::Snapshot s;
    NS::Get(s);
    char rateIn[32], rateOut[32], totIn[32], totOut[32];
    FmtRate(s.inBps, rateIn, sizeof(rateIn));
    FmtRate(s.outBps, rateOut, sizeof(rateOut));
    FmtBytes(s.bytesRecv, totIn, sizeof(totIn));
    FmtBytes(s.bytesSent, totOut, sizeof(totOut));
    ImGui::Spacing();
    ImGui::SeparatorText("Session");
    if (!s.connected) ImGui::TextDisabled("No active session.");
    ImGui::Text("Down:  %s now,  %s total,  %llu packets",
                rateIn, totIn, static_cast<unsigned long long>(s.packetsRecv));
    ImGui::Text("Up:    %s now,  %s total,  %llu packets",
                rateOut, totOut, static_cast<unsigned long long>(s.packetsSent));
    if (s.connected) {
        if (s.pingMaxMs >= 0) ImGui::Text("Peers: %d,  worst ping %d ms", s.peers, s.pingMaxMs);
        else                  ImGui::Text("Peers: %d", s.peers);
    }
}

}  // namespace ui::net_stats_panel
