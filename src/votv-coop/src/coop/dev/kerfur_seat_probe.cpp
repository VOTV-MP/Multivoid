// coop/dev/kerfur_seat_probe.cpp -- see coop/dev/kerfur_seat_probe.h.

#include "coop/dev/kerfur_seat_probe.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"

#include "ue_wrap/core/field_io.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cstdint>
#include <string>

namespace coop::dev::kerfur_seat_probe {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace sg = ue_wrap::script_gate;

UE_FACT_FN(kRowActionName, "dev.kerfur_seat_probe", L"kerfurOmega_C", L"actionName", Declared);
UE_FACT_FN(kRowUseSelected, "dev.kerfur_seat_probe", L"mainPlayer_C", L"useSelectedAction", Declared);

constexpr int      kTagActionName = 0x4B535001;  // 'KSP' 1
constexpr int      kTagUseSelected = 0x4B535002;
constexpr uint32_t kMaxLines = 200;

std::atomic<coop::net::Session*> g_session{nullptr};

// One watch: registered with the gate, then asked each tick until it settles live or dead.
struct Watch {
    const ue_wrap::facts::Function& row;
    const char*                     label;
    int                             tag;
    sg::PreFn                       pre;
    bool                            registered;
    bool                            settled;
    uint32_t                        calls;
    uint32_t                        lines;
};

R::ParamOffset g_nameOff{L"name"};

bool Enabled() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::kerfur_seat_probe);
    return s;
}

const char* Role() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return "solo";
    return s->role() == coop::net::Role::Host ? "host" : "client";
}

std::string Narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s.push_back(c < 0x20 || c == L'"' ? ' ' : (c < 128 ? static_cast<char>(c) : '?'));
    return s;
}

std::string ObjectName(void* o) { return o && R::IsLive(o) ? Narrow(R::ToString(R::NameOf(o))) : std::string("?"); }

// "Class:name" of a live object, else "?".
std::string ClassAndName(void* o) {
    if (!o || !R::IsLive(o)) return "?";
    return Narrow(R::ClassNameOf(o)) + ":" + ObjectName(o);
}

// "Class.function" of the Blueprint frame that made the call; "none" when none did (the ProcessEvent route).
std::string Caller(const sg::Call& c) {
    if (!c.callerObject || !c.callerFunction || !R::IsLive(c.callerObject) || !R::IsLive(c.callerFunction)) return "none";
    return Narrow(R::ClassNameOf(c.callerObject)) + "." + Narrow(R::ToString(R::NameOf(c.callerFunction)));
}

Watch g_actionName{kRowActionName, "kerfurOmega_C.actionName", kTagActionName, nullptr, false, false, 0, 0};
Watch g_useSelected{kRowUseSelected, "mainPlayer_C.useSelectedAction", kTagUseSelected, nullptr, false, false, 0, 0};

// Counts the call; true while it may print, the cap said once as it is reached.
bool MayPrint(Watch& w) {
    ++w.calls;
    if (w.lines >= kMaxLines) {
        if (w.lines == kMaxLines) {
            ++w.lines;
            UE_LOGI("[KERFUR-SEAT] role=%s '%s' past %u lines -- further calls counted only", Role(), w.label,
                    kMaxLines);
        }
        return false;
    }
    ++w.lines;
    return true;
}

sg::Verdict OnActionNamePre(const sg::Call& c) {
    if (!MayPrint(g_actionName)) return sg::Verdict::Run;
    const int32_t off = g_nameOff.Of(c.function);
    const std::wstring action = off >= 0 && c.locals ? ue_wrap::field_io::ReadFStringAt(c.locals, off) : L"?";
    UE_LOGI("[KERFUR-SEAT] role=%s actionName obj=%s action=%s caller=%s ours=%d", Role(), ObjectName(c.object).c_str(),
            Narrow(action).c_str(), Caller(c).c_str(), c.fromOurCode ? 1 : 0);
    return sg::Verdict::Run;
}

sg::Verdict OnUseSelectedPre(const sg::Call& c) {
    if (!MayPrint(g_useSelected)) return sg::Verdict::Run;
    void* self = c.object;
    bool release = false;
    int32_t idx = -1;
    const bool haveSel = E::ReadMainPlayerRadialSelect(self, release, idx);
    const std::string sel = haveSel ? std::to_string(idx) : std::string("?");
    UE_LOGI("[KERFUR-SEAT] role=%s useSelectedAction self=%s local=%d looked=%s sel=%s release=%s", Role(),
            ObjectName(self).c_str(), coop::players::Registry::Get().IsLocal(self) ? 1 : 0,
            ClassAndName(E::ReadMainPlayerHitActor(self)).c_str(), sel.c_str(),
            haveSel ? (release ? "1" : "0") : "?");
    return sg::Verdict::Run;
}

// The shape of impact_damage_probe's Install: a refused registration is asked again while the gate is not armed; a
// watch whose name resolved dead is said once, with the reason.
void InstallWatch(Watch& w) {
    if (w.settled) return;
    if (!w.registered) w.registered = sg::WatchName(w.row, w.tag, w.pre, nullptr);
    if (!w.registered) {
        if (sg::IsInstalled()) {
            w.settled = true;
            UE_LOGE("[KERFUR-SEAT] watch '%s' NOT live: the gate refused the registration", w.label);
        }
        return;
    }
    sg::ResolvePendingNames();
    if (sg::NameWatchLive(w.row, w.tag)) {
        w.settled = true;
        UE_LOGI("[KERFUR-SEAT] watch '%s' live", w.label);
    } else if (sg::NameWatchSettled(w.row, w.tag)) {
        w.settled = true;
        UE_LOGE("[KERFUR-SEAT] watch '%s' NOT live: its name resolved dead (the class declares no such function, a "
                "native one, or the table is full)", w.label);
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    if (!Enabled()) return;
    g_session.store(session, std::memory_order_release);
    if (!g_actionName.pre) {
        g_actionName.pre = &OnActionNamePre;
        g_useSelected.pre = &OnUseSelectedPre;
    }
    InstallWatch(g_actionName);
    InstallWatch(g_useSelected);
}

void OnDisconnect() {
    if (!Enabled()) return;
    UE_LOGI("[KERFUR-SEAT] session end -- actionName=%u useSelectedAction=%u", g_actionName.calls, g_useSelected.calls);
    g_actionName.calls = g_actionName.lines = 0;
    g_useSelected.calls = g_useSelected.lines = 0;
}

}  // namespace coop::dev::kerfur_seat_probe
