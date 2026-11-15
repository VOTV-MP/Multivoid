#include "ue_wrap/core/class_load.h"

#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/gc_pin.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"

#include <chrono>
#include <cwctype>
#include <unordered_map>

namespace ue_wrap::class_load {

namespace {
namespace R = ue_wrap::reflection;

struct Entry {
    GcPin    pin;             // the class this load made, rooted for the process
    void*    cls = nullptr;   // the loaded class (the pin's object, or one the engine had already rooted)
    int32_t  idx = -1;        // its slot, to tell the object from a later tenant of the address
    uint64_t failedAtMs = 0;  // 0 = no failure remembered
};

// Leaked, as every pin registry here is: a GcPin inside it can release during static destruction at DLL unload.
std::unordered_map<std::wstring, Entry>& Table() {
    static auto* t = new std::unordered_map<std::wstring, Entry>();
    return *t;
}

uint64_t NowMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

std::wstring Lower(const wchar_t* s) {
    std::wstring o(s);
    for (wchar_t& c : o) c = static_cast<wchar_t>(std::towlower(c));
    return o;
}

bool IsClassObject(void* obj) {
    const std::wstring meta = R::ClassNameOf(obj);
    return meta.size() >= 5 && meta.compare(meta.size() - 5, 5, L"Class") == 0;
}

void* Look(Lookup lookup, const wchar_t* name) { return lookup ? lookup(name) : R::FindClass(name); }

}  // namespace

bool IsLoadablePath(const std::wstring& path) {
    if (path.size() < 8 || path.size() > 200 || path.compare(0, 6, L"/Game/") != 0) return false;
    for (wchar_t c : path) {
        const bool ok = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'_' ||
                        c == L'/' || c == L'.';
        if (!ok) return false;
    }
    return path.find(L"..") == std::wstring::npos && path.find(L"//") == std::wstring::npos;
}

std::wstring ObjectPathOfClass(void* cls) {
    if (!cls) return {};
    void* const pkg = R::OuterOf(cls);
    if (!pkg) return {};
    const std::wstring pkgName = R::ToString(R::NameOf(pkg));
    if (pkgName.compare(0, 6, L"/Game/") != 0) return {};
    return pkgName + L"." + R::ToString(R::NameOf(cls));
}

void* ResolveSoftPath(const std::wstring& softPath, Lookup lookup) {
    const size_t colon = softPath.find(L':');
    const std::wstring path = colon == std::wstring::npos ? softPath : softPath.substr(0, colon);
    const size_t dot = path.rfind(L'.');
    if (dot == std::wstring::npos || dot + 1 >= path.size()) return nullptr;
    const std::wstring leaf = path.substr(dot + 1);
    return Resolve(leaf.c_str(), path.c_str(), lookup);
}

void* Resolve(const wchar_t* shortName, const wchar_t* objectPath, Lookup lookup) {
    if (!shortName || !*shortName) return nullptr;
    if (void* hit = Look(lookup, shortName)) return hit;
    if (!objectPath || !*objectPath) return nullptr;

    const std::wstring key = Lower(shortName);
    Entry& e = Table()[key];
    // A class this seam loaded and rooted that the lookup still misses (still readying, or an index not caught up):
    // the rooted object is the answer, while its slot still holds it.
    if (e.cls && R::IsLiveByIndex(e.cls, e.idx)) return e.cls;
    e.cls = nullptr;
    const uint64_t now = NowMs();
    if (e.failedAtMs != 0 && now - e.failedAtMs < kRetryAfterMs) return nullptr;
    const std::wstring path(objectPath);
    if (!IsLoadablePath(path)) {
        e.failedAtMs = now;
        UE_LOGW("class_load: refusing path '%ls' for class '%ls' (not a /Game/ content path)", objectPath, shortName);
        return nullptr;
    }
    // The object name after the last dot must be the class asked for, so a path that names another asset is not
    // loaded for this name.
    const size_t dot = path.rfind(L'.');
    if (dot == std::wstring::npos || ::_wcsicmp(path.c_str() + dot + 1, shortName) != 0) {
        e.failedAtMs = now;
        UE_LOGW("class_load: path '%ls' does not name class '%ls' -- not loaded", objectPath, shortName);
        return nullptr;
    }
    const uint64_t t0 = NowMs();
    void* const obj = asset_load::LoadObjectByPath(objectPath);
    if (!obj || !IsClassObject(obj) || ::_wcsicmp(R::ToString(R::NameOf(obj)).c_str(), shortName) != 0) {
        e.failedAtMs = NowMs();
        UE_LOGW("class_load: '%ls' did not give class '%ls' (object %p) -- not asked again for %llu ms", objectPath,
                shortName, obj, static_cast<unsigned long long>(kRetryAfterMs));
        return nullptr;
    }
    // Rooted: this class has no referrer but the load that made it. A pin refused because the engine had already
    // rooted it is fine, the class is held.
    const bool pinned = e.pin.Pin(obj);
    e.cls = obj;
    e.idx = R::InternalIndexOf(obj);
    e.failedAtMs = 0;
    // An index-backed lookup trails the engine's birth notifications; catch it up so the next name lookup answers.
    object_index::DrainAll();
    UE_LOGI("class_load: loaded class '%ls' from '%ls' in %llu ms (%s); lookup now %s", shortName, objectPath,
            static_cast<unsigned long long>(NowMs() - t0), pinned ? "rooted" : "already rooted by the engine",
            Look(lookup, shortName) ? "answers" : "still misses (the load's own object is returned)");
    return obj;
}

}  // namespace ue_wrap::class_load
