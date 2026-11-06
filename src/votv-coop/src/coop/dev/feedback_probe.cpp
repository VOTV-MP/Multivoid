// coop/dev/feedback_probe.cpp -- see coop/dev/feedback_probe.h.

#include "coop/dev/feedback_probe.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/net_clock.h"
#include "coop/net/session.h"

#include "ue_wrap/core/ftext_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/engine/engine_audio.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cstdint>
#include <string>

namespace coop::dev::feedback_probe {
namespace {

namespace E  = ue_wrap::engine;
namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;
namespace UH = ue_wrap::ufunction_hook;

UE_FACT_FN(kRowAddHint, "dev.feedback_probe", nullptr, L"addHint", AnyClass);
UE_FACT_FN(kRowPlaySound2D, "dev.feedback_probe", L"GameplayStatics", L"PlaySound2D", Declared);

constexpr int      kTagHint   = 0x46424B01;  // 'FBK' 1
constexpr uint32_t kLinesPerS = 20;
constexpr uint64_t kWindowMs  = 1000;

std::atomic<coop::net::Session*> g_session{nullptr};

bool g_hintRegistered = false;
bool g_hintSettled = false;
bool g_soundHooked = false;
bool g_soundRefused = false;
void* g_soundFn = nullptr;
R::ParamOffset g_textOff{L"InText"};
int32_t g_soundOff = -1;

// The line cap's window: lines printed in it, and calls past the cap.
uint64_t g_windowStartMs = 0;
uint32_t g_windowLines = 0;
uint32_t g_windowDropped = 0;

bool Enabled() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::feedback_probe);
    return s;
}

const char* Role() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return "solo";
    return s->role() == coop::net::Role::Host ? "host" : "client";
}

void SayDropped() {
    if (g_windowDropped == 0) return;
    UE_LOGI("[FEEDBACK-PROBE] role=%s dropped %u lines past the cap of %u a second", Role(), g_windowDropped,
            kLinesPerS);
    g_windowDropped = 0;
}

// Whether this call may print: a second's window with a cap, the drops of the last one said as the next opens.
bool MayPrint() {
    const uint64_t now = coop::net::NowMs();
    if (now - g_windowStartMs >= kWindowMs) {
        SayDropped();
        g_windowStartMs = now;
        g_windowLines = 0;
    }
    if (g_windowLines >= kLinesPerS) {
        ++g_windowDropped;
        return false;
    }
    ++g_windowLines;
    return true;
}

// A log-safe text: control characters (a hint's newlines) as spaces, quotes as apostrophes.
std::wstring Clean(std::wstring s) {
    for (wchar_t& c : s)
        if (c < 0x20 || c == L'"') c = c == L'"' ? L'\'' : L' ';
    return s;
}

// "Class.function" of a UFunction, or "none".
std::wstring FunctionName(void* fn) {
    if (!fn || !R::IsLive(fn)) return L"none";
    void* owner = R::OuterOf(fn);
    const std::wstring name = R::ToString(R::NameOf(fn));
    if (!owner || !R::IsLive(owner)) return name;
    return R::ToString(R::NameOf(owner)) + L"." + name;
}

sg::Verdict OnHintPre(const sg::Call& c) {
    if (!MayPrint()) return sg::Verdict::Run;
    const int32_t o = g_textOff.Of(c.function);
    const std::wstring text = o >= 0 && c.locals ? ue_wrap::ftext_utils::FTextToString(c.locals + o) : L"?";
    UE_LOGI("[FEEDBACK-PROBE] role=%s hint \"%ls\" from=%ls", Role(), Clean(text).c_str(),
            FunctionName(sg::OutermostFunction(c)).c_str());
    return sg::Verdict::Run;
}

sg::Verdict OnSound2DPre(void*, void*, const uint8_t* parms) {
    if (!MayPrint()) return sg::Verdict::Run;
    void* sound = g_soundOff >= 0 ? *reinterpret_cast<void* const*>(parms + g_soundOff) : nullptr;
    std::wstring name = sound && R::IsLive(sound) ? E::SoundName(sound) : std::wstring();
    if (name.empty()) name = sound && R::IsLive(sound) ? R::ToString(R::NameOf(sound)) : L"null";
    // A call through ProcessEvent has no calling frame: the frame is the called function's own.
    void* caller = UH::CurrentCallerFrame().function;
    UE_LOGI("[FEEDBACK-PROBE] role=%s sound2d \"%ls\" from=%ls", Role(), Clean(name).c_str(),
            caller == g_soundFn ? L"none" : FunctionName(caller).c_str());
    return sg::Verdict::Run;
}

void InstallHint() {
    if (g_hintSettled) return;
    if (!g_hintRegistered) g_hintRegistered = sg::WatchName(kRowAddHint, kTagHint, &OnHintPre, nullptr);
    if (!g_hintRegistered) {
        if (sg::IsInstalled()) {
            g_hintSettled = true;
            UE_LOGE("[FEEDBACK-PROBE] the gate refused the addHint watch -- no hint is logged");
        }
        return;
    }
    sg::ResolvePendingNames();
    if (sg::NameWatchLive(kRowAddHint, kTagHint)) {
        g_hintSettled = true;
        UE_LOGI("[FEEDBACK-PROBE] the addHint watch is live");
    } else if (sg::NameWatchSettled(kRowAddHint, kTagHint)) {
        g_hintSettled = true;
        UE_LOGE("[FEEDBACK-PROBE] the addHint watch is dead -- no hint is logged");
    }
}

void InstallSound() {
    if (g_soundHooked || g_soundRefused) return;
    const ue_wrap::facts::Resolved fn = ue_wrap::facts::Resolve(kRowPlaySound2D);
    if (!fn) return;  // loading: a later tick finds it
    g_soundFn = fn.Fn();
    g_soundOff = R::FindParamOffset(g_soundFn, L"Sound");
    if (g_soundOff < 0 || !UH::InstallPreHook(fn, &OnSound2DPre, /*armed=*/true)) {
        g_soundRefused = true;
        UE_LOGE("[FEEDBACK-PROBE] no pre hook on PlaySound2D (Sound offset %d) -- no sound is logged", g_soundOff);
        return;
    }
    g_soundHooked = true;
    UE_LOGI("[FEEDBACK-PROBE] the PlaySound2D hook is installed");
}

}  // namespace

void Install(coop::net::Session* session) {
    if (!Enabled()) return;
    g_session.store(session, std::memory_order_release);
    if (g_windowDropped != 0 && coop::net::NowMs() - g_windowStartMs >= kWindowMs) SayDropped();
    InstallHint();
    InstallSound();
}

void OnDisconnect() {
    if (!Enabled()) return;
    SayDropped();
    g_windowLines = 0;
}

}  // namespace coop::dev::feedback_probe
