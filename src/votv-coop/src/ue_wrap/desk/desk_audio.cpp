// ue_wrap/desk_audio.cpp -- see ue_wrap/desk_audio.h.

#include "ue_wrap/desk/desk_audio.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/desk/console_desk.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <chrono>
#include <cstring>
#include <string>
#include <unordered_map>

namespace ue_wrap::desk_audio {
namespace {

namespace R = ue_wrap::reflection;

using Clock = std::chrono::steady_clock;

// The frozen wire-order component name table (protocol.h DeskSndComp).
const wchar_t* const kCompNames[kCompCount] = {
    L"audio_coordKeyPress",     // 0
    L"audio_coordFail",         // 1
    L"audio_coordButtonSound",  // 2
    L"audio_coord_pingSound",   // 3
    L"corrds_loop",             // 4 (loop)
    L"audio_coord_pingLoop",    // 5 (loop)
};

// ---- class-level resolves (persist across level reloads) ----
void*   g_deskCls = nullptr;
int32_t g_compOff[kCompCount] = {-1, -1, -1, -1, -1, -1};
bool    g_offsetsResolved = false;

void*   g_playFn = nullptr;       // AudioComponent:Play(StartTime)
void*   g_setSoundFn = nullptr;   // AudioComponent:SetSound(NewSound)
void*   g_setActiveFn = nullptr;  // ActorComponent:SetActive(bNewActive, bReset)
void*   g_activateFn = nullptr;   // ActorComponent:Activate(bReset)
void*   g_deactivateFn = nullptr; // ActorComponent:Deactivate() -- L6 deck stop edge
int32_t g_sigOff = -1;            // signalSound ObjectProperty (L6 -- NOT in the wire table)
int32_t g_soundOff = -1;          // AudioComponent::Sound (USoundBase*)
int32_t g_activeByteOff = -1;     // ActorComponent::bIsActive bitfield
uint8_t g_activeMask = 0;

// Failure backoff (the install-loop-bomb guard, L7 lesson): while anything is
// unresolved, retry at most 1/s -- EnsureResolved is called from ticks.
Clock::time_point g_nextResolveTry{};

// ---- instance-level cache (refreshed ONLY here / on EnsureResolved -- the
// Func-patch hot path never triggers a walk; see IndexOfComp) ----
void*   g_desk = nullptr;
int32_t g_deskIdx = -1;
void*   g_comps[kCompCount] = {};
void*   g_sigComp = nullptr;      // L6: the desk's signalSound (same lifecycle as g_comps)
bool    g_compsValid = false;

// Mirror-side cue cache: short name -> USoundBase* (validated live on use).
// A null entry = NEGATIVE cache (the name failed to resolve once; never
// re-walk per packet -- the install-loop-bomb class).
struct CueEntry { void* obj; int32_t idx; };
std::unordered_map<std::string, CueEntry> g_cueCache;

void RefreshInstanceCache() {
    g_compsValid = false;
    g_desk = nullptr;
    g_sigComp = nullptr;
    if (!g_offsetsResolved) return;
    void* d = ue_wrap::console_desk::Instance();
    if (!d) return;
    for (int i = 0; i < kCompCount; ++i) {
        void* comp = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(d) + g_compOff[i]);
        if (!comp) return;  // desk not fully constructed yet; retry next EnsureResolved
        g_comps[i] = comp;
    }
    void* sig = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(d) + g_sigOff);
    if (!sig) return;  // same not-fully-constructed retry as the whitelist comps
    for (int i = 0; i < kCompCount; ++i) {
        if (g_comps[i] == sig) {
            // 7 distinct ObjectProperties can never alias -- if they do, the
            // routing invariant (deck vs DeskSndFx) is broken: refuse the cache.
            UE_LOGW("desk_audio: signalSound aliases whitelist comp %d -- deck routing disabled", i);
            return;
        }
    }
    g_sigComp = sig;
    g_desk = d;
    g_deskIdx = R::InternalIndexOf(d);
    g_compsValid = true;
}

}  // namespace

