// coop/world/world_sound_sync.cpp -- see coop/world/world_sound_sync.h.

#include "coop/world/world_sound_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_sound.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cstdint>
#include <cstring>

namespace coop::world_sound_sync {
namespace {

namespace R = ue_wrap::reflection;
namespace H = ue_wrap::ufunction_hook;

UE_FACT_FN(kRowPlaySound2D, "world_sound_sync", L"GameplayStatics", L"PlaySound2D", Dispatch);

// The sky's two PlaySound2D calls, by the ubergraph entry each runs in: tp's start and its continuation (0.9.0n).
constexpr int32_t kEntryEarthIn = 5062;
constexpr int32_t kEntryEarthOut = 437;

// As the sky's own calls play them.
constexpr float kEarthVolume = 1.5f;
constexpr float kEarthPitch = 0.f;

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_hooked = false;
uint32_t g_sent = 0, g_played = 0, g_clicks = 0;

void OnPlaySound2D(void* /*context*/, void* source, void* /*result*/) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Host) return;
    if (!source || !R::IsLive(source) || R::ClassNameOf(source) != L"newsky_C") return;
    const auto frame = H::CurrentCallerFrame();
    if (!frame.function || !frame.locals || !R::NameEquals(R::NameOf(frame.function), L"ExecuteUbergraph_newsky")) return;
    const int32_t off = R::FindParamOffset(frame.function, L"EntryPoint");
    if (off < 0) return;
    int32_t entry = 0;
    std::memcpy(&entry, frame.locals + off, sizeof(entry));
    if (entry != kEntryEarthIn && entry != kEntryEarthOut) return;
    coop::net::EventCuePayload p{};
    p.cueId = entry == kEntryEarthIn ? kCueEarthIn : kCueEarthOut;
    if (s->SendReliable(coop::net::ReliableKind::EventCue, &p, sizeof(p))) ++g_sent;
    UE_LOGI("world_sound: the sky's %hs sound -- EventCue sent (#%u)", entry == kEntryEarthIn ? "earthTP_in" : "earthTP_out", g_sent);
}

bool Play2D(const wchar_t* soundName) {
    void* const local = coop::players::Registry::Get().Local();
    void* const asset = R::FindObject(soundName, L"SoundWave");
    void* const statics = R::FindClassDefaultObject(L"GameplayStatics");
    void* const fn = statics ? R::FindDispatchFunctionCached(R::ClassOf(statics), L"PlaySound2D") : nullptr;
    if (!local || !asset || !fn) {
        UE_LOGW("world_sound: %ls did not resolve -- no sound", soundName);
        return false;
    }
    ue_wrap::ParamFrame f(fn);
    if (!f.valid() || !f.Set<void*>(L"WorldContextObject", local) || !f.Set<void*>(L"Sound", asset) ||
        !f.Set<float>(L"VolumeMultiplier", kEarthVolume) || !f.Set<float>(L"PitchMultiplier", kEarthPitch))
        return false;
    return ue_wrap::Call(statics, f);
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_hooked) return;
    const ue_wrap::facts::Resolved fn = ue_wrap::facts::Resolve(kRowPlaySound2D);
    if (fn) g_hooked = H::InstallPostHook(fn, &OnPlaySound2D);
}

bool OnCue(const coop::net::EventCuePayload& p) {
    if (p.cueId != kCueEarthIn && p.cueId != kCueEarthOut) return false;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Client) return true;
    const wchar_t* const name = p.cueId == kCueEarthIn ? L"earthTP_in" : L"earthTP_out";
    const bool played = Play2D(name);
    if (played) ++g_played;
    UE_LOGI("world_sound: %ls %hs (#%u)", name, played ? "played" : "NOT played", g_played);
    return true;
}

void OnFaucetChanged(void* faucet) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    // A joiner's first states are the host's world arriving, not a press.
    if (s->role() == coop::net::Role::Client && !coop::net_pump::HasAnnouncedWorldReady()) return;
    coop::prop_sound::PlayFaucetClick(faucet);
    ++g_clicks;
    UE_LOGI("world_sound: faucet click played (#%u)", g_clicks);
}

void OnDisconnect() {
    if (g_sent || g_played || g_clicks)
        UE_LOGI("world_sound: session end -- sent=%u played=%u clicks=%u", g_sent, g_played, g_clicks);
    g_sent = g_played = g_clicks = 0;
}

}  // namespace coop::world_sound_sync
