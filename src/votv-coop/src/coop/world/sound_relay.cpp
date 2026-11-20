// coop/world/sound_relay.cpp -- see coop/world/sound_relay.h.

#include "coop/world/sound_relay.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"  // the sound drill's red arm of the send
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/world/sound_catalog.h"
#include "coop/world/sound_judge.h"

#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_path.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/engine/engine_audio.h"
#include "ue_wrap/facts/function_row.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace coop::sound_relay {
namespace {

namespace R = ue_wrap::reflection;
namespace H = ue_wrap::ufunction_hook;
namespace N = coop::net;
namespace C = coop::sound_catalog;
using Verdict = ue_wrap::script_gate::Verdict;

UE_FACT_FN(kRowPlaySoundAt, "sound_relay", L"GameplayStatics", L"PlaySoundAtLocation", Dispatch);

constexpr uint64_t kRefusalSayMs = 10000;

// The Blueprint functions whose sounds are relayed, with the class that declares each. kOwnPlayer: sent by the machine
// whose own player ran it, either role. kHostWorld: an actor that lives on the host alone, sent by the host. `bit` is the
// sound table's name for the caller: the sound it plays must allow it.
enum class From : uint8_t { kOwnPlayer, kHostWorld };
struct Caller {
    const wchar_t* cls;
    const wchar_t* fn;
    From from;
    uint8_t bit;
};
constexpr Caller kCallers[] = {
    {L"mainPlayer_C", L"attack", From::kOwnPlayer, C::kAttack},   // a melee hit's impact sound
    {L"kocker_C", L"knock", From::kHostWorld, C::kKnock},         // the vent knocker's knocks (Ghezra/Multivoid#39)
    // The wandering church's bells (Ghezra/Multivoid#39): `bell` only calls the ubergraph at its bell entry, and the
    // ubergraph holds every PlaySoundAtLocation of the chime, so that is the frame the call is seen from.
    {L"church_C", L"ExecuteUbergraph_church", From::kHostWorld, C::kBell},
};
constexpr int kCallerCount = static_cast<int>(sizeof(kCallers) / sizeof(kCallers[0]));

std::atomic<N::Session*> g_session{nullptr};
void* g_fn = nullptr;
bool g_installFailed = false;
bool g_armed = false;
int32_t g_offSound = -1, g_offLoc = -1, g_offVol = -1, g_offPitch = -1;
void* g_callerFn[kCallerCount] = {};
std::unordered_set<void*> g_notCaller;   // functions judged not to be a listed caller
bool g_cancelSaid[kCallerCount] = {};
bool g_notReadySaid = false;
Counts g_counts;
uint64_t g_nextRefusalSayMs[static_cast<int>(Refusal::kCount)] = {};
std::unordered_set<std::wstring> g_uncataloguedSaid;
std::unordered_set<std::wstring> g_loadFailedSaid;
std::unordered_map<std::wstring, ue_wrap::CachedObjRef> g_assets;   // path -> the loaded asset

bool SendOff() {
    static const bool s = coop::config::ResolveFlag(coop::config_registry::rows::sound_send_off);
    return s;
}

// The asset at `path`, loaded on first use and again when the engine has let it go. A path that does not load is said once.
void* Asset(const wchar_t* path) {
    ue_wrap::CachedObjRef& ref = g_assets[path];
    if (void* const o = ref.Get()) return o;
    void* const o = ue_wrap::asset_load::LoadObjectByPath(path);
    ref.Set(o);
    if (!o && g_loadFailedSaid.insert(path).second) UE_LOGW("sound_relay: '%ls' did not load -- not played", path);
    return o;
}

// The listed caller's index, or -1.
// Every Blueprint's PlaySoundAtLocation reaches the hook, so a function judged not to be a listed caller is remembered: a
// later call from it costs a hash lookup, not the name renders.
int CallerIndex(void* caller) {
    if (!caller) return -1;
    for (int i = 0; i < kCallerCount; ++i)
        if (g_callerFn[i] == caller) return i;
    if (g_notCaller.count(caller)) return -1;
    void* const owner = R::OuterOf(caller);
    for (int i = 0; i < kCallerCount; ++i) {
        if (g_callerFn[i]) continue;
        if (owner && R::NameEquals(R::NameOf(caller), kCallers[i].fn) && R::NameEquals(R::NameOf(owner), kCallers[i].cls)) {
            g_callerFn[i] = caller;
            UE_LOGI("sound_relay: %ls::%ls's sounds are relayed", kCallers[i].cls, kCallers[i].fn);
            return i;
        }
    }
    g_notCaller.insert(caller);
    return -1;
}

// Counts a refusal and says it, the first of its kind at once and then at most every kRefusalSayMs.
void Refuse(int senderSlot, Refusal why, unsigned index) {
    const int w = static_cast<int>(why);
    ++g_counts.refused[w];
    const uint64_t now = ::GetTickCount64();
    if (now < g_nextRefusalSayMs[w]) return;
    g_nextRefusalSayMs[w] = now + kRefusalSayMs;
    UE_LOGI("sound_relay: refused %s -- slot %d's sound index %u (%u so far)", RefusalName(why), senderSlot, index,
            g_counts.refused[w]);
}

Verdict OnPlaySoundAtPre(void* /*context*/, void* source, const uint8_t* parms) {
    const int i = CallerIndex(H::CurrentCallerFrame().function);
    if (i < 0) return Verdict::Run;
    N::Session* const s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || !source) return Verdict::Run;
    // The host's world plays its sounds on the host alone, and the host's relayed copy is the one every client hears: a
    // client's own copy of such an actor (the church is a level actor) plays nothing.
    if (kCallers[i].from == From::kHostWorld && s->role() == N::Role::Client) {
        if (!g_cancelSaid[i]) {
            g_cancelSaid[i] = true;
            UE_LOGI("sound_relay: this client's %ls::%ls sounds are cancelled -- the host's relayed sound is the one it hears",
                    kCallers[i].cls, kCallers[i].fn);
        }
        return Verdict::Cancel;
    }
    if (kCallers[i].from == From::kOwnPlayer ? source != coop::players::Registry::Get().Local()
                                             : s->role() != N::Role::Host)
        return Verdict::Run;
    void* const sound = *reinterpret_cast<void* const*>(parms + g_offSound);
    if (!sound) return Verdict::Run;
    // Until this peer's table is built it sends nothing (the next sound tries again), and says so once.
    if (!C::Ready()) {
        ++g_counts.notReadyHere;
        if (!g_notReadySaid) {
            g_notReadySaid = true;
            UE_LOGI("sound_relay: catalogue not ready -- this peer sends no sound until its table builds (said once)");
        }
        return Verdict::Run;
    }
    const std::wstring path = ue_wrap::object_path::Of(sound);
    const uint16_t index = C::IndexOf(path);
    const C::Entry* const entry = C::Find(index);
    if (!entry || !(entry->callers & kCallers[i].bit)) {
        ++g_counts.notCatalogued;
        if (g_uncataloguedSaid.insert(path).second)
            UE_LOGW("sound_relay: not catalogued: %ls from %ls::%ls is not in the sound table for that caller -- it is not "
                    "relayed", path.c_str(), kCallers[i].cls, kCallers[i].fn);
        return Verdict::Run;
    }
    ++g_counts.playedHere;
    g_counts.lastPlayedHere = index;
    UE_LOGI("sound_relay: played here index %u '%ls'", static_cast<unsigned>(index), path.c_str());
    if (SendOff()) {
        UE_LOGI("sound_relay: sound_send_off: index %u is not sent", static_cast<unsigned>(index));
        return Verdict::Run;
    }
    N::SoundAtPayload p{};
    p.sound = index;
    std::memcpy(p.loc, parms + g_offLoc, sizeof(p.loc));
    p.volume = g_offVol >= 0 ? *reinterpret_cast<const float*>(parms + g_offVol) : 1.f;
    p.pitch = g_offPitch >= 0 ? *reinterpret_cast<const float*>(parms + g_offPitch) : 1.f;
    p.catalogue = C::Digest();
    s->SendReliable(N::ReliableKind::SoundAt, &p, sizeof(p));
    ++g_counts.sent;
    UE_LOGI("sound_relay: sent index %u at (%.0f,%.0f,%.0f)", static_cast<unsigned>(index), p.loc[0], p.loc[1], p.loc[2]);
    return Verdict::Run;
}

