// ue_wrap/core/class_resolve.cpp -- see class_resolve.h.
#include "ue_wrap/core/class_resolve.h"

#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/world_identity.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <deque>
#include <unordered_map>
#include <unordered_set>

namespace ue_wrap::class_resolve {
namespace {

namespace R  = ue_wrap::reflection;
namespace OI = ue_wrap::object_index;
namespace WI = ue_wrap::world_identity;
namespace P  = ue_wrap::profile;

constexpr const wchar_t* kScanRoot = L"/Game";
constexpr const wchar_t* kLoadRoot = L"/Game/objects/";  // the only place a class is loaded from
constexpr double kBudgetMs = 8.0;                         // load time per tick, once one load has run
constexpr size_t kNameCap = 256;                          // names kept in each of the capped sets
constexpr int kDrainRounds = 8;                           // a load's new objects reach the index through its drain
constexpr int kAncestorHops = 16;                         // the deepest Blueprint chain the family walk follows

enum class MapState { NotBuilt, Built, Unavailable };

// A set of names that keeps the newest kNameCap: a flood of distinct names cannot grow it, and drops its oldest.
class NameSet {
public:
    bool Has(const std::wstring& k) const { return set_.count(k) != 0; }
    void Add(const std::wstring& k) {
        if (!set_.insert(k).second) return;
        order_.push_back(k);
        if (order_.size() > kNameCap) {
            set_.erase(order_.front());
            order_.pop_front();
        }
    }
    void Clear() { set_.clear(); order_.clear(); }

private:
    std::unordered_set<std::wstring> set_;
    std::deque<std::wstring> order_;
};

struct Asset {
    std::wstring path;    // "<package>.<asset>"
    std::wstring other;   // a second Blueprint of the same name under the load root: the name is ambiguous
    std::wstring parent;  // the ParentClass tag's class name, lower case with its `_c` for a Blueprint; empty: not read
    bool parentNative = false;  // the parent is a native class (its path starts /Script/)
};

struct State {
    uint32_t worldGen = 0;
    bool registryStep = true;
    MapState map = MapState::NotBuilt;
    bool loaderSaid = false;
    double spentMs = 0;
    int loadsThisTick = 0;
    uint64_t loadsRun = 0;
    uint64_t loadsDuringReplay = 0;
    bool (*inReplay)() = nullptr;
    std::unordered_map<std::wstring, void*> found;   // "<name>|<family>" lower-cased -> its class, re-checked vs the index
    NameSet absent;
    NameSet refused;     // "<name>|<family>": refused before or after a load of it
    NameSet declined;    // a bare name a loading ask was answered Refused or Absent for: what a drill reads
    NameSet refusalSaid; // a name an already-listed class was refused for, said once
    std::unordered_map<std::wstring, Asset> assetOf;  // lower-cased asset name -> its Blueprint under kLoadRoot
};
State g_s;

double NowMs() {
    static const double perMs = [] { LARGE_INTEGER f{}; ::QueryPerformanceFrequency(&f); return f.QuadPart / 1000.0; }();
    LARGE_INTEGER c{};
    ::QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / perMs;
}

std::wstring Lower(const std::wstring& s) {
    std::wstring o = s;
    for (wchar_t& c : o)
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    return o;
}

bool EndsWithC(const std::wstring& n) { return n.size() > 2 && n.compare(n.size() - 2, 2, L"_C") == 0; }

// Classes and refusals belong to a world: a level change can unload a Blueprint class. The registry map does not:
// the cooked registry is the same for the whole process; a scan that failed is tried again in the next world.
void SyncWorld() {
    (void)WI::CurrentWorld();
    const uint32_t gen = WI::Generation();
    if (gen == g_s.worldGen) return;
    g_s.worldGen = gen;
    g_s.found.clear();
    g_s.absent.Clear();
    g_s.refused.Clear();
    g_s.declined.Clear();
    g_s.refusalSaid.Clear();
    if (g_s.map == MapState::Unavailable) g_s.map = MapState::NotBuilt;
}

// "Class'/Game/objects/prop.prop_C'" or a bare path: the class name after the last dot, and whether the path is native.
bool ParseParent(const std::wstring& text, std::wstring& name, bool& native) {
    if (text.empty()) return false;
    size_t from = text.find(L'\'');
    size_t to = text.rfind(L'\'');
    const std::wstring path = (from != std::wstring::npos && to != from) ? text.substr(from + 1, to - from - 1) : text;
    const size_t dot = path.rfind(L'.');
    if (dot == std::wstring::npos || dot + 1 >= path.size()) return false;
    name = Lower(path.substr(dot + 1));
    native = path.compare(0, 8, L"/Script/") == 0;
    return true;
}

// One scan of every asset under /Game, keeping the Blueprints under the load root by asset name, with each one's
// ParentClass tag (read through AssetRegistryHelpers::GetTagValue). The registry object comes from the helper
// library's GetAssetRegistry (a TScriptInterface, whose first word is the object); the query is
// IAssetRegistry::GetAssetsByPath, a UFunction of the interface class.
bool BuildMap() {
    const double t0 = NowMs();
    void* helpers = R::FindClassDefaultObject(L"AssetRegistryHelpers");
    void* getRegistry = helpers ? R::FindFunction(R::ClassOf(helpers), L"GetAssetRegistry") : nullptr;
    void* getTag = helpers ? R::FindFunction(R::ClassOf(helpers), L"GetTagValue") : nullptr;
    void* iface = R::FindClass(L"AssetRegistry");
    void* query = iface ? R::FindFunction(iface, L"GetAssetsByPath") : nullptr;
    void* assetData = R::FindObject(L"AssetData", L"ScriptStruct");
    if (!getRegistry || !query || !assetData) {
        UE_LOGW("class_resolve: the asset registry is not reachable (helpers=%p GetAssetRegistry=%p "
                "GetAssetsByPath=%p AssetData=%p) -- no class is loaded on demand",
                helpers, getRegistry, query, assetData);
        return false;
    }
    void* registry = nullptr;
    {
        ParamFrame f(getRegistry);
        struct ScriptInterface { void* obj; void* iface; };
        if (f.valid() && Call(helpers, f)) registry = f.Get<ScriptInterface>(L"ReturnValue").obj;
    }
    if (!registry || !R::IsLive(registry)) {
        UE_LOGW("class_resolve: GetAssetRegistry returned no live object -- no class is loaded on demand");
        return false;
    }
    int32_t offPkg = -1, offAsset = -1, offClass = -1;
    for (const auto& fld : R::EnumerateStructFields(assetData)) {
        if (fld.name == L"PackageName") offPkg = fld.offset;
        else if (fld.name == L"AssetName") offAsset = fld.offset;
        else if (fld.name == L"AssetClass") offClass = fld.offset;
    }
    const int32_t stride = R::StructSize(assetData);
    if (stride <= 0 || offPkg < 0 || offAsset < 0 || offClass < 0) {
        UE_LOGW("class_resolve: the AssetData layout is not readable (size=%d package@%d name@%d class@%d)", stride,
                offPkg, offAsset, offClass);
        return false;
    }

    ParamFrame f(query);
    if (!f.valid()) return false;
    f.Set<R::FName>(L"PackagePath", fname_utils::StringToFName(kScanRoot));
    f.Set<uint8_t>(L"bRecursive", 1);
    f.Set<uint8_t>(L"bIncludeOnlyOnDiskAssets", 1);
    if (!Call(registry, f)) {
        UE_LOGW("class_resolve: GetAssetsByPath dispatch failed");
        return false;
    }
    struct RawArray { uint8_t* data; int32_t num; int32_t max; };
    const RawArray arr = f.Get<RawArray>(L"OutAssetData");
    if (!arr.data || arr.num <= 0) {
        UE_LOGW("class_resolve: GetAssetsByPath('%ls') returned no assets", kScanRoot);
        return false;
    }

    // GetTagValue(AssetData, TagName, out FString) -> bool: the frame is built once and its three parameters found by
    // their shape (the struct, the 8-byte name, the 16-byte out string), so a renamed parameter does not break it.
    std::wstring pAsset, pTag, pOut;
    if (getTag) {
        for (const auto& p : R::FunctionParams(getTag)) {
            if (p.flags & P::cpf::ReturnParm) continue;
            if (p.size == stride && pAsset.empty()) pAsset = p.name;
            else if ((p.flags & P::cpf::OutParm) && p.size == 16 && pOut.empty()) pOut = p.name;
            else if (p.size == 8 && pTag.empty()) pTag = p.name;
        }
    }
    ParamFrame tagFrame(getTag ? getTag : query);  // a frame is built either way; canTag says whether it is the tag call
    const bool canTag = getTag && tagFrame.valid() && !pAsset.empty() && !pTag.empty() && !pOut.empty();
    if (canTag) tagFrame.Set<R::FName>(pTag.c_str(), fname_utils::StringToFName(L"ParentClass"));
    const int32_t outOff = canTag ? tagFrame.ParamOffset(pOut.c_str()) : -1;

    // The asset class is an FName and there are a few dozen distinct ones: its text is read once per name index.
    // The package is read only for a Blueprint, and the name only for one under the load root.
    std::unordered_map<int32_t, bool> isBlueprint;
    const size_t rootLen = std::wcslen(kLoadRoot);
    size_t kept = 0, ambiguous = 0, tagged = 0;
    for (int32_t i = 0; i < arr.num; ++i) {
        const uint8_t* row = arr.data + static_cast<size_t>(i) * static_cast<size_t>(stride);
        R::FName cls{};
        std::memcpy(&cls, row + offClass, sizeof(cls));
        auto it = isBlueprint.find(cls.ComparisonIndex);
        if (it == isBlueprint.end()) it = isBlueprint.emplace(cls.ComparisonIndex, R::NameEquals(cls, L"Blueprint")).first;
        if (!it->second) continue;
        R::FName pkg{}, asset{};
        std::memcpy(&pkg, row + offPkg, sizeof(pkg));
        std::memcpy(&asset, row + offAsset, sizeof(asset));
        const std::wstring pkgText = R::ToString(pkg);
        if (pkgText.compare(0, rootLen, kLoadRoot) != 0) continue;
        const std::wstring name = R::ToString(asset);
        const std::wstring path = pkgText + L"." + name;
        auto [pos, added] = g_s.assetOf.emplace(Lower(name), Asset{path, {}, {}, false});
        if (!added) {
            if (pos->second.other.empty()) {
                pos->second.other = path;
                ++ambiguous;
            }
            continue;
        }
        ++kept;
        if (!canTag) continue;
        tagFrame.SetRaw(pAsset.c_str(), row, stride);
        std::memset(static_cast<uint8_t*>(tagFrame.data()) + outOff, 0, 16);
        if (!Call(helpers, tagFrame)) continue;
        struct RawString { wchar_t* data; int32_t num; int32_t max; };
        RawString out{};
        std::memcpy(&out, static_cast<uint8_t*>(tagFrame.data()) + outOff, sizeof(out));
        if (out.data && out.num > 1) {
            std::wstring parent;
            bool native = false;
            if (ParseParent(std::wstring(out.data), parent, native)) {
                pos->second.parent = parent;
                pos->second.parentNative = native;
                ++tagged;
            }
        }
        if (out.data) R::EngineFree(out.data);
    }
    // The array's buffer is the engine's and goes back to the engine's allocator. Each row's tag map holds a reference
    // that is not released: the scan runs once per process, so what is left is bounded and small.
    R::EngineFree(arr.data);
    UE_LOGI("class_resolve: asset registry scan of %ls: %d assets, %zu Blueprints under %ls (%zu ambiguous names, %zu "
            "with a readable ParentClass tag%s), %.3f ms in all", kScanRoot, arr.num, kept, kLoadRoot, ambiguous, tagged,
            canTag ? "" : "; GetTagValue is not usable: the family is judged after the load only", NowMs() - t0);
    return true;
}

bool EnsureMap() {
    if (g_s.map == MapState::NotBuilt) g_s.map = BuildMap() ? MapState::Built : MapState::Unavailable;
    return g_s.map == MapState::Built;
}

bool EqualsNoCase(const std::wstring& lowerA, const wchar_t* b) {
    size_t i = 0;
    for (; i < lowerA.size() && b[i]; ++i) {
        wchar_t c = b[i];
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
        if (lowerA[i] != c) return false;
    }
    return i == lowerA.size() && b[i] == L'\0';
}

// Whether the family admits the asset, from the registry's tags alone: 1 yes, 0 no, -1 the tags cannot say (an ancestor
// the map does not hold, or a Blueprint whose tag was not read), which leaves the verdict to the class itself after the
// load. A native parent reached without meeting a base is a no.
int AdmitsAsset(const Family& family, const std::wstring& lowerAsset) {
    if (!family.bases || family.nBases == 0) return 1;
    std::wstring cur = lowerAsset + L"_c";
    for (int hop = 0; hop < kAncestorHops; ++hop) {
        for (size_t i = 0; i < family.nBases; ++i)
            if (EqualsNoCase(cur, family.bases[i])) return 1;
        const auto it = g_s.assetOf.find(cur.substr(0, cur.size() - 2));
        if (it == g_s.assetOf.end() || it->second.parent.empty()) return -1;
        if (it->second.parentNative) return 0;
        cur = it->second.parent;
        if (cur.size() < 2) return -1;
    }
    return -1;
}

// The generated class of `<package>.<asset>_C`, or null. The load returns the BlueprintGeneratedClass itself.
void* LoadClass(const std::wstring& className, const std::wstring& assetPath) {
    const double t0 = NowMs();
    void* obj = asset_load::LoadObjectByPath((assetPath + L"_C").c_str());
    const double ms = NowMs() - t0;
    g_s.spentMs += ms;
    ++g_s.loadsThisTick;
    ++g_s.loadsRun;
    const bool replay = g_s.inReplay && g_s.inReplay();
    if (replay) ++g_s.loadsDuringReplay;
    if (!obj || !R::IsLive(obj)) {
        UE_LOGW("class_resolve: loading '%ls' (%ls) returned nothing", className.c_str(), assetPath.c_str());
        return nullptr;
    }
    const std::wstring metaClass = R::ClassNameOf(obj);
    if (metaClass.size() < 5 || metaClass.compare(metaClass.size() - 5, 5, L"Class") != 0) {
        UE_LOGW("class_resolve: '%ls' loaded as a '%ls', not a class", className.c_str(), metaClass.c_str());
        return nullptr;
    }
    // The object index lists the load's new objects only after a drain (measured: not at once, listed after one). The
    // listener buffer is not the index's backlog, so the first drain is unconditional; the rounds that follow finish a
    // budgeted drain's remainder.
    OI::Drain();
    for (int i = 0; i < kDrainRounds && OI::Backlog() > 0; ++i) OI::Drain();
    UE_LOGI("class_resolve: loaded %ls in %.3f ms%s", className.c_str(), ms, replay ? " (during the save replay)" : "");
    return obj;
}

Resolved Refuse(const std::wstring& fkey) {
    g_s.refused.Add(fkey);
    return {ClassState::Refused, nullptr};
}

Resolved Missing(const std::wstring& key, const wchar_t* name, const char* why) {
    g_s.absent.Add(key);
    UE_LOGW("class_resolve: class '%ls' is absent for this world: %s", name, why);
    return {ClassState::Absent, nullptr};
}

}  // namespace

const Family& AnyFamily() {
    static const Family any{L"any", nullptr, 0, nullptr};
    return any;
}

static Resolved ResolveCore(const wchar_t* className, const Family& family, Load load) {
    if (!className || !*className) return {};
    if (game_thread::IsDefinitelyOffGameThread()) {
        UE_ASSERT_GAME_THREAD("class_resolve::ResolveClass");
        void* c = OI::ClassByName(className);
        return {c ? ClassState::Found : ClassState::Absent, c};
    }
    SyncWorld();
    const std::wstring name(className);
    const std::wstring key = Lower(name);
    const std::wstring fkey = key + L"|" + family.label;

    if (g_s.refused.Has(fkey)) return {ClassState::Refused, nullptr};

    // A class held from an earlier call is re-checked against the object index, which a collected class leaves.
    void* indexed = OI::ClassByName(className);
    if (auto it = g_s.found.find(fkey); it != g_s.found.end()) {
        if (indexed == it->second) return {ClassState::Found, indexed};
        g_s.found.erase(it);
    }
    if (indexed) {
        // A class already in the index is judged by the family the first time it is handed out in this world. The
        // refusal is not remembered: a family test can say no while its own base is still resolving.
        if (family.acceptsClass && !family.acceptsClass(indexed)) {
            if (!g_s.refusalSaid.Has(fkey)) {
                g_s.refusalSaid.Add(fkey);
                UE_LOGW("class_resolve: class '%ls' is listed here but is not a %ls class -- refused", className,
                        family.label);
            }
            return {ClassState::Refused, nullptr};
        }
        g_s.found[fkey] = indexed;
        return {ClassState::Found, indexed};
    }
    // A host asking about a class a client named, or a drill's red arm: the index alone, as the receive sites
    // looked a class up before the resolver existed.
    if (load == Load::LookupOnly) return {ClassState::Absent, nullptr};
    if (!g_s.registryStep) {
        void* c = R::FindClass(className);
        if (c) g_s.found[fkey] = c;
        return {c ? ClassState::Found : ClassState::Absent, c};
    }
    if (g_s.absent.Has(key)) return {ClassState::Absent, nullptr};

    // A name the registry does not hold may still be a class this process has (a native class, or one a pak
    // brought): the index can trail it, so one whole-array lookup per name per world answers before it is absent.
    const bool blueprint = EndsWithC(name) && EnsureMap();
    const std::wstring assetKey = blueprint ? key.substr(0, key.size() - 2) : std::wstring();
    const auto asset = blueprint ? g_s.assetOf.find(assetKey) : g_s.assetOf.end();
    if (asset == g_s.assetOf.end()) {
        if (void* c = R::FindClass(className)) {
            // A class the whole-array lookup finds while the index trails is judged like an indexed one.
            if (family.acceptsClass && !family.acceptsClass(c)) return {ClassState::Refused, nullptr};
            g_s.found[fkey] = c;
            return {ClassState::Found, c};
        }
        return Missing(key, className, blueprint ? "it is not a Blueprint under /Game/objects/" :
                       g_s.map == MapState::Unavailable ? "the asset registry is unreachable" : "it is a native class that is not loaded");
    }

    if (!asset->second.other.empty()) {
        UE_LOGW("class_resolve: class '%ls' names two Blueprints (%ls and %ls) -- refused", className,
                asset->second.path.c_str(), asset->second.other.c_str());
        return Refuse(fkey);
    }
    // The family, before any load, from the registry's own tags (MTA validates a model id before it builds the entity).
    if (AdmitsAsset(family, assetKey) == 0) {
        UE_LOGW("class_resolve: class '%ls' descends from no %ls base by the registry's tags -- refused, not loaded",
                className, family.label);
        return Refuse(fkey);
    }
    if (!asset_load::Available()) {
        if (!g_s.loaderSaid) {
            g_s.loaderSaid = true;
            UE_LOGE("class_resolve: the loader is unavailable (the game's RyRuntime plugin is absent): no class is "
                    "loaded on demand");
        }
        return {ClassState::Absent, nullptr};
    }
    if (load == Load::Budgeted && g_s.loadsThisTick > 0 && g_s.spentMs >= kBudgetMs) return {ClassState::Pending, nullptr};

    void* cls = LoadClass(name, asset->second.path);
    if (!cls) return Missing(key, className, "its asset did not load");
    if (family.acceptsClass && !family.acceptsClass(cls)) {
        UE_LOGW("class_resolve: loaded class '%ls' is not a %ls class -- refused", className, family.label);
        return Refuse(fkey);
    }
    g_s.found[fkey] = cls;
    return {ClassState::Found, cls};
}

Resolved ResolveClass(const wchar_t* className, const Family& family, Load load) {
    const Resolved r = ResolveCore(className, family, load);
    // A refusal is the cold path, so the note costs a found class nothing.
    if ((r.state == ClassState::Refused || r.state == ClassState::Absent) && load != Load::LookupOnly && className)
        g_s.declined.Add(Lower(className));
    return r;
}

bool WasDeclined(const wchar_t* className) {
    return className && g_s.declined.Has(Lower(className));
}

// The world is synced by the resolve that needs it, so an idle frame costs two stores.
void Tick() {
    g_s.spentMs = 0;
    g_s.loadsThisTick = 0;
}

bool LoadBudgetSpent() { return g_s.loadsThisTick > 0 && g_s.spentMs >= kBudgetMs; }

uint64_t LoadsRun() { return g_s.loadsRun; }
uint64_t LoadsDuringReplay() { return g_s.loadsDuringReplay; }
void SetReplayProbe(bool (*inReplay)()) { g_s.inReplay = inReplay; }

std::vector<std::wstring> RegistryClassNames(const wchar_t* prefix) {
    std::vector<std::wstring> out;
    if (!prefix || game_thread::IsDefinitelyOffGameThread() || !EnsureMap()) return out;
    const std::wstring want = Lower(prefix);
    for (const auto& kv : g_s.assetOf) {
        if (kv.first.compare(0, want.size(), want) != 0 || !kv.second.other.empty()) continue;
        const size_t dot = kv.second.path.rfind(L'.');
        out.push_back(kv.second.path.substr(dot + 1) + L"_C");
    }
    std::sort(out.begin(), out.end());
    return out;
}

void SetRegistryStep(bool on) { g_s.registryStep = on; }

}  // namespace ue_wrap::class_resolve
