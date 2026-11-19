// coop/world/sound_relay.cpp -- see coop/world/sound_relay.h.

#include "coop/world/sound_relay.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"

#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/devices/grime.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_audio.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/facts/function_row.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>

namespace coop::sound_relay {
namespace {

namespace R = ue_wrap::reflection;
namespace H = ue_wrap::ufunction_hook;
namespace N = coop::net;
using Verdict = ue_wrap::script_gate::Verdict;

UE_FACT_FN(kRowPlaySoundAt, "sound_relay", L"GameplayStatics", L"PlaySoundAtLocation", Dispatch);
UE_FACT_FN(kRowPlaySound2D, "sound_relay", L"GameplayStatics", L"PlaySound2D", Dispatch);

constexpr int kMaxSendsPerSecond = 20;   // this machine's relayed sounds past this in one second are dropped
constexpr int kMaxRecvPerSecond = 30;    // and a sender's received ones

// The Blueprint functions whose sounds are relayed, with the class that declares each. kOwnPlayer: sent by the machine
// whose own player ran it, either role. kHostWorld: an actor that lives on the host alone, sent by the host. kGrime: a
// grime decal's own clean sound, sent by the machine that cleaned it -- grime_C::clean runs only where a sponge, a mop
// or a cleaner scrubbed it (another peer takes the clean's dirt from coop/interactables/grime_sync, not from clean).
enum class From : uint8_t { kOwnPlayer, kHostWorld, kGrime };
struct Caller {
    const wchar_t* cls;
    const wchar_t* fn;
    From from;
};
constexpr Caller kCallers[] = {
    {L"mainPlayer_C", L"attack", From::kOwnPlayer},   // a melee hit's impact sound
    {L"kocker_C", L"knock", From::kHostWorld},        // the vent knocker's knocks (Ghezra/Multivoid#39)
    {L"grime_C", L"ExecuteUbergraph_grime", From::kGrime},          // a scrubbed decal's squelch (its clean's Sound)
};
constexpr int kCallerCount = static_cast<int>(sizeof(kCallers) / sizeof(kCallers[0]));

std::atomic<N::Session*> g_session{nullptr};
void* g_fn = nullptr;
bool g_installFailed = false;
bool g_armed = false;
int32_t g_offSound = -1, g_offLoc = -1, g_offVol = -1, g_offPitch = -1, g_offAtt = -1;
void* g_callerFn[kCallerCount] = {};
uint64_t g_sendSecond = 0;
int g_sendN = 0;
struct Rate {
    uint64_t second = 0;
    int n = 0;
};
Rate g_recv[N::kMaxPeers];
std::unordered_map<std::wstring, void*> g_assets;   // path -> loaded asset (assets are not collected while loaded)

uint64_t NowSec() { return ::GetTickCount64() / 1000; }

// "/Game/audio/x.x": the object's package and name.
std::wstring PathOf(void* o) {
    if (!o) return {};
    void* const outer = R::OuterOf(o);
    const std::wstring name = R::ToString(R::NameOf(o));
    if (!outer) return name;
    return PathOf(outer) + (R::OuterOf(outer) ? L":" : L".") + name;
}

bool FillPath(char* dst, size_t cap, void* o) {
    std::memset(dst, 0, cap);
    if (!o) return true;
    const std::wstring w = PathOf(o);
    if (w.empty() || w.size() >= cap) return false;
    for (size_t i = 0; i < w.size(); ++i) {
        if (w[i] > 0x7e) return false;
        dst[i] = static_cast<char>(w[i]);
    }
    return true;
}

void* Asset(const char* path, size_t cap) {
    const size_t n = strnlen(path, cap);
    if (n == 0 || n == cap) return nullptr;
    const std::wstring w(path, path + n);
    const auto it = g_assets.find(w);
    if (it != g_assets.end()) return it->second;
    void* const o = ue_wrap::asset_load::LoadObjectByPath(w.c_str());
    if (!o) UE_LOGW("sound_relay: '%ls' did not load -- not played", w.c_str());
    g_assets[w] = o;
    return o;
}

// The listed caller's index, or -1.
int CallerIndex(void* caller) {
    if (!caller) return -1;
    for (int i = 0; i < kCallerCount; ++i) {
        if (g_callerFn[i]) {
            if (g_callerFn[i] == caller) return i;
            continue;
        }
        void* const owner = R::OuterOf(caller);
        if (owner && R::NameEquals(R::NameOf(caller), kCallers[i].fn) && R::NameEquals(R::NameOf(owner), kCallers[i].cls)) {
            g_callerFn[i] = caller;
            UE_LOGI("sound_relay: %ls::%ls's sounds are relayed", kCallers[i].cls, kCallers[i].fn);
            return i;
        }
    }
    return -1;
}

Verdict OnPlaySoundAtPre(void* /*context*/, void* source, const uint8_t* parms) {
    const int i = CallerIndex(H::CurrentCallerFrame().function);
    if (i < 0) return Verdict::Run;
    N::Session* const s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || !source) return Verdict::Run;
    const From from = kCallers[i].from;
    if (from == From::kOwnPlayer ? source != coop::players::Registry::Get().Local()
        : from == From::kGrime   ? !ue_wrap::grime::IsGrime(source)
                                 : s->role() != N::Role::Host)
        return Verdict::Run;
    const uint64_t sec = NowSec();
    if (sec != g_sendSecond) {
        g_sendSecond = sec;
        g_sendN = 0;
    }
    if (++g_sendN > kMaxSendsPerSecond) return Verdict::Run;
    N::SoundAtPayload p{};
    void* const sound = *reinterpret_cast<void* const*>(parms + g_offSound);
    void* const att = g_offAtt >= 0 ? *reinterpret_cast<void* const*>(parms + g_offAtt) : nullptr;
    if (!sound || !FillPath(p.sound, sizeof(p.sound), sound) || !FillPath(p.attenuation, sizeof(p.attenuation), att))
        return Verdict::Run;
    std::memcpy(p.loc, parms + g_offLoc, sizeof(p.loc));
    p.volume = g_offVol >= 0 ? *reinterpret_cast<const float*>(parms + g_offVol) : 1.f;
    p.pitch = g_offPitch >= 0 ? *reinterpret_cast<const float*>(parms + g_offPitch) : 1.f;
    s->SendReliable(N::ReliableKind::SoundAt, &p, sizeof(p));
    UE_LOGI("sound_relay: this machine's '%hs' at (%.0f,%.0f,%.0f) sent", p.sound, p.loc[0], p.loc[1], p.loc[2]);
    return Verdict::Run;
}

// A 2D sound (GameplayStatics::PlaySound2D) is heard by this machine's player alone, wherever it is. The item in this
// player's hand plays its use's sounds so (the mop's and the sponge's dip, a bucket's scoop, its water thrown out or
// poured on a plant), so another player never heard them. One the hand's item plays goes out as a placed sound at this
// player, with the game's default attenuation (att_default, which the items' own placed sounds use), for the others.
void* g_fn2D = nullptr;
bool g_armed2D = false, g_install2DFailed = false;
int32_t g_off2DSound = -1, g_off2DVol = -1, g_off2DPitch = -1;
char g_attDefault[64] = {};
bool g_attResolved = false;

bool AttDefault() {
    if (g_attResolved) return g_attDefault[0] != 0;
    g_attResolved = true;
    for (void* o : R::FindObjectsByClass(L"SoundAttenuation"))
        if (o && R::NameEquals(R::NameOf(o), L"att_default") && FillPath(g_attDefault, sizeof(g_attDefault), o)) break;
    if (!g_attDefault[0]) UE_LOGW("sound_relay: no att_default attenuation -- a hand item's 2D sounds are not relayed");
    return g_attDefault[0] != 0;
}

Verdict OnPlaySound2DPre(void* /*context*/, void* source, const uint8_t* parms) {
    N::Session* const s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || !source) return Verdict::Run;
    void* const me = coop::players::Registry::Get().Local();
    ue_wrap::engine::MainPlayerGrabState gs{};
    if (!me || !ue_wrap::engine::ReadMainPlayerGrabState(me, gs) || !gs.holdingActor || gs.holdingActor != source)
        return Verdict::Run;
    if (!AttDefault()) return Verdict::Run;
    const uint64_t sec = NowSec();
    if (sec != g_sendSecond) {
        g_sendSecond = sec;
        g_sendN = 0;
    }
    if (++g_sendN > kMaxSendsPerSecond) return Verdict::Run;
    N::SoundAtPayload p{};
    void* const sound = *reinterpret_cast<void* const*>(parms + g_off2DSound);
    ue_wrap::FVector at{};
    if (!sound || !FillPath(p.sound, sizeof(p.sound), sound) || !ue_wrap::engine::TryGetActorLocation(me, at))
        return Verdict::Run;
    std::memcpy(p.attenuation, g_attDefault, sizeof(p.attenuation));
    p.loc[0] = at.X;
    p.loc[1] = at.Y;
    p.loc[2] = at.Z;
    p.volume = g_off2DVol >= 0 ? *reinterpret_cast<const float*>(parms + g_off2DVol) : 1.f;
    p.pitch = g_off2DPitch >= 0 ? *reinterpret_cast<const float*>(parms + g_off2DPitch) : 1.f;
    s->SendReliable(N::ReliableKind::SoundAt, &p, sizeof(p));
    UE_LOGI("sound_relay: this player's hand item's 2D '%hs' sent at (%.0f,%.0f,%.0f)", p.sound, p.loc[0], p.loc[1],
            p.loc[2]);
    return Verdict::Run;
}