void InstallHook() {
    if (g_fn || g_installFailed) return;
    const ue_wrap::facts::Resolved fn = ue_wrap::facts::Resolve(kRowPlaySoundAt);
    if (!fn) return;
    g_offSound = R::FindParamOffset(fn.Fn(), L"Sound");
    g_offLoc = R::FindParamOffset(fn.Fn(), L"Location");
    g_offVol = R::FindParamOffset(fn.Fn(), L"VolumeMultiplier");
    g_offPitch = R::FindParamOffset(fn.Fn(), L"PitchMultiplier");
    if (g_offSound < 0 || g_offLoc < 0 || !H::InstallPreHook(fn, &OnPlaySoundAtPre, /*armed=*/false)) {
        g_installFailed = true;
        UE_LOGE("sound_relay: no pre hook on PlaySoundAtLocation (Sound %d, Location %d) -- no sound is relayed", g_offSound,
                g_offLoc);
        return;
    }
    g_fn = fn.Fn();
}

}  // namespace

const char* RefusalName(Refusal r) {
    switch (r) {
    case Refusal::NotReady: return "catalogue not ready";
    case Refusal::Digest:   return "digest";
    case Refusal::Index:    return "index";
    case Refusal::Caller:   return "caller";
    case Refusal::NoBody:   return "nobody";
    case Refusal::Reach:    return "reach";
    case Refusal::Budget:   return "budget";
    default:                return "?";
    }
}

