// coop/world/world_sound_sync.cpp -- see coop/world/world_sound_sync.h.

#include "coop/world/world_sound_sync.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_sound.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <chrono>
#include <cstdint>

namespace coop::world_sound_sync {
namespace {

namespace R = ue_wrap::reflection;
namespace H = ue_wrap::ufunction_hook;

UE_FACT_FN(kRowPlaySound2D, "world_sound_sync", L"GameplayStatics", L"PlaySound2D", Dispatch);

// As the sky's own calls play them.
constexpr float kEarthVolume = 1.5f;
constexpr float kEarthPitch = 0.f;
constexpr int kResolveTries = 3;   // the sounds load with the sky's class; a world that has not got them in three asks has none
constexpr int kResolveGapSec = 5;
using Clock = std::chrono::steady_clock;

std::atomic<coop::net::Session*> g_session{nullptr};
H::PreNativeCallback g_cb = nullptr;
void* g_fn = nullptr;       // PlaySound2D, once the hook is on
int32_t g_offSound = -1;
bool g_armed = false;
// The two sounds' objects, found once per world: the hook compares the Sound argument's pointer and never reads it.
void* g_soundIn = nullptr;
void* g_soundOut = nullptr;
uint32_t g_soundsWorld = 0;
int g_tries = 0;
Clock::time_point g_nextTry{};
Counts g_c;

// The drill's red arms: the host sends no cue for the sky's sounds, and no faucet click is played here.
bool EarthOff() {
    static const bool off = coop::config::ResolveFlag(coop::config_registry::rows::world_sound_earth_off);
    return off;
}
bool FaucetOff() {
    static const bool off = coop::config::ResolveFlag(coop::config_registry::rows::world_sound_faucet_off);
    return off;
}

// Before the original plays: the sky's tp plays earthTP_in and, twenty seconds on, earthTP_out, each 2D, so only the
// machine whose scheduler ran tp hears them. The Sound argument is the identity; the caller is the sky's own Blueprint.
ue_wrap::script_gate::Verdict OnPlaySound2DPre(void* /*context*/, void* source, const uint8_t* parms) {
    void* const sound = *reinterpret_cast<void* const*>(parms + g_offSound);
    if (!sound || (sound != g_soundIn && sound != g_soundOut)) return ue_wrap::script_gate::Verdict::Run;
    if (!source || !R::IsLive(source) || R::ClassNameOf(source) != L"newsky_C") return ue_wrap::script_gate::Verdict::Run;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Host) return ue_wrap::script_gate::Verdict::Run;
    ++g_c.matched;
    if (EarthOff()) return ue_wrap::script_gate::Verdict::Run;
    const bool in = sound == g_soundIn;
    coop::net::EventCuePayload p{};
    p.cueId = in ? kCueEarthIn : kCueEarthOut;
    if (s->SendReliable(coop::net::ReliableKind::EventCue, &p, sizeof(p))) ++g_c.sent;
    UE_LOGI("world_sound: the sky's %hs sound -- EventCue sent (#%u)", in ? "earthTP_in" : "earthTP_out", g_c.sent);
    return ue_wrap::script_gate::Verdict::Run;
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

// The hook goes on once for the process; the host arms it while its session runs, so a client and a single-player game
// pay one load and a branch on every sound.
void InstallHook() {
    if (g_fn) return;
    const ue_wrap::facts::Resolved fn = ue_wrap::facts::Resolve(kRowPlaySound2D);
    if (!fn) return;
    const int32_t off = R::FindParamOffset(fn.Fn(), L"Sound");
    if (off < 0 || !H::InstallPreHook(fn, &OnPlaySound2DPre, /*armed=*/false)) {
        static bool sSaid = false;
        if (!sSaid) UE_LOGE("world_sound: no pre hook on PlaySound2D (Sound offset %d) -- the sky's sounds stay the host's", off);
        sSaid = true;
        return;
    }
    g_cb = &OnPlaySound2DPre;
    g_offSound = off;
    g_fn = fn.Fn();
}

// Once per world: the two sounds' objects. A search of the object array, tried a few times while the world's sky loads.
void ResolveSounds() {
    const uint32_t world = ue_wrap::world_identity::Generation();
    if (world != g_soundsWorld) {
        g_soundsWorld = world;
        g_soundIn = g_soundOut = nullptr;
        g_tries = 0;
    }
    if ((g_soundIn && g_soundOut) || g_tries >= kResolveTries) return;
    const Clock::time_point now = Clock::now();
    if (now < g_nextTry) return;
    g_nextTry = now + std::chrono::seconds(kResolveGapSec);
    ++g_tries;
    g_soundIn = R::FindObject(L"earthTP_in", L"SoundWave");
    g_soundOut = R::FindObject(L"earthTP_out", L"SoundWave");
    if (g_tries == kResolveTries && !(g_soundIn && g_soundOut))
        UE_LOGW("world_sound: earthTP_in or earthTP_out is not in this world (in=%p out=%p) -- the sky's sounds stay the host's",
                g_soundIn, g_soundOut);
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    InstallHook();
    if (!g_fn) return;
    const bool host = session && session->connected() && session->role() == coop::net::Role::Host;
    if (host) ResolveSounds();
    const bool want = host && g_soundIn && g_soundOut;
    if (want != g_armed && H::SetArmed(g_fn, g_cb, want)) g_armed = want;
}

bool OnCue(const coop::net::EventCuePayload& p) {
    if (p.cueId != kCueEarthIn && p.cueId != kCueEarthOut) return false;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Client) return true;
    ++g_c.received;
    const wchar_t* const name = p.cueId == kCueEarthIn ? L"earthTP_in" : L"earthTP_out";
    const bool played = Play2D(name);
    if (played) ++g_c.played;
    UE_LOGI("world_sound: %ls %hs (#%u)", name, played ? "played" : "NOT played", g_c.played);
    return true;
}

void OnFaucetChanged(void* faucet) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || FaucetOff()) return;
    // A joiner's first states are the host's world arriving, not a press.
    if (s->role() == coop::net::Role::Client && !coop::net_pump::HasAnnouncedWorldReady()) return;
    coop::prop_sound::PlayFaucetClick(faucet);
    ++g_c.clicks;
    UE_LOGI("world_sound: faucet click played (#%u)", g_c.clicks);
}

Counts GetCounts() { return g_c; }

void OnDisconnect() {
    if (g_c.sent || g_c.received || g_c.played || g_c.clicks)
        UE_LOGI("world_sound: session end -- sent=%u received=%u played=%u clicks=%u", g_c.sent, g_c.received, g_c.played,
                g_c.clicks);
    g_c = Counts{};
}

}  // namespace coop::world_sound_sync
