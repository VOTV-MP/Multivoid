// coop/dev/tv_link_drill.cpp -- see coop/dev/tv_link_drill.h.

#include "coop/dev/tv_link_drill.h"

#include "coop/config/config.h"
#include "coop/interactables/tv_link_sync.h"
#include "coop/net/net_clock.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/roster.h"
#include "coop/props/prop_snapshot.h"
#include "coop/save/join_window_baseline.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/ftext_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/devices/tv.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/world/game_rules.h"
#include "ue_wrap/world/world_singleton.h"

#include <cstdint>
#include <string>
#include <vector>

namespace coop::dev::tv_link_drill {
namespace {

namespace TV = ue_wrap::tv;

constexpr uint64_t kSettleMs = 8000;
constexpr uint64_t kWatchMs = 40000;
constexpr uint64_t kStepMs = 2000;
constexpr uint64_t kGiveUpMs = 150000;

struct Plan {
    std::wstring url;
    bool typed = false;  // `typed:<address>`: entered in the remote's search box, not handed to the lane
    bool fromClient = false;  // `client:<address>`: the client asks its copy of the set, the host watches
    bool viaSet = false;      // `set:<address>`: the set's own openLink is called, as a row of its remote's list does
    bool control = false;     // `control:<address>`: the host also pauses at 4 s, then seeks to 1 s and resumes at 10 s
};

const Plan& ThePlan() {
    static const Plan p = [] {
        Plan out;
        std::string v = coop::config::ResolveString(::coop::config_registry::rows::tv_link_drill);
        if (v.compare(0, 6, "typed:") == 0) {
            out.typed = true;
            v.erase(0, 6);
        } else if (v.compare(0, 8, "control:") == 0) {
            out.control = true;
            v.erase(0, 8);
        } else if (v.compare(0, 4, "set:") == 0) {
            out.viaSet = true;
            v.erase(0, 4);
        } else if (v.compare(0, 7, "client:") == 0) {
            out.fromClient = true;
            v.erase(0, 7);
        }
        out.url.assign(v.begin(), v.end());
        return out;
    }();
    return p;
}

const std::wstring& Url() { return ThePlan().url; }

// Enter the address in the search box of the set's remote, as a player's Enter does: the remote is told
// which set it serves and its committed event is called with the text.
bool TypeOnRemote(void* tv, const std::wstring& url) {
    namespace R = ue_wrap::reflection;
    void* remote = ue_wrap::world_singleton::Find(L"ui_tvInterface_C");
    if (!remote) return false;
    const int32_t radioOff = R::FindPropertyOffset(R::ClassOf(remote), L"radio");
    void* fn = R::FindDispatchFunctionCached(
        R::ClassOf(remote),
        L"BndEvt__ui_tvInterface_textbox_search_K2Node_ComponentBoundEvent_13_OnEditableTextBoxCommittedEvent__DelegateSignature");
    if (radioOff < 0 || !fn) return false;
    *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(remote) + radioOff) = tv;
    ue_wrap::ParamFrame f(fn);
    const int32_t textOff = f.valid() ? f.ParamOffset(L"Text") : -1;
    if (textOff < 0 || !ue_wrap::ftext_utils::MintFText(url.c_str(), static_cast<uint8_t*>(f.data()) + textOff)) return false;
    f.Set<uint8_t>(L"CommitMethod", 1);  // ETextCommit::OnEnter
    return ue_wrap::Call(remote, f);
}

uint64_t g_readyMs = 0;
uint64_t g_nextMs = 0;
uint64_t g_playedMs = 0;     // HOST: when the address was asked for. CLIENT: when a link was first opened here.
uint64_t g_startMs = 0;
double   g_first = -1.0;
double   g_last = -1.0;
double   g_max = -1.0;
size_t   g_candidate = 0;
bool     g_done = false;

const char* Who() { return coop::roster::LocalIsHost() ? "H" : "C"; }

bool ClientInWorld(coop::net::Session& s) {
    for (int slot = 1; slot < static_cast<int>(coop::players::kMaxPeers); ++slot)
        if (s.IsSlotWorldReady(slot) && coop::prop_snapshot::IsBracketClosed(slot) &&
            !coop::join_window_baseline::IsLateWindowOpen(slot))
            return true;
    return false;
}

void* FirstSet() {
    // The drill's save has the Custom Content rule off; the lane needs it on, as a player's save would have it.
    static bool sLogged = false;
    const int rule = ue_wrap::game_rules::ReadCustomContent();
    if (rule == 0 && ue_wrap::game_rules::ForceCustomContent(true) && !sLogged) {
        sLogged = true;
        UE_LOGI("[TV-LINK-DRILL] [%s] the Custom Content rule was off in this process -- forced on for the drill", Who());
    }
    const std::vector<void*> all = TV::FindAll();
    for (void* tv : all) TV::ForcePowered(tv, true);
    return all.empty() ? nullptr : all.front();
}

int g_controlStep = 0;

// HOST, `control:`: what a player's stop button, slider and play button do, by the player's verbs and the lane's
// own send.
void HostControls(void* tv, uint64_t sinceMs) {
    namespace N = coop::net;
    if (g_controlStep == 0 && sinceMs >= 4000) {
        g_controlStep = 1;
        const double at = TV::PlayTime(tv);
        const bool ok = TV::Pause(tv) && coop::tv_link_sync::ControlLocal(tv, N::kTvOpPause, at);
        UE_LOGI("[TV-LINK-DRILL] [H] paused at %.1fs and sent it: %d", at, ok ? 1 : 0);
    } else if (g_controlStep == 1 && sinceMs >= 10000) {
        g_controlStep = 2;
        const bool ok = TV::Seek(tv, 1.0) && TV::Resume(tv) && coop::tv_link_sync::ControlLocal(tv, N::kTvOpResume, 1.0);
        UE_LOGI("[TV-LINK-DRILL] [H] resumed from 1.0s and sent it: %d", ok ? 1 : 0);
    }
}

void Watch(uint64_t now) {
    void* tv = FirstSet();
    if (tv && ThePlan().control && coop::roster::LocalIsHost()) HostControls(tv, now - g_playedMs);
    const int playing = tv ? TV::IsPlaying(tv) : -1;
    const double t = tv ? TV::PlayTime(tv) : -1.0;
    if (playing == 1 && t > 0.0 && g_first < 0.0) g_first = t;
    g_last = t;
    if (t > g_max) g_max = t;
    UE_LOGI("[TV-LINK-DRILL] [%s] t+%llus set=%d playing=%d time=%.1fs opened=%llu controls=%llu", Who(),
            static_cast<unsigned long long>((now - g_playedMs) / 1000), tv ? 1 : 0, playing, t,
            static_cast<unsigned long long>(coop::tv_link_sync::Opened()),
            static_cast<unsigned long long>(coop::tv_link_sync::Controls()));
    if (now - g_playedMs < kWatchMs) return;
    // A clip shorter than the watch plays again from its start, so the furthest time is the measure.
    if (g_first >= 0.0 && g_max >= g_first + 2.0)
        UE_LOGI("[TV-LINK-DRILL] [%s] PASS the set's play time moved from %.1fs to %.1fs", Who(), g_first, g_max);
    else
        UE_LOGW("[TV-LINK-DRILL] [%s] FAIL the set's play time did not move (first %.1fs, last %.1fs)", Who(), g_first,
                g_last);
    g_done = true;
}

}  // namespace

