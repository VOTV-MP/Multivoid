// coop/dev/signal_drill.cpp -- see coop/dev/signal_drill.h.

#include "coop/dev/signal_drill.h"

#include "coop/net/net_clock.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_component.h"
#include "ue_wrap/world/world_singleton.h"

#include <cstdint>
#include <cstring>

namespace coop::dev::signal_drill {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

struct Spec {
    const char*    name;
    const wchar_t* key;      // the signal's name, as list_signals names it
    const wchar_t* product;  // the class the desk mints for it
    const wchar_t* probe;    // a scene component of the product that moves by itself, or null
};
constexpr Spec kSpecs[] = {
    {"sigTardis", L"tardis",     L"tardis_C",     nullptr},
    {"sigTrifo",  L"trifo",      L"ufo_trifo_C",  L"ufo"},
    {"sigDuende", L"shitduende", L"shitDuende_C", nullptr},
};
// The prank names: the spawner class, the spawner's own spawn function (null for none), and the classes of what it throws.
struct Prank {
    const char*    name;
    const wchar_t* spawner;
    const wchar_t* spawnFn;
    const wchar_t* thrown[2];
};
constexpr Prank kPranks[] = {
    {"hillRoller", L"hillRollerSpawner_C", L"spawn", {L"nail_C", L"nail_ax_C"}},
    {"alienJump",  L"alienJump_C",         L"spawn", {L"prop_rock_C", nullptr}},
    {"rockThrow",  L"rockThrower_C",       nullptr,  {L"prop_rock_C", nullptr}},
};

const Spec* SpecOf(const std::string& n) {
    for (const Spec& s : kSpecs)
        if (n == s.name) return &s;
    return nullptr;
}
const Prank* PrankOf(const std::string& n) {
    for (const Prank& p : kPranks)
        if (n == p.name) return &p;
    return nullptr;
}

constexpr uint64_t kClientOwnMs = 30000, kPrankCallMs = 6000, kSayEveryMs = 5000, kSayUntilMs = 150000;
constexpr int32_t kOffSignalInDLData = 0x50;  // signal_dynamic::kOff_signal: the template key, an FName

uint64_t g_clientSeenMs = 0, g_lastSayHost = 0, g_lastSayClient = 0;
bool g_hostDone = false, g_clientOwnDone = false, g_prankCalled = false;
size_t g_clientBefore = 0;

size_t CountLive(const wchar_t* cls) {
    size_t n = 0;
    for (void* obj : R::FindObjectsByClass(cls))
        if (obj && R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__")) ++n;
    return n;
}

void* FirstLive(const wchar_t* cls) {
    for (void* obj : R::FindObjectsByClass(cls))
        if (obj && R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__")) return obj;
    return nullptr;
}

bool ReadFlag(void* obj, const wchar_t* name) {
    int32_t off = -1;
    uint8_t mask = 0;
    return obj && R::FindBoolProperty(R::ClassOf(obj), name, off, mask) && off >= 0 &&
           (*(static_cast<const uint8_t*>(obj) + off) & mask) != 0;
}

float ReadFloat(void* obj, const wchar_t* name) {
    const int32_t off = obj ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    float v = -1.f;
    if (off >= 0) std::memcpy(&v, static_cast<const uint8_t*>(obj) + off, sizeof(v));
    return v;
}

// The desk's finished download, as the game has it when autoSave runs: the download data's signal name and the detection needle at
// 100 %. Both are put back before this returns, so no later write of the desk (or a save) carries them. Returns whether autoSave ran.
bool RunAutoSave(const Spec& s, const char* who) {
    void* desk = ue_wrap::world_singleton::Find(L"analogDScreenTest_C");
    if (!desk) {
        UE_LOGW("[SIG-DRILL] [%s] %s: no live analogDScreenTest_C", who, s.name);
        return false;
    }
    void* cls = R::ClassOf(desk);
    const int32_t dataOff = R::FindPropertyOffset(cls, L"DL_SignalDownloadDLData");
    const int32_t pctOff = R::FindPropertyOffset(cls, L"DL_resDetecPercent");
    void* fn = R::FindDispatchFunctionCached(cls, L"autoSave");
    if (dataOff < 0 || pctOff < 0 || !fn) {
        UE_LOGW("[SIG-DRILL] [%s] %s: the desk's download data (%d), detection (%d) or autoSave (%p) does not resolve", who, s.name,
                dataOff, pctOff, fn);
        return false;
    }
    const R::FName key = ue_wrap::fname_utils::StringToFName(s.key);
    if (key.ComparisonIndex == 0) {
        UE_LOGW("[SIG-DRILL] [%s] %s: the name %ls did not intern", who, s.name, s.key);
        return false;
    }
    uint8_t* base = static_cast<uint8_t*>(desk);
    R::FName* keySlot = reinterpret_cast<R::FName*>(base + dataOff + kOffSignalInDLData);
    float* pct = reinterpret_cast<float*>(base + pctOff);
    const R::FName keySaved = *keySlot;
    const float pctSaved = *pct;
    *keySlot = key;
    *pct = 1.f;
    ue_wrap::ParamFrame f(fn);
    const bool ran = f.valid() && ue_wrap::Call(desk, f);
    *keySlot = keySaved;
    *pct = pctSaved;
    return ran;
}

void SayProduct(const char* who, const Spec& s, uint64_t sinceMs) {
    const unsigned t = static_cast<unsigned>(sinceMs / 1000);
    size_t n = 0;
    for (void* obj : R::FindObjectsByClass(s.product)) {
        if (!obj || !R::IsLive(obj) || R::NameStartsWith(R::NameOf(obj), L"Default__")) continue;
        ++n;
        ue_wrap::FVector at{}, comp{};
        E::TryGetActorLocation(obj, at);
        const int32_t off = s.probe ? R::FindPropertyOffset(R::ClassOf(obj), s.probe) : -1;
        void* c = off >= 0 ? *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(obj) + off) : nullptr;
        if (c && R::IsLive(c) && E::TryGetComponentLocation(c, comp))
            UE_LOGI("[SIG-DRILL] [%s] %s t=%us: %ls #%zu at (%.0f, %.0f, %.0f), %ls at (%.0f, %.0f, %.0f)", who, s.name, t, s.product, n,
                    at.X, at.Y, at.Z, s.probe, comp.X, comp.Y, comp.Z);
        else if (std::strcmp(s.name, "sigTardis") == 0)
            UE_LOGI("[SIG-DRILL] [%s] %s t=%us: %ls #%zu at (%.0f, %.0f, %.0f), delay %.1f appeared %d", who, s.name, t, s.product, n, at.X,
                    at.Y, at.Z, static_cast<double>(ReadFloat(obj, L"delay")), ReadFlag(obj, L"appeared") ? 1 : 0);
        else
            UE_LOGI("[SIG-DRILL] [%s] %s t=%us: %ls #%zu at (%.0f, %.0f, %.0f)", who, s.name, t, s.product, n, at.X, at.Y, at.Z);
    }
    UE_LOGI("[SIG-DRILL] [%s] %s t=%us: %zu %ls live", who, s.name, t, n, s.product);
}

void SayPrank(const char* who, const Prank& p, uint64_t sinceMs) {
    const unsigned t = static_cast<unsigned>(sinceMs / 1000);
    const size_t a = p.thrown[0] ? CountLive(p.thrown[0]) : 0;
    const size_t b = p.thrown[1] ? CountLive(p.thrown[1]) : 0;
    UE_LOGI("[SIG-DRILL] [%s] %s t=%us: %zu %ls live; thrown: %ls %zu, %ls %zu", who, p.name, t, CountLive(p.spawner), p.spawner,
            p.thrown[0] ? p.thrown[0] : L"-", a, p.thrown[1] ? p.thrown[1] : L"-", b);
}

bool SayDue(uint64_t sinceMs, uint64_t& last) {
    if (sinceMs > kSayUntilMs) return false;
    const uint64_t bucket = sinceMs / kSayEveryMs + 1;
    if (bucket <= last) return false;
    last = bucket;
    return true;
}

}  // namespace

bool IsSignalDrill(const std::string& n) { return SpecOf(n) != nullptr; }

void ClientStep(const std::string& name) {
    const Prank* p = PrankOf(name);
    const Spec* s = SpecOf(name);
    if (!p && !s) return;
    const uint64_t now = coop::net::NowMs();
    if (!g_clientSeenMs) g_clientSeenMs = now;
    const uint64_t since = now - g_clientSeenMs;
    if (p) {
        if (SayDue(since, g_lastSayClient)) SayPrank("C", *p, since);
        return;
    }
    if (SayDue(since, g_lastSayClient)) SayProduct("C", *s, since);
    // This peer's own try: the desk's autoSave with the signal stood, and for the TriFO spawn_trifo itself. The desk's refusal and the
    // spawn authority's must leave the product's count where the host's copy put it.
    if (!g_clientOwnDone && since >= kClientOwnMs) {
        g_clientOwnDone = true;
        g_clientBefore = CountLive(s->product);
        const bool ran = RunAutoSave(*s, "C");
        bool trifoRan = false;
        const bool isTrifo = std::strcmp(s->name, "sigTrifo") == 0;
        if (isTrifo) {
            void* desk = ue_wrap::world_singleton::Find(L"analogDScreenTest_C");
            void* fn = desk ? R::FindDispatchFunctionCached(R::ClassOf(desk), L"spawn_trifo") : nullptr;
            ue_wrap::ParamFrame f(fn);
            trifoRan = fn && f.valid() && ue_wrap::Call(desk, f);
        }
        const size_t after = CountLive(s->product);
        UE_LOGI("[SIG-DRILL] [C] %s: this peer's own autoSave called: %d%s; %ls held %zu before, %zu after (%s)", name.c_str(), ran ? 1 : 0,
                isTrifo ? (trifoRan ? ", spawn_trifo called: 1" : ", spawn_trifo called: 0") : "", s->product, g_clientBefore, after,
                after == g_clientBefore ? "refused" : "MADE ONE OF ITS OWN");
    }
}

void HostStep(const std::string& name, uint64_t since) {
    if (const Prank* p = PrankOf(name)) {
        if (SayDue(since, g_lastSayHost)) SayPrank("H", *p, since);
        if (p->spawnFn && !g_prankCalled && since >= kPrankCallMs) {
            void* spawner = FirstLive(p->spawner);
            void* fn = spawner ? R::FindDispatchFunctionCached(R::ClassOf(spawner), p->spawnFn) : nullptr;
            if (spawner) {
                g_prankCalled = true;
                ue_wrap::ParamFrame f(fn);
                const bool ran = fn && f.valid() && ue_wrap::Call(spawner, f);
                ue_wrap::FVector at{};
                E::TryGetActorLocation(spawner, at);
                UE_LOGI("[SIG-DRILL] [H] %s: the spawner's own %ls called: %d (spawner %p at (%.0f, %.0f, %.0f))", name.c_str(), p->spawnFn,
                        ran ? 1 : 0, spawner, at.X, at.Y, at.Z);
            }
        }
        return;
    }
    const Spec* s = SpecOf(name);
    if (!s) return;
    if (SayDue(since, g_lastSayHost)) SayProduct("H", *s, since);
    if (!g_hostDone && since >= 8000) {
        g_hostDone = true;
        // The drill restarts when a client rejoins: a product the host still holds from before is what the joiner is owed.
        if (CountLive(s->product) > 0) {
            UE_LOGI("[SIG-DRILL] [H] %s: the host already holds %zu %ls from before, so the signal is not run again", name.c_str(),
                    CountLive(s->product), s->product);
            return;
        }
        const bool ran = RunAutoSave(*s, "H");
        UE_LOGI("[SIG-DRILL] [H] %s: the desk's autoSave called with the %ls signal stood: %d; %zu %ls live", name.c_str(), s->key,
                ran ? 1 : 0, CountLive(s->product), s->product);
    }
}

void OnDisconnect() {
    g_clientSeenMs = g_lastSayHost = g_lastSayClient = 0;
    g_hostDone = g_clientOwnDone = g_prankCalled = false;
    g_clientBefore = 0;
}

}  // namespace coop::dev::signal_drill