bool EnsureResolved() {
    if (g_offsetsResolved && g_compsValid &&
        g_desk && R::IsLiveByIndex(g_desk, g_deskIdx))
        return true;

    const auto now = Clock::now();
    if (now < g_nextResolveTry) return false;
    g_nextResolveTry = now + std::chrono::seconds(1);

    if (!g_offsetsResolved) {
        if (!ue_wrap::console_desk::EnsureResolved()) return false;
        if (!g_deskCls) g_deskCls = R::FindClass(L"analogDScreenTest_C");
        if (!g_deskCls) return false;
        bool all = true;
        for (int i = 0; i < kCompCount; ++i) {
            if (g_compOff[i] < 0) g_compOff[i] = R::FindPropertyOffset(g_deskCls, kCompNames[i]);
            all = all && g_compOff[i] >= 0;
        }
        void* audioCls = R::FindClass(L"AudioComponent");
        void* actorCompCls = R::FindClass(L"ActorComponent");
        if (audioCls) {
            if (!g_playFn)     g_playFn = R::FindFunction(audioCls, L"Play");
            if (!g_setSoundFn) g_setSoundFn = R::FindFunction(audioCls, L"SetSound");
            if (g_soundOff < 0) g_soundOff = R::FindPropertyOffset(audioCls, L"Sound");
        }
        if (actorCompCls) {
            if (!g_setActiveFn) g_setActiveFn = R::FindFunction(actorCompCls, L"SetActive");
            if (!g_activateFn)  g_activateFn = R::FindFunction(actorCompCls, L"Activate");
            if (!g_deactivateFn) g_deactivateFn = R::FindFunction(actorCompCls, L"Deactivate");
            if (g_activeByteOff < 0)
                R::FindBoolProperty(actorCompCls, L"bIsActive", g_activeByteOff, g_activeMask);
        }
        if (g_sigOff < 0) g_sigOff = R::FindPropertyOffset(g_deskCls, L"signalSound");
        if (!all || !g_playFn || !g_setSoundFn || !g_setActiveFn || !g_activateFn ||
            !g_deactivateFn || g_soundOff < 0 || g_activeByteOff < 0 || g_sigOff < 0) {
            static bool s_warned = false;  // log-once: a permanently-renamed
            if (!s_warned) {               // property (game recook) must not spam 1 Hz
                s_warned = true;
                UE_LOGW("desk_audio: resolve incomplete (offs=%d play=%d setSound=%d setActive=%d "
                        "activate=%d deactivate=%d sound=%d bIsActive=%d sig=%d) -- backoff retry (log-once)",
                        all ? 1 : 0, g_playFn ? 1 : 0, g_setSoundFn ? 1 : 0, g_setActiveFn ? 1 : 0,
                        g_activateFn ? 1 : 0, g_deactivateFn ? 1 : 0, g_soundOff >= 0 ? 1 : 0,
                        g_activeByteOff >= 0 ? 1 : 0, g_sigOff >= 0 ? 1 : 0);
            }
            return false;
        }
        g_offsetsResolved = true;
        UE_LOGI("desk_audio: class-level resolve complete (6 comp offsets + signalSound@0x%X + "
                "Sound@0x%X + bIsActive@0x%X/%02X + 5 UFunctions)",
                g_sigOff, g_soundOff, g_activeByteOff, g_activeMask);
    }

    RefreshInstanceCache();
    return g_compsValid;
}

void* PlayFn()       { return g_playFn; }
void* SetActiveFn()  { return g_setActiveFn; }
void* ActivateFn()   { return g_activateFn; }
void* DeactivateFn() { return g_deactivateFn; }

bool IsSignalSound(void* comp) {
    // HOT PATH (inside the Deactivate/Activate Func-patch, game-wide): one
    // pointer compare on the miss path; liveness only on a match (the
    // IndexOfComp discipline -- a recycled address must never spoof-route).
    if (!g_compsValid || !comp || comp != g_sigComp) return false;
    if (!R::IsLiveByIndex(g_desk, g_deskIdx)) { g_compsValid = false; return false; }
    return true;
}

bool SelfTestSignalSound(bool on) {
    if (!g_compsValid || !g_sigComp) return false;
    if (!R::IsLiveByIndex(g_desk, g_deskIdx)) { g_compsValid = false; return false; }
    if (on) {
        if (!g_activateFn) return false;
        ue_wrap::ParamFrame f(g_activateFn);
        if (!f.valid()) return false;
        f.Set<bool>(L"bReset", true);  // the measured playSignal operand shape
        return ue_wrap::Call(g_sigComp, f);
    }
    if (!g_deactivateFn) return false;
    ue_wrap::ParamFrame f(g_deactivateFn);
    return f.valid() && ue_wrap::Call(g_sigComp, f);
}