void Install2DHook() {
    if (g_fn2D || g_install2DFailed) return;
    const ue_wrap::facts::Resolved fn = ue_wrap::facts::Resolve(kRowPlaySound2D);
    if (!fn) return;
    g_off2DSound = R::FindParamOffset(fn.Fn(), L"Sound");
    g_off2DVol = R::FindParamOffset(fn.Fn(), L"VolumeMultiplier");
    g_off2DPitch = R::FindParamOffset(fn.Fn(), L"PitchMultiplier");
    if (g_off2DSound < 0 || !H::InstallPreHook(fn, &OnPlaySound2DPre, /*armed=*/false)) {
        g_install2DFailed = true;
        UE_LOGE("sound_relay: no pre hook on PlaySound2D (Sound %d) -- a hand item's 2D sounds are not relayed",
                g_off2DSound);
        return;
    }
    g_fn2D = fn.Fn();
}

void InstallHook() {
    if (g_fn || g_installFailed) return;
    const ue_wrap::facts::Resolved fn = ue_wrap::facts::Resolve(kRowPlaySoundAt);
    if (!fn) return;
    g_offSound = R::FindParamOffset(fn.Fn(), L"Sound");
    g_offLoc = R::FindParamOffset(fn.Fn(), L"Location");
    g_offVol = R::FindParamOffset(fn.Fn(), L"VolumeMultiplier");
    g_offPitch = R::FindParamOffset(fn.Fn(), L"PitchMultiplier");
    g_offAtt = R::FindParamOffset(fn.Fn(), L"AttenuationSettings");
    if (g_offSound < 0 || g_offLoc < 0 || !H::InstallPreHook(fn, &OnPlaySoundAtPre, /*armed=*/false)) {
        g_installFailed = true;
        UE_LOGE("sound_relay: no pre hook on PlaySoundAtLocation (Sound %d, Location %d) -- no sound is relayed", g_offSound,
                g_offLoc);
        return;
    }
    g_fn = fn.Fn();
}

}  // namespace

