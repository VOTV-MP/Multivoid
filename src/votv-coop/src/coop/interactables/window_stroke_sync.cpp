// coop/interactables/window_stroke_sync.cpp -- see coop/interactables/window_stroke_sync.h.

#include "coop/interactables/window_stroke_sync.h"

#include "coop/config/config.h"
#include "coop/element/object_scan_hub.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/devices/window_canvas.h"
#include "ue_wrap/engine/world_identity.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

namespace coop::window_stroke_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace WC = ue_wrap::window_canvas;
using Clock = std::chrono::steady_clock;

// A sponge dabs every 0.25 s, so these bounds are minutes of wiping, never reached in play.
constexpr size_t kMaxQueued = 256;
constexpr auto kInboundTtl = std::chrono::seconds(20);

bool Verbose() {
    static const bool s = ::coop::config::ResolveFlag(::coop::config_registry::rows::window_stroke_log);
    return s;
}

std::atomic<coop::net::Session*> g_session{nullptr};
std::atomic<bool> g_armed{false};  // connected: the observer ignores every draw otherwise
int  g_wireApplyDepth = 0;         // GT: our own replay's draw is not a local stroke
bool g_hookInstalled = false;

// Outbound: stroke corners taken inside the Func hook, sent from Tick (both game thread).
std::vector<std::pair<float, float>> g_outbound;

// Inbound: dabs waiting for the window to be present and the canvas to be free.
struct Pending {
    WC::Dab dab;
    Clock::time_point deadline;
};
std::deque<Pending> g_inbound;

uint64_t g_sent = 0, g_applied = 0, g_unsent = 0, g_expired = 0;

// ---- the window singleton, found by the shared scan ----
void*    g_window = nullptr;
int32_t  g_windowIdx = -1;
uint32_t g_windowGen = 0;
void*    g_scanFound = nullptr;
int32_t  g_scanIdx = -1;

void HubPassBegin(void*, bool) { g_scanFound = nullptr; g_scanIdx = -1; }

void HubMatch(void*, void* obj) {
    if (R::NameStartsWith(R::NameOf(obj), L"Default__") || !R::IsLive(obj)) return;
    g_scanFound = obj;
    g_scanIdx = R::InternalIndexOf(obj);
}

size_t HubPassComplete(void*, bool isFull, uint32_t worldGen) {
    const void* before = g_window;
    if (g_scanFound) {
        g_window = g_scanFound;
        g_windowIdx = g_scanIdx;
    } else if (isFull || !R::IsLiveByIndex(g_window, g_windowIdx)) {
        g_window = nullptr;
        g_windowIdx = -1;
    }
    g_windowGen = worldGen;
    if (g_window != before) UE_LOGI("window_stroke: window %p (world gen %u)", g_window, worldGen);
    return g_window ? 1 : 0;
}

void RegisterWithScanHub() {
    static bool sDone = false;
    if (sDone) return;
    sDone = true;
    coop::element::scan_hub::Register(coop::element::scan_hub::Consumer{
        "window_stroke", nullptr, &WC::EnsureResolved, &WC::IsWindow,
        &HubPassBegin, &HubMatch, &HubPassComplete, /*settleScans*/ 15});
}

void* ResolveWindow() {
    if (g_windowGen != ue_wrap::world_identity::Generation()) return nullptr;
    return R::IsLiveByIndex(g_window, g_windowIdx) ? g_window : nullptr;
}

// Func post-hook on UCanvas::K2_DrawMaterial: game thread, deep inside the engine call, so field
// reads and a vector push only.
void OnDrawPost(void* /*canvas*/, void* sourceObject, void* /*result*/) {
    if (g_wireApplyDepth > 0 || !g_armed.load(std::memory_order_relaxed)) return;
    if (!WC::IsStrokeDraw(sourceObject)) return;
    float x = 0.f, y = 0.f;
    if (!WC::ReadStrokeCorner(sourceObject, x, y) || g_outbound.size() >= kMaxQueued) return;
    g_outbound.emplace_back(x, y);
}

