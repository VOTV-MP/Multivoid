// coop/dev/end_play_probe.cpp -- [dev] the end-of-play seam against the K2_DestroyActor seam. See the
// header.
#include "coop/dev/end_play_probe.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/engine/actor_end_play.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace coop::dev::end_play_probe {
namespace {

namespace R = ue_wrap::reflection;
namespace P = ue_wrap::profile;
namespace EP = ue_wrap::actor_end_play;

// A destroyed end of play waiting for the K2_DestroyActor post-hook of the call it ran inside, which
// fires on the same stack right after it; one still unmatched at a later pump tick had none.
struct Pending {
    void* actor;
    std::wstring cls;
    bool matched;
    uint64_t tick;
};
std::vector<Pending> g_pending;
constexpr size_t kMaxPending = 4096;

uint64_t g_tick = 0;
unsigned long long g_byReason[5] = {};
unsigned long long g_k2Matched = 0, g_k2NoEndPlay = 0, g_native = 0, g_overflow = 0;
std::unordered_map<std::wstring, unsigned> g_nativeByClass, g_k2NoEndPlayByClass;
int g_nativeLines = 0, g_k2Lines = 0;
constexpr int kMaxLines = 40;

void OnEndPlay(void* actor, EP::Reason reason) {
    const auto r = static_cast<unsigned>(reason);
    if (r < 5) ++g_byReason[r];
    if (reason != EP::Reason::Destroyed) return;
    if (g_pending.size() >= kMaxPending) { ++g_overflow; return; }
    g_pending.push_back({actor, R::ClassNameOf(actor), false, g_tick});
}

void OnK2DestroyPost(void* actor, void* /*sourceObject*/, void* /*result*/) {
    for (auto it = g_pending.rbegin(); it != g_pending.rend(); ++it)
        if (it->actor == actor && !it->matched) { it->matched = true; ++g_k2Matched; return; }
    ++g_k2NoEndPlay;
    const std::wstring cls = actor ? R::ClassNameOf(actor) : std::wstring(L"<null>");
    ++g_k2NoEndPlayByClass[cls];
    if (g_k2Lines++ < kMaxLines)
        UE_LOGI("end_play_probe: K2_DestroyActor with no end of play: class=%ls", cls.c_str());
}

void Classify() {
    size_t keep = 0;
    for (size_t i = 0; i < g_pending.size(); ++i) {
        Pending& p = g_pending[i];
        if (p.tick >= g_tick) { g_pending[keep++] = std::move(p); continue; }
        if (p.matched) continue;
        ++g_native;
        ++g_nativeByClass[p.cls];
        if (g_nativeLines++ < kMaxLines)
            UE_LOGI("end_play_probe: an end of play with no K2_DestroyActor: class=%ls", p.cls.c_str());
    }
    g_pending.resize(keep);
}

std::string Top(const std::unordered_map<std::wstring, unsigned>& m) {
    std::vector<std::pair<std::wstring, unsigned>> v(m.begin(), m.end());
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    std::string out;
    for (size_t i = 0; i < v.size() && i < 12; ++i) {
        if (!out.empty()) out += ", ";
        for (wchar_t c : v[i].first) out.push_back((c > 0 && c < 128) ? static_cast<char>(c) : '?');
        out += " x" + std::to_string(v[i].second);
    }
    return out.empty() ? "none" : out;
}

void Summary() {
    const EP::Stats s = EP::GetStats();
    UE_LOGI("end_play_probe: SUMMARY seen=%llu offGameThread=%llu | destroyed=%llu transition=%llu "
            "removedFromWorld=%llu quit=%llu | inside a K2_DestroyActor=%llu, with none=%llu, K2_DestroyActor "
            "with no end of play=%llu, overflow=%llu",
            s.seen, s.offGameThread, g_byReason[0], g_byReason[1], g_byReason[3], g_byReason[4], g_k2Matched,
            g_native, g_k2NoEndPlay, g_overflow);
    UE_LOGI("end_play_probe: with no K2_DestroyActor, by class: %s", Top(g_nativeByClass).c_str());
    UE_LOGI("end_play_probe: K2_DestroyActor with no end of play, by class: %s", Top(g_k2NoEndPlayByClass).c_str());
}

bool Arm() {
    if (!EP::IsInstalled()) {
        UE_LOGW("end_play_probe: the end-of-play seam is not installed -- nothing to measure");
        return false;
    }
    void* actorCls = R::FindClass(P::name::ActorClass);
    void* k2 = actorCls ? R::FindFunction(actorCls, L"K2_DestroyActor") : nullptr;
    if (!k2 || !ue_wrap::ufunction_hook::InstallPostHook(k2, &OnK2DestroyPost)) {
        UE_LOGW("end_play_probe: the K2_DestroyActor post-hook did not install (fn=%p) -- nothing to compare", k2);
        return false;
    }
    if (!EP::AddSink(&OnEndPlay)) return false;
    UE_LOGI("end_play_probe: armed -- every end of play by reason, each destroy matched to its K2_DestroyActor");
    return true;
}

}  // namespace

void Tick() {
    static const bool s_on = coop::config::ResolveFlag(::coop::config_registry::rows::end_play_probe);
    if (!s_on) return;
    static int s_state = 0;  // 0 = not armed yet, 1 = armed, -1 = could not arm
    if (s_state == 0) s_state = Arm() ? 1 : -1;
    if (s_state < 0) return;
    ++g_tick;
    Classify();
    static auto s_next = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    const auto now = std::chrono::steady_clock::now();
    if (now >= s_next) {
        s_next = now + std::chrono::seconds(30);
        Summary();
    }
}

}  // namespace coop::dev::end_play_probe