void Tick(coop::net::Session* session) {
    if (g_done || Url().empty() || !session || !session->connected()) return;
    const uint64_t now = coop::net::NowMs();
    if (now < g_nextMs) return;
    g_nextMs = now + kStepMs;
    if (!g_startMs) g_startMs = now;
    if (g_playedMs) {
        Watch(now);
        return;
    }
    if (now - g_startMs > kGiveUpMs) {
        UE_LOGW("[TV-LINK-DRILL] [%s] FAIL no link was played within %llu s", Who(),
                static_cast<unsigned long long>(kGiveUpMs / 1000));
        g_done = true;
        return;
    }
    if (!coop::roster::LocalIsHost()) {
        void* copy = FirstSet();  // power for the copy, before a link lands
        if (ThePlan().fromClient && copy) {
            // The asker is this client: its copy has stood for the settle time, so the host's set has too.
            if (!g_readyMs) g_readyMs = now;
            if (now - g_readyMs < kSettleMs) return;
            const bool asked = coop::tv_link_sync::PlayLocal(copy, Url());
            UE_LOGI("[TV-LINK-DRILL] [C] asked this peer's copy of the set for the address: %d", asked ? 1 : 0);
            if (asked) g_playedMs = now;
            else g_done = true;
            return;
        }
        if (coop::tv_link_sync::Opened() > 0) {
            g_playedMs = now;
            UE_LOGI("[TV-LINK-DRILL] [C] a link was opened on this peer's set");
        }
        return;
    }
    if (!ClientInWorld(*session)) return;
    if (!g_readyMs) g_readyMs = now;
    if (now - g_readyMs < kSettleMs) return;
    void* tv = FirstSet();
    if (!tv) {
        // The save holds no set: the mini TV's class is spawned in front of the host, once.
        if (g_candidate > 0) {
            if (now - g_readyMs > 60000) {
                UE_LOGW("[TV-LINK-DRILL] [H] FAIL the spawned mini TV never appeared among the world's televisions");
                g_done = true;
            }
            return;
        }
        g_candidate = 1;
        void* me = coop::players::Registry::Get().Local();
        void* cls = ue_wrap::asset_load::LoadObjectByPath(L"/Game/objects/prop_minitv.prop_minitv_C");
        ue_wrap::FVector at{};
        void* made = nullptr;
        if (me && cls && ue_wrap::engine::TryGetActorLocation(me, at)) {
            const ue_wrap::FVector fwd = ue_wrap::engine::GetActorForwardVector(me);
            made = ue_wrap::engine::SpawnActor(cls, {at.X + fwd.X * 150.f, at.Y + fwd.Y * 150.f, at.Z + 40.f});
        }
        UE_LOGI("[TV-LINK-DRILL] [H] no television here -- a mini TV spawned in front of the host: %d", made ? 1 : 0);
        if (!made) {
            UE_LOGW("[TV-LINK-DRILL] [H] FAIL the mini TV's class did not load or spawn");
            g_done = true;
        }
        g_readyMs = now - kSettleMs + 6000;  // six seconds for the prop to reach the client
        return;
    }
    if (ThePlan().fromClient) {
        // The host only watches: its set plays once the client's address has crossed.
        if (coop::tv_link_sync::Opened() > 0) {
            g_playedMs = now;
            UE_LOGI("[TV-LINK-DRILL] [H] the client's link was opened on the host's set");
        }
        return;
    }
    const std::wstring key = ue_wrap::prop::GetInteractableKeyString(tv);
    const uint64_t before = coop::tv_link_sync::Opened();
    const bool asked = ThePlan().typed    ? (TypeOnRemote(tv, Url()) && coop::tv_link_sync::Opened() > before)
                       : ThePlan().viaSet ? (TV::TakeAnyChannel(tv) && TV::OpenLink(tv, L"drill", Url()))
                                          : coop::tv_link_sync::PlayLocal(tv, Url());
    UE_LOGI("[TV-LINK-DRILL] [H] asked the set key='%ls' (%ls) for the address%s: %d", key.c_str(),
            ue_wrap::reflection::ClassNameOf(tv).c_str(), ThePlan().typed ? ", typed on its remote" : "", asked ? 1 : 0);
    if (!asked) {
        UE_LOGW("[TV-LINK-DRILL] [H] FAIL the lane refused the address");
        g_done = true;
        return;
    }
    g_playedMs = now;
}

void OnDisconnect() {
    g_readyMs = g_nextMs = g_playedMs = g_startMs = 0;
    g_first = g_last = g_max = -1.0;
    g_controlStep = 0;
    g_candidate = 0;
}

}  // namespace coop::dev::tv_link_drill