void SendOutbound(coop::net::Session* s) {
    if (g_outbound.empty()) return;
    float size = 0.f, opac = 0.f, col = 0.f;
    if (!WC::ReadHeldBrush(coop::players::Registry::Get().Local(), size, opac, col)) {
        g_unsent += g_outbound.size();  // not the local hand's sponge: a thrown sponge hit the window
        if (Verbose()) UE_LOGI("window_stroke: %zu dab(s) without a held sponge -- not sent", g_outbound.size());
        g_outbound.clear();
        return;
    }
    for (const auto& corner : g_outbound) {
        coop::net::WindowStrokePayload p{};
        p.x = corner.first;
        p.y = corner.second;
        p.size = size;
        p.opac = opac;
        p.col = col;
        if (!s->SendReliable(coop::net::ReliableKind::WindowStroke, &p, sizeof(p))) continue;
        ++g_sent;
        if (Verbose() || g_sent <= 3 || g_sent % 200 == 0)
            UE_LOGI("window_stroke: sent #%llu at (%.1f,%.1f) size=%.0f opac=%.2f col=%.2f",
                    static_cast<unsigned long long>(g_sent), p.x, p.y, p.size, p.opac, p.col);
    }
    g_outbound.clear();
}

void ApplyInbound() {
    if (g_inbound.empty()) return;
    const auto now = Clock::now();
    void* win = ResolveWindow();
    if (!win || WC::IsSignalPlaying()) {
        while (!g_inbound.empty() && now >= g_inbound.front().deadline) {
            g_inbound.pop_front();
            ++g_expired;
        }
        return;
    }
    while (!g_inbound.empty()) {
        const WC::Dab dab = g_inbound.front().dab;
        g_inbound.pop_front();
        ++g_wireApplyDepth;
        const bool ok = WC::DrawDab(win, dab);
        --g_wireApplyDepth;
        if (!ok) {
            UE_LOGW("window_stroke: replay failed at (%.1f,%.1f)", dab.x, dab.y);
            continue;
        }
        ++g_applied;
        if (Verbose() || g_applied <= 3 || g_applied % 200 == 0)
            UE_LOGI("window_stroke: applied #%llu at (%.1f,%.1f) size=%.0f opac=%.2f",
                    static_cast<unsigned long long>(g_applied), dab.x, dab.y, dab.size, dab.opac);
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    RegisterWithScanHub();
}

void OnReliable(const coop::net::WindowStrokePayload& payload, uint8_t senderPeerSlot) {
    WC::Dab dab;
    dab.x = payload.x;
    dab.y = payload.y;
    dab.size = payload.size;
    dab.opac = payload.opac;
    dab.col = payload.col;
    if (g_inbound.size() >= kMaxQueued) {
        g_inbound.pop_front();
        ++g_expired;
    }
    g_inbound.push_back(Pending{dab, Clock::now() + kInboundTtl});
    if (Verbose())
        UE_LOGI("window_stroke: received (%.1f,%.1f) from slot %u", dab.x, dab.y,
                static_cast<unsigned>(senderPeerSlot));
}

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    const bool connected = s && s->connected();
    g_armed.store(connected, std::memory_order_relaxed);
    if (!WC::EnsureResolved()) return;
    RegisterWithScanHub();
    if (!g_hookInstalled) {
        g_hookInstalled = true;  // InstallPostHook is idempotent and process-lifetime
        const bool ok = ue_wrap::ufunction_hook::InstallPostHook(WC::DrawMaterialFn(), &OnDrawPost);
        UE_LOGI("window_stroke: K2_DrawMaterial post-hook %s", ok ? "installed" : "FAILED");
    }
    if (!connected) {
        g_outbound.clear();
        return;
    }
    SendOutbound(s);
    ApplyInbound();
}

void OnDisconnect() {
    if (g_sent || g_applied || g_unsent || g_expired)
        UE_LOGI("window_stroke: session end -- sent=%llu applied=%llu unsent=%llu expired=%llu",
                static_cast<unsigned long long>(g_sent), static_cast<unsigned long long>(g_applied),
                static_cast<unsigned long long>(g_unsent), static_cast<unsigned long long>(g_expired));
    g_armed.store(false, std::memory_order_relaxed);
    g_outbound.clear();
    g_inbound.clear();
    g_sent = g_applied = g_unsent = g_expired = 0;
    WC::ReleaseBrush();
}

}  // namespace coop::window_stroke_sync
