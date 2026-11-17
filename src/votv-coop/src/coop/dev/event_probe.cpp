// coop/dev/event_probe.cpp -- [dev] what an event leaves in the world, on each peer (VOTVCOOP_EVENT_PROBE=<event>, with
// VOTVCOOP_EVENT_PROBE_CLASS=<substring>, on both peers), for Ghezra/Multivoid#39.
//
// THE HOST, 24 s after a client is connected: arms the event and drives its trigger box's own overlap with its player,
// as a walk-in does (coop/dev/event_force), or, for an event with no box, fires it as the dev event menu does; a
// "Class.function" instead runs that paramless function of the class's last live actor. BOTH
// PEERS, at 20 s, 34 s and 50 s: every live actor whose class name holds the substring (any case), with its place.

#include "coop/dev/event_probe.h"

#include "coop/config/config.h"
#include "coop/dev/event_force.h"
#include "coop/dev/event_trigger.h"
#include "coop/net/session.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <string>
#include <vector>

namespace coop::dev::event_probe {
namespace {

namespace R = ue_wrap::reflection;
namespace OI = ue_wrap::object_index;

uint64_t Now() { return ::GetTickCount64(); }

const std::string& Event() {
    static const std::string v = coop::config::ReadEnv("VOTVCOOP_EVENT_PROBE");
    return v;
}

std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

uint64_t g_startAt = 0;
int g_census = 0;
bool g_fired = false;

void Census(const char* when) {
    const std::string narrow = coop::config::ReadEnv("VOTVCOOP_EVENT_PROBE_CLASS");
    const std::wstring sub = Lower(std::wstring(narrow.begin(), narrow.end()));
    if (sub.empty()) return;
    std::vector<void*> classes;
    OI::ForEachClass([](void* ctx, void* cls, void*) { static_cast<std::vector<void*>*>(ctx)->push_back(cls); }, &classes);
    int n = 0;
    for (void* cls : classes) {
        const std::wstring name = R::ToString(R::NameOf(cls));
        if (Lower(name).find(sub) == std::wstring::npos) continue;
        struct Ctx { const std::wstring* name; int* n; const char* when; } ctx{&name, &n, when};
        OI::ForEachInstance(cls, [](void* c, void* obj, int32_t) {
            auto* x = static_cast<Ctx*>(c);
            if (!obj || !R::IsLive(obj) || R::NameStartsWith(R::NameOf(obj), L"Default__")) return;
            ue_wrap::FVector at{};
            if (!ue_wrap::engine::TryGetActorLocation(obj, at)) return;   // actors only
            ++*x->n;
            UE_LOGI("[EVPROBE] %s: %ls '%ls' at (%.0f,%.0f,%.0f)", x->when, x->name->c_str(),
                    R::ToString(R::NameOf(obj)).c_str(), at.X, at.Y, at.Z);
        }, &ctx);
    }
    UE_LOGI("[EVPROBE] %s: %d actor(s) of a class holding '%ls'", when, n, sub.c_str());
}

}  // namespace

void Tick(coop::net::Session* session) {
    if (Event().empty() || !session || !session->running() || !session->connected()) return;
    const bool host = session->role() == coop::net::Role::Host;
    if (!g_startAt) {
        g_startAt = Now();
        UE_LOGI("[EVPROBE] armed role=%s event=%s", host ? "host" : "client", Event().c_str());
    }
    const uint64_t t = Now() - g_startAt;
    static const uint64_t kCensusAt[] = {20000u, 34000u, 50000u};
    static const char* const kWhen[] = {"before", "after", "later"};
    if (g_census < 3 && t >= kCensusAt[g_census]) {
        Census(kWhen[g_census]);
        ++g_census;
    }
    if (host && !g_fired && t >= 24000u) {
        g_fired = true;
        // "Class.function" instead of an event: the host runs that function of the class's last live actor.
        const size_t dot = Event().find('.');
        if (dot != std::string::npos) {
            const std::wstring cls(Event().begin(), Event().begin() + static_cast<std::ptrdiff_t>(dot));
            const std::wstring fn(Event().begin() + static_cast<std::ptrdiff_t>(dot) + 1, Event().end());
            void* target = nullptr;
            for (void* o : R::FindObjectsByClass(cls.c_str()))
                if (o && R::IsLive(o) && !R::NameStartsWith(R::NameOf(o), L"Default__")) target = o;
            void* const f = target ? R::FindDispatchFunctionCached(R::ClassOf(target), fn.c_str()) : nullptr;
            ue_wrap::ParamFrame pf(f);
            const bool ran = f && pf.valid() && ue_wrap::Call(target, pf);
            UE_LOGI("[EVPROBE] host: %s ran=%d", Event().c_str(), ran ? 1 : 0);
            return;
        }
        bool ok = coop::dev::event_force::ForceNow(Event().c_str());
        const char* how = "forced through its box";
        if (!ok) {
            how = "fired";
            for (const auto& ev : coop::dev::event_trigger::Events())
                if (Event() == ev.name) ok = coop::dev::event_trigger::Trigger(ev);
        }
        UE_LOGI("[EVPROBE] host: '%s' %s (ok=%d)", Event().c_str(), how, ok ? 1 : 0);
    }
}

}  // namespace coop::dev::event_probe
