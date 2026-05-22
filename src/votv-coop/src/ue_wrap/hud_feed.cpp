#include "ue_wrap/hud_feed.h"

#include "ue_wrap/engine.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"

#include <deque>

namespace ue_wrap::hud_feed {

namespace {

constexpr int kMaxLines = 8;     // last N lines kept on screen
constexpr int kZOrder = 100;     // above VOTV's own UMG HUD

void* g_root = nullptr;          // the UUserWidget (for re-attach on level change)
void* g_text = nullptr;          // the UTextBlock we drive
std::deque<std::wstring> g_lines;

// Rebuild the multi-line string (oldest at top, newest at bottom) and push it to the
// single UTextBlock. The block wraps newlines, so one SetText paints the whole feed.
void Repaint() {
    if (!g_text) return;
    std::wstring all;
    for (size_t i = 0; i < g_lines.size(); ++i) {
        if (i) all += L'\n';
        all += g_lines[i];
    }
    engine::SetWidgetText(g_text, all.c_str());
}

}  // namespace

bool Init(void* outer) {
    if (g_root) return true;  // already up
    if (!outer) { UE_LOGW("hud_feed: Init with null outer"); return false; }
    if (!engine::SpawnHudFeedWidget(outer, kZOrder, &g_root, &g_text) || !g_root || !g_text) {
        UE_LOGE("hud_feed: SpawnHudFeedWidget failed");
        g_root = g_text = nullptr;
        return false;
    }
    Repaint();  // paint whatever lines queued before Init (if any)
    UE_LOGI("hud_feed: initialized (root=%p text=%p)", g_root, g_text);
    return true;
}

bool IsInitialized() {
    // Self-heal: if the widget object was destroyed (e.g. GC'd on a level change),
    // drop the stale pointers so the caller's lazy Init re-creates it (the queued
    // lines are kept and re-painted on the next Init). Never call into a dead UObject.
    if (g_root && !reflection::IsLive(g_root)) {
        UE_LOGI("hud_feed: widget no longer live -- will re-init");
        g_root = g_text = nullptr;
    }
    return g_root != nullptr;
}

void Push(const std::wstring& line) {
    g_lines.push_back(line);
    while (g_lines.size() > static_cast<size_t>(kMaxLines)) g_lines.pop_front();
    UE_LOGI("hud_feed: '%ls'", line.c_str());
    Repaint();
}

void Shutdown() {
    if (g_root) engine::RemoveWidgetFromViewport(g_root);
    g_root = g_text = nullptr;
    g_lines.clear();
}

}  // namespace ue_wrap::hud_feed