void Install(N::Session* session) {
    g_session.store(session, std::memory_order_release);
    InstallHook();
    Install2DHook();
    const bool want = session && session->connected();
    if (g_fn && want != g_armed && H::SetArmed(g_fn, &OnPlaySoundAtPre, want)) g_armed = want;
    if (g_fn2D && want != g_armed2D && H::SetArmed(g_fn2D, &OnPlaySound2DPre, want)) g_armed2D = want;
}

void OnSoundAt(int senderSlot, const N::SoundAtPayload& p) {
    if (senderSlot < 0 || senderSlot >= N::kMaxPeers) return;
    Rate& r = g_recv[senderSlot];
    const uint64_t sec = NowSec();
    if (r.second != sec) r = Rate{sec, 0};
    if (++r.n > kMaxRecvPerSecond) return;
    if (!std::isfinite(p.loc[0]) || !std::isfinite(p.loc[1]) || !std::isfinite(p.loc[2]) || !std::isfinite(p.volume) ||
        !std::isfinite(p.pitch))
        return;
    void* const local = coop::players::Registry::Get().Local();
    void* const sound = Asset(p.sound, sizeof(p.sound));
    if (!local || !sound) return;
    void* const att = p.attenuation[0] ? Asset(p.attenuation, sizeof(p.attenuation)) : nullptr;
    const float volume = p.volume < 0.f ? 0.f : (p.volume > 4.f ? 4.f : p.volume);
    const float pitch = p.pitch < 0.1f ? 0.1f : (p.pitch > 4.f ? 4.f : p.pitch);
    ue_wrap::engine::PlaySoundAtLocation(local, sound, {p.loc[0], p.loc[1], p.loc[2]}, att, volume, pitch);
    UE_LOGI("sound_relay: slot %d's '%hs' played at (%.0f,%.0f,%.0f)", senderSlot, p.sound, p.loc[0], p.loc[1], p.loc[2]);
}

void OnDisconnect() {
    if (g_fn && g_armed && H::SetArmed(g_fn, &OnPlaySoundAtPre, false)) g_armed = false;
    if (g_fn2D && g_armed2D && H::SetArmed(g_fn2D, &OnPlaySound2DPre, false)) g_armed2D = false;
    for (Rate& r : g_recv) r = Rate{};
    g_session.store(nullptr, std::memory_order_release);
}

}  // namespace coop::sound_relay
