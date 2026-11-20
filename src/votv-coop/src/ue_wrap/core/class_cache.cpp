// ue_wrap/core/class_cache.cpp -- see ue_wrap/core/class_cache.h.

#include "ue_wrap/core/class_cache.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/object_index.h"

#include <cwchar>
#include <string>
#include <vector>

namespace ue_wrap::class_cache {
namespace {

struct Entry {
    std::wstring name;
    CachedObjRef cls;
};
std::vector<Entry> g_entries;   // bounded by the distinct names callers ask; game thread

}  // namespace

void* ByName(const wchar_t* name) {
    UE_ASSERT_GAME_THREAD("class_cache::ByName");
    if (!name) return nullptr;
    for (Entry& e : g_entries) {
        if (::_wcsicmp(e.name.c_str(), name) != 0) continue;
        if (!e.cls.Alive()) e.cls.Set(object_index::ClassByName(name));
        return e.cls.Raw();
    }
    g_entries.push_back({name, {}});
    g_entries.back().cls.Set(object_index::ClassByName(name));
    return g_entries.back().cls.Raw();
}

}  // namespace ue_wrap::class_cache