int IndexOfComp(void* comp) {
    // HOT PATH (inside the Func-patch, fires for every BP Play/SetActive
    // game-wide): pure compares against the cached pointers; NEVER walks or
    // resolves. The cache refreshes only via EnsureResolved (tick-driven,
    // backoff-throttled).
    if (!g_compsValid || !comp) return -1;
    for (int i = 0; i < kCompCount; ++i) {
        if (g_comps[i] == comp) {
            // Match (rare: a desk sound) -- validate the desk is still the
            // live instance so a recycled address can never spoof-forward.
            if (!R::IsLiveByIndex(g_desk, g_deskIdx)) { g_compsValid = false; return -1; }
            return i;
        }
    }
    return -1;
}

bool ReadCueName(void* comp, char* out, int cap) {
    if (!comp || g_soundOff < 0 || !out || cap <= 1) return false;
    void* snd = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(comp) + g_soundOff);
    if (!snd) return false;
    const std::wstring w = R::ToString(R::NameOf(snd));
    if (w.empty() || static_cast<int>(w.size()) > cap - 1) return false;
    for (size_t i = 0; i < w.size(); ++i) {
        const wchar_t c = w[i];
        if (c < 0x20 || c > 0x7E) return false;  // wire carries ASCII short names only
        out[i] = static_cast<char>(c);
    }
    out[w.size()] = '\0';
    return true;
}

bool ReadLoopActive(int compIdx, bool& outActive) {
    if (compIdx < 0 || compIdx >= kCompCount || !g_compsValid || g_activeByteOff < 0) return false;
    if (!R::IsLiveByIndex(g_desk, g_deskIdx)) { g_compsValid = false; return false; }
    const uint8_t b =
        *(reinterpret_cast<uint8_t*>(g_comps[compIdx]) + g_activeByteOff);
    outActive = (b & g_activeMask) != 0;
    return true;
}

bool ReplayPlay(int compIdx, const char* cueName) {
    if (compIdx < 0 || compIdx >= kCompCount || !g_compsValid || !cueName || !cueName[0])
        return false;
    if (!R::IsLiveByIndex(g_desk, g_deskIdx)) { g_compsValid = false; return false; }
    void* comp = g_comps[compIdx];

    // Resolve the cue (cached; a FindObject walk only on first sight of a name).
    void* cue = nullptr;
    auto it = g_cueCache.find(cueName);
    if (it != g_cueCache.end()) {
        if (!it->second.obj) return false;  // negative-cached: known-unresolvable name
        if (R::IsLiveByIndex(it->second.obj, it->second.idx)) cue = it->second.obj;
    }
    if (!cue) {
        std::wstring w(cueName, cueName + std::strlen(cueName));
        cue = R::FindObject(w.c_str(), L"SoundCue");
        if (!cue) cue = R::FindObject(w.c_str(), L"SoundWave");
        g_cueCache[cueName] = {cue, cue ? R::InternalIndexOf(cue) : -1};
        if (!cue) return false;  // caller WARNs once; negative-cached above
    }

    // The exact native helper shape: SetSound(NewSound) + Play(0).
    if (g_setSoundFn) {
        ue_wrap::ParamFrame f(g_setSoundFn);
        if (f.valid()) {
            f.Set<void*>(L"NewSound", cue);
            ue_wrap::Call(comp, f);
        }
    }
    ue_wrap::ParamFrame p(g_playFn);
    if (!p.valid()) return false;
    p.Set<float>(L"StartTime", 0.0f);
    return ue_wrap::Call(comp, p);
}

bool ReplaySetActive(int compIdx, bool on) {
    if (compIdx < 0 || compIdx >= kCompCount || !g_compsValid || !g_setActiveFn) return false;
    if (!R::IsLiveByIndex(g_desk, g_deskIdx)) { g_compsValid = false; return false; }
    ue_wrap::ParamFrame f(g_setActiveFn);
    if (!f.valid()) return false;
    // ALL measured native ON sites use reset semantics (corrds_loop
    // SetActive(x,true); pingLoop Activate(true)); bReset is ignored by the
    // engine on deactivate -- static mapping, no wire bit (qf R2).
    f.Set<bool>(L"bNewActive", on);
    f.Set<bool>(L"bReset", on);
    return ue_wrap::Call(g_comps[compIdx], f);
}

void ResetCache() {
    g_compsValid = false;
    g_desk = nullptr;
    g_deskIdx = -1;
    for (auto& c : g_comps) c = nullptr;
    g_sigComp = nullptr;
    g_cueCache.clear();
    g_nextResolveTry = {};
}

}  // namespace ue_wrap::desk_audio
