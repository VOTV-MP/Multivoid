// coop/dev/media_url_probe.cpp -- see coop/dev/media_url_probe.h.

#include "coop/dev/media_url_probe.h"

#include "coop/config/config.h"
#include "coop/net/net_clock.h"
#include "coop/player/players_registry.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/world/world_singleton.h"

#include <cstdint>
#include <string>
#include <vector>

namespace coop::dev::media_url_probe {
namespace {

namespace R = ue_wrap::reflection;

constexpr uint64_t kSettleMs = 20000;
constexpr uint64_t kWatchMs = 25000;
constexpr int32_t  kChannel = 15;

// The addresses, `|` between them, each watched in turn.
const std::vector<std::wstring>& Urls() {
    static const std::vector<std::wstring> all = [] {
        std::vector<std::wstring> out;
        const std::string v = coop::config::ResolveString(::coop::config_registry::rows::media_url_probe);
        size_t at = 0;
        while (at <= v.size() && !v.empty()) {
            const size_t bar = v.find('|', at);
            const std::string one = v.substr(at, bar == std::string::npos ? std::string::npos : bar - at);
            if (!one.empty()) out.emplace_back(one.begin(), one.end());
            if (bar == std::string::npos) break;
            at = bar + 1;
        }
        return out;
    }();
    return all;
}

ue_wrap::CachedObjRef g_player;
uint64_t g_worldMs = 0;
uint64_t g_openedMs = 0;
uint64_t g_nextReadMs = 0;
int64_t  g_firstTicks = -1;
int64_t  g_lastTicks = -1;
size_t   g_index = 0;
bool     g_asked = false;

void* Fn(void* obj, const wchar_t* name) { return R::FindDispatchFunctionCached(R::ClassOf(obj), name); }

// A bool getter: 1, 0, or -1 when the function is not on the class.
int CallBool(void* obj, const wchar_t* name) {
    void* fn = Fn(obj, name);
    if (!fn) return -1;
    ue_wrap::ParamFrame f(fn);
    if (!f.valid() || !ue_wrap::Call(obj, f)) return -1;
    return f.Get<bool>(L"ReturnValue") ? 1 : 0;
}

// An FTimespan getter's ticks (100 ns each), or -1.
int64_t CallTicks(void* obj, const wchar_t* name) {
    void* fn = Fn(obj, name);
    if (!fn) return -1;
    ue_wrap::ParamFrame f(fn);
    if (!f.valid() || !ue_wrap::Call(obj, f)) return -1;
    return f.Get<int64_t>(L"ReturnValue");
}

void* TakeChannel() {
    void* gm = ue_wrap::world_singleton::Find(L"mainGamemode_C");
    if (!gm) return nullptr;
    void* fn = Fn(gm, L"getMediaChannel");
    if (!fn) return nullptr;
    ue_wrap::ParamFrame f(fn);
    if (!f.valid() || !f.Set<int32_t>(L"Index", kChannel) || !ue_wrap::Call(gm, f)) return nullptr;
    return f.Get<void*>(L"media");
}

// A function of the player that takes the address and answers a bool (OpenUrl, CanPlayUrl).
int CallWithUrl(void* player, const wchar_t* name, const std::wstring& url) {
    void* fn = Fn(player, name);
    if (!fn) return -1;
    ue_wrap::ParamFrame f(fn);
    // The address is read during the call and the engine keeps its own copy, so the header points
    // at this frame's buffer.
    struct Header {
        const wchar_t* data;
        int32_t num;
        int32_t max;
    } h{url.c_str(), static_cast<int32_t>(url.size() + 1), static_cast<int32_t>(url.size() + 1)};
    if (!f.valid() || !f.SetRaw(L"Url", &h, sizeof(h)) || !ue_wrap::Call(player, f)) return -1;
    return f.Get<bool>(L"ReturnValue") ? 1 : 0;
}

void Close(void* player) {
    if (void* fn = Fn(player, L"Close")) {
        ue_wrap::ParamFrame f(fn);
        if (f.valid()) ue_wrap::Call(player, f);
    }
}

}  // namespace

void Tick() {
    if (g_index >= Urls().size()) return;
    const uint64_t now = coop::net::NowMs();
    if (!coop::players::Registry::Get().Local()) return;
    if (!g_worldMs) g_worldMs = now;
    if (now - g_worldMs < kSettleMs) return;
    const std::wstring& url = Urls()[g_index];
    if (!g_asked) {
        void* player = TakeChannel();
        if (!player) return;  // the gamemode is not up yet
        g_asked = true;
        g_player.Set(player);
        g_firstTicks = g_lastTicks = -1;
        const int can = CallWithUrl(player, L"CanPlayUrl", url);
        const int ok = CallWithUrl(player, L"OpenUrl", url);
        g_openedMs = now;
        g_nextReadMs = now + 1000;
        UE_LOGI("[MEDIA-URL-PROBE] #%zu '%ls': CanPlayUrl=%d OpenUrl=%d on channel %d", g_index, url.c_str(), can, ok,
                kChannel);
        return;
    }
    if (now < g_nextReadMs) return;
    g_nextReadMs = now + 1000;
    void* player = g_player.Get();
    if (!player) {
        UE_LOGW("[MEDIA-URL-PROBE] #%zu NOT-PLAYED the player object ended", g_index);
        g_index = Urls().size();
        return;
    }
    const int ready = CallBool(player, L"IsReady");
    int playing = CallBool(player, L"IsPlaying");
    if (ready == 1 && playing == 0) playing = CallBool(player, L"Play");
    const int64_t ticks = CallTicks(player, L"GetTime");
    const int64_t duration = CallTicks(player, L"GetDuration");
    if (playing == 1 && ticks > 0 && g_firstTicks < 0) g_firstTicks = ticks;
    g_lastTicks = ticks;
    UE_LOGI("[MEDIA-URL-PROBE] #%zu t+%llus closed=%d connecting=%d preparing=%d buffering=%d ready=%d playing=%d "
            "error=%d time=%.1fs duration=%.1fs",
            g_index, static_cast<unsigned long long>((now - g_openedMs) / 1000), CallBool(player, L"IsClosed"),
            CallBool(player, L"IsConnecting"), CallBool(player, L"IsPreparing"), CallBool(player, L"IsBuffering"),
            ready, playing, CallBool(player, L"HasError"), static_cast<double>(ticks) / 1e7,
            static_cast<double>(duration) / 1e7);
    if (now - g_openedMs < kWatchMs) return;
    const bool played = g_firstTicks >= 0 && g_lastTicks > g_firstTicks + 20000000;  // two seconds on
    if (played)
        UE_LOGI("[MEDIA-URL-PROBE] #%zu PLAYED the time moved from %.1fs to %.1fs", g_index,
                static_cast<double>(g_firstTicks) / 1e7, static_cast<double>(g_lastTicks) / 1e7);
    else
        UE_LOGW("[MEDIA-URL-PROBE] #%zu NOT-PLAYED the time did not move", g_index);
    Close(player);
    ++g_index;
    g_asked = false;
}

}  // namespace coop::dev::media_url_probe
