// coop/dev/classload_probe.cpp -- [dev] load-on-demand of an unloaded class. See the header.
#include "coop/dev/classload_probe.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/session/net_pump.h"
#include "coop/session/rig_verdict.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"

#define PSAPI_VERSION 2
#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace coop::dev::classload_probe {
namespace {

namespace R  = ue_wrap::reflection;
namespace OI = ue_wrap::object_index;
namespace P  = ue_wrap::profile;

// A class a fresh client is expected not to have loaded: its asset name (the class name without
// `_C`) and the class name the receive sites look up. Picked from the Blueprint dumps; the probe
// asserts each is absent and says so when one is not.
struct Target {
    const wchar_t* kind;
    const wchar_t* stem;
    const wchar_t* cls;
};
const Target kTargets[] = {
    {L"food (the #108 class)", L"prop_food_mre_AB",    L"prop_food_mre_AB_C"},
    {L"food",                  L"prop_food_snuskLoaf", L"prop_food_snuskLoaf_C"},
    {L"prop",                  L"prop_cementBag",      L"prop_cementBag_C"},
    {L"npc",                   L"npc_zombie",          L"npc_zombie_C"},
    {L"world actor",           L"ufoDropper",          L"ufoDropper_C"},
};
constexpr int kN = static_cast<int>(sizeof(kTargets) / sizeof(kTargets[0]));

// The asset registry's package-path query, tried on the registry's class and on the interface it
// implements; the names are the BlueprintCallable spellings 4.27 is known to use.
const wchar_t* const kQueryNames[] = {L"GetAssetsByPath", L"K2_GetAssetsByPath"};

struct Hit {
    std::wstring pkg;
    std::wstring asset;
    std::wstring assetClass;
    int          matches = 0;
};

bool g_done = false;

// Every prop_ and npc_ Blueprint the registry scan lists, for the cold census.
struct Cand {
    std::wstring pkg;
    std::wstring asset;
};
std::vector<Cand> g_cands;

constexpr size_t kCensusProps = 12;  // props (the foods among them) not loaded on this client
constexpr size_t kCensusNpcs = 8;    // npc_ classes not loaded on this client

size_t WorkingSetKb() {
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof(pmc);
    return ::GetProcessMemoryInfo(::GetCurrentProcess(), &pmc, sizeof(pmc)) ? pmc.WorkingSetSize / 1024 : 0;
}

double NowMs() {
    static const double perMs = [] { LARGE_INTEGER f{}; ::QueryPerformanceFrequency(&f); return f.QuadPart / 1000.0; }();
    LARGE_INTEGER c{};
    ::QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / perMs;
}

bool Loaded(const wchar_t* cls) { return OI::ClassByName(cls) != nullptr || R::FindClass(cls) != nullptr; }

// Every UFunction a class declares, one line each, so a name that fails to resolve is read from the log.
void LogFunctions(const char* label, void* cls) {
    if (!cls) return;
    int n = 0;
    for (const auto& c : R::ChildObjectsOf(cls)) {
        if (c.className != L"Function") continue;
        UE_LOGI("classload: fn %hs %ls::%ls", label, R::ToString(R::NameOf(cls)).c_str(), c.name.c_str());
        ++n;
    }
    UE_LOGI("classload: fn %hs %ls declares %d functions", label, R::ToString(R::NameOf(cls)).c_str(), n);
}

// The registry object: the helper library's GetAssetRegistry() (a TScriptInterface whose first word is
// the object), else any live instance of the implementing class.
void* FindRegistry() {
    void* cdo = R::FindClassDefaultObject(L"AssetRegistryHelpers");
    if (cdo) {
        LogFunctions("helpers", R::ClassOf(cdo));
        void* fn = R::FindFunction(R::ClassOf(cdo), L"GetAssetRegistry");
        if (fn) {
            ue_wrap::ParamFrame f(fn);
            if (f.valid() && ue_wrap::Call(cdo, f)) {
                struct ScriptInterface { void* obj; void* iface; };
                const ScriptInterface si = f.Get<ScriptInterface>(L"ReturnValue");
                if (si.obj && R::IsLive(si.obj)) {
                    UE_LOGI("classload: M1 registry via AssetRegistryHelpers::GetAssetRegistry -> %p class='%ls'",
                            si.obj, R::ClassNameOf(si.obj).c_str());
                    return si.obj;
                }
            }
            UE_LOGW("classload: M1 GetAssetRegistry returned no live object");
        } else {
            UE_LOGW("classload: M1 AssetRegistryHelpers has no GetAssetRegistry function");
        }
    } else {
        UE_LOGW("classload: M1 AssetRegistryHelpers CDO not found");
    }
    void* byClass = R::FindObjectByClass(L"AssetRegistryImpl");
    if (byClass) UE_LOGI("classload: M1 registry via FindObjectByClass(AssetRegistryImpl) -> %p", byClass);
    return byClass;
}

// Asks the registry for every asset under `path` and keeps the ones whose AssetName is a target's stem.
// False when the query could not be made at all (the log says which step).
bool QueryByPath(void* registry, const wchar_t* path, Hit (&hits)[kN]) {
    void* owners[3] = {R::ClassOf(registry), R::FindClass(L"AssetRegistry"), R::FindClass(L"AssetRegistryImpl")};
    void* fn = nullptr;
    void* owner = nullptr;
    for (void* cls : owners) {
        if (!cls) continue;
        for (const wchar_t* name : kQueryNames) {
            if (void* f = R::FindFunction(cls, name)) { fn = f; owner = cls; break; }
        }
        if (fn) break;
    }
    if (!fn) {
        for (void* cls : owners) LogFunctions("registry", cls);
        UE_LOGW("classload: M1 no GetAssetsByPath on the registry or its interface (the functions are listed above)");
        return false;
    }
    UE_LOGI("classload: M1 query function %p declared by '%ls'", fn, R::ToString(R::NameOf(owner)).c_str());

    void* assetData = R::FindObject(L"AssetData", L"ScriptStruct");
    if (!assetData) { UE_LOGW("classload: M1 the AssetData struct is not found"); return false; }
    int32_t offPkg = -1, offAsset = -1, offClass = -1;
    for (const auto& fld : R::EnumerateStructFields(assetData)) {
        if (fld.name == L"PackageName") offPkg = fld.offset;
        else if (fld.name == L"AssetName") offAsset = fld.offset;
        else if (fld.name == L"AssetClass") offClass = fld.offset;
    }
    const int32_t stride = R::StructSize(assetData);
    UE_LOGI("classload: M1 AssetData size=%d PackageName@%d AssetName@%d AssetClass@%d", stride, offPkg, offAsset,
            offClass);
    if (stride <= 0 || offPkg < 0 || offAsset < 0) return false;

    ue_wrap::ParamFrame f(fn);
    if (!f.valid()) return false;
    std::wstring arrParam;
    for (const auto& p : R::FunctionParams(fn)) {
        UE_LOGI("classload: M1 param '%ls' off=%d size=%d flags=0x%llx", p.name.c_str(), p.offset, p.size,
                static_cast<unsigned long long>(p.flags));
        if (p.flags & P::cpf::ReturnParm) continue;
        if ((p.flags & P::cpf::OutParm) && p.size == 16 && arrParam.empty()) {
            arrParam = p.name;
        } else if (p.size == 8 && p.name.find(L"Path") != std::wstring::npos) {
            f.Set<R::FName>(p.name.c_str(), ue_wrap::fname_utils::StringToFName(path));
        } else if (p.size == 1) {
            const uint8_t on = (p.name.find(L"Recursive") != std::wstring::npos ||
                                p.name.find(L"OnDisk") != std::wstring::npos) ? 1 : 0;
            f.Set<uint8_t>(p.name.c_str(), on);
        }
    }
    if (arrParam.empty()) { UE_LOGW("classload: M1 the query has no TArray out parameter"); return false; }

    const double t0 = NowMs();
    if (!ue_wrap::Call(registry, f)) { UE_LOGW("classload: M1 the query's dispatch failed"); return false; }
    const double ms = NowMs() - t0;
    struct RawArray { uint8_t* data; int32_t num; int32_t max; };
    const RawArray arr = f.Get<RawArray>(arrParam.c_str());
    UE_LOGI("classload: M1 query '%ls' (recursive) -> %d assets in %.3f ms", path, arr.num, ms);
    if (!arr.data || arr.num <= 0) return false;

    std::unordered_map<int32_t, bool> isBlueprint;
    for (int32_t i = 0; i < arr.num; ++i) {
        const uint8_t* row = arr.data + static_cast<size_t>(i) * static_cast<size_t>(stride);
        R::FName assetName{};
        std::memcpy(&assetName, row + offAsset, sizeof(assetName));
        if (offClass >= 0 && (R::NameStartsWith(assetName, L"prop_") || R::NameStartsWith(assetName, L"npc_"))) {
            R::FName cls{};
            std::memcpy(&cls, row + offClass, sizeof(cls));
            auto it = isBlueprint.find(cls.ComparisonIndex);
            if (it == isBlueprint.end()) it = isBlueprint.emplace(cls.ComparisonIndex, R::NameEquals(cls, L"Blueprint")).first;
            if (it->second) {
                R::FName pkg{};
                std::memcpy(&pkg, row + offPkg, sizeof(pkg));
                g_cands.push_back({R::ToString(pkg), R::ToString(assetName)});
            }
        }
        for (int t = 0; t < kN; ++t) {
            if (!R::NameEquals(assetName, kTargets[t].stem)) continue;
            Hit& h = hits[t];
            if (h.matches++ == 0) {
                R::FName pkg{}, cls{};
                std::memcpy(&pkg, row + offPkg, sizeof(pkg));
                if (offClass >= 0) std::memcpy(&cls, row + offClass, sizeof(cls));
                h.pkg = R::ToString(pkg);
                h.asset = kTargets[t].stem;
                if (offClass >= 0) h.assetClass = R::ToString(cls);
            }
        }
    }
    // The array's buffer is the engine's; the rows' tag maps keep a reference each that is not released here (a
    // one-shot probe).
    R::EngineFree(arr.data);
    return true;
}

// The cold census: Blueprint classes the object index does not hold, loaded one after another, each timed. The
// first ones in registry order that are not resident, up to the quotas; the working set grows with each.
void Census() {
    size_t universe[2] = {}, unloaded[2] = {};  // [0] prop_, [1] npc_
    for (const Cand& c : g_cands) {
        const int k = c.asset.compare(0, 4, L"npc_") == 0 ? 1 : 0;
        ++universe[k];
        if (!OI::ClassByName((c.asset + L"_C").c_str())) ++unloaded[k];
    }
    UE_LOGI("classload: C0 universe prop=%zu (%zu not in the index) npc=%zu (%zu not in the index)", universe[0],
            unloaded[0], universe[1], unloaded[1]);

    std::vector<double> ms;
    size_t taken[2] = {};
    const size_t quota[2] = {kCensusProps, kCensusNpcs};
    for (const Cand& c : g_cands) {
        const int k = c.asset.compare(0, 4, L"npc_") == 0 ? 1 : 0;
        if (taken[k] >= quota[k]) continue;
        const std::wstring cls = c.asset + L"_C";
        if (OI::ClassByName(cls.c_str()) || R::FindClass(cls.c_str())) continue;
        ++taken[k];
        const std::wstring path = c.pkg + L"." + c.asset + L"_C";
        const size_t ws0 = WorkingSetKb();
        const double t0 = NowMs();
        void* obj = ue_wrap::asset_load::LoadObjectByPath(path.c_str());
        const double cold = NowMs() - t0;
        const bool indexed = OI::ClassByName(cls.c_str()) != nullptr;
        OI::Drain();
        const bool afterDrain = OI::ClassByName(cls.c_str()) != nullptr;
        const size_t ws1 = WorkingSetKb();
        if (obj) ms.push_back(cold);
        UE_LOGI("classload: C0 %ls kind=%ls load=%.3f ms indexed=%d afterDrain=%d loaded=%d workingset=+%lld KB path=%ls",
                cls.c_str(), k ? L"npc" : L"prop", cold, indexed ? 1 : 0, afterDrain ? 1 : 0, obj ? 1 : 0,
                static_cast<long long>(ws1) - static_cast<long long>(ws0), path.c_str());
    }
    if (ms.empty()) {
        UE_LOGW("classload: C0 SUMMARY no class was loaded (every candidate was already in the index)");
        return;
    }
    std::sort(ms.begin(), ms.end());
    double sum = 0;
    size_t over16 = 0;
    for (const double v : ms) { sum += v; if (v > 16.0) ++over16; }
    UE_LOGI("classload: C0 SUMMARY n=%zu max=%.3f ms p50=%.3f ms p95=%.3f ms sum=%.3f ms over16=%zu", ms.size(),
            ms.back(), ms[ms.size() / 2], ms[(ms.size() * 95) / 100 < ms.size() ? (ms.size() * 95) / 100 : ms.size() - 1],
            sum, over16);
}

void Run() {
    UE_LOGI("classload: start -- %d classes", kN);
    bool preLoaded[kN] = {};
    for (int t = 0; t < kN; ++t) {
        preLoaded[t] = Loaded(kTargets[t].cls);
        UE_LOGI("classload: pre %ls kind=%ls loaded=%d%s", kTargets[t].cls, kTargets[t].kind, preLoaded[t] ? 1 : 0,
                preLoaded[t] ? " -- ALREADY LOADED, its cold load is not measured" : "");
    }

    Hit hits[kN];
    void* registry = FindRegistry();
    if (registry) QueryByPath(registry, L"/Game", hits);
    else UE_LOGW("classload: M1 no asset registry object reachable");

    for (int t = 0; t < kN; ++t) {
        const Hit& h = hits[t];
        if (h.matches > 0)
            UE_LOGI("classload: M1 %ls path=%ls.%ls assetclass=%ls matches=%d", kTargets[t].cls, h.pkg.c_str(),
                    h.asset.c_str(), h.assetClass.c_str(), h.matches);
        else
            UE_LOGI("classload: M1 %ls path=NONE", kTargets[t].cls);
    }

    for (int t = 0; t < kN; ++t) {
        const Hit& h = hits[t];
        if (h.matches == 0) continue;
        const std::wstring path = h.pkg + L"." + h.asset + L"_C";
        const bool before = Loaded(kTargets[t].cls);
        const double t0 = NowMs();
        void* cls = ue_wrap::asset_load::LoadObjectByPath(path.c_str());
        const double cold = NowMs() - t0;
        const bool indexed = OI::ClassByName(kTargets[t].cls) != nullptr;
        const bool found = R::FindClass(kTargets[t].cls) != nullptr;
        const double t1 = NowMs();
        void* again = ue_wrap::asset_load::LoadObjectByPath(path.c_str());
        const double warm = NowMs() - t1;
        OI::Drain();
        const bool indexedAfterDrain = OI::ClassByName(kTargets[t].cls) != nullptr;
        UE_LOGI("classload: M2 %ls load=%.3f ms", kTargets[t].cls, cold);
        UE_LOGI("classload: M2x %ls warm=%.3f ms loadedBefore=%d returned=%p again=%p same=%d", kTargets[t].cls,
                warm, before ? 1 : 0, cls, again, cls == again ? 1 : 0);
        UE_LOGI("classload: M3 %ls indexed=%d", kTargets[t].cls, indexed ? 1 : 0);
        UE_LOGI("classload: M3x %ls findclass=%d indexedAfterDrain=%d", kTargets[t].cls, found ? 1 : 0,
                indexedAfterDrain ? 1 : 0);
    }
    Census();
    coop::rig_verdict::Say("classload", coop::rig_verdict::Outcome::Invalid, "measured");
}

}  // namespace

void Tick() {
    static const bool armed = coop::config::ResolveFlag(::coop::config_registry::rows::classload_probe);
    if (!armed || g_done || !coop::net_pump::IsWorldSettled()) return;
    g_done = true;
    Run();
}

}  // namespace coop::dev::classload_probe