Counts GetCounts() { return g_counts; }

void Install(N::Session* session) {
    g_session.store(session, std::memory_order_release);
    InstallHook();
    if (!g_fn) return;
    const bool want = session && session->connected();
    if (want != g_armed && H::SetArmed(g_fn, &OnPlaySoundAtPre, want)) g_armed = want;
}

void OnSoundAt(int senderSlot, const N::SoundAtPayload& p) {
    N::Session* const s = g_session.load(std::memory_order_acquire);
    if (!s || senderSlot < 0 || senderSlot >= N::kMaxPeers) return;
    // A client's one link is the host, whose sound is judged already; the host hears clients only, and judges each ask.
    const bool host = s->role() == N::Role::Host;
    if (host && senderSlot < 1) return;
    void* const local = coop::players::Registry::Get().Local();
    if (!local) return;
    const C::Entry* entry = nullptr;
    switch (C::Lookup(p.catalogue, p.sound, entry)) {
    case C::Resolve::Ok: break;
    case C::Resolve::NotReady: Refuse(senderSlot, Refusal::NotReady, p.sound); return;
    case C::Resolve::Digest:   Refuse(senderSlot, Refusal::Digest, p.sound); return;
    case C::Resolve::Index:    Refuse(senderSlot, Refusal::Index, p.sound); return;
    }
    if (host) {
        Refusal why{};
        if (!coop::sound_judge::Allow(*s, static_cast<uint8_t>(senderSlot), *entry, p, why)) {
            Refuse(senderSlot, why, p.sound);
            return;
        }
    }
    if (!std::isfinite(p.loc[0]) || !std::isfinite(p.loc[1]) || !std::isfinite(p.loc[2])) return;
    void* const sound = Asset(entry->path.c_str());
    if (!sound) return;
    void* const att = entry->attenuation ? Asset(entry->attenuation) : nullptr;
    N::SoundAtPayload out = p;
    C::Clamp(*entry, out.volume, out.pitch);
    ue_wrap::engine::PlaySoundAtLocation(local, sound, {out.loc[0], out.loc[1], out.loc[2]}, att, out.volume, out.pitch);
    ++g_counts.played;
    g_counts.lastPlayed = p.sound;
    UE_LOGI("sound_relay: slot %d's index %u played '%ls' at (%.0f,%.0f,%.0f)", senderSlot, static_cast<unsigned>(p.sound),
            entry->path.c_str(), p.loc[0], p.loc[1], p.loc[2]);
    // The host sends a client's judged sound to every other client, with the digest and the values it played.
    if (host) s->SendReliable(N::ReliableKind::SoundAt, &out, sizeof(out), senderSlot);
}

void OnDisconnect() {
    if (g_fn && g_armed && H::SetArmed(g_fn, &OnPlaySoundAtPre, false)) g_armed = false;
    g_session.store(nullptr, std::memory_order_release);
}

}  // namespace coop::sound_relay
