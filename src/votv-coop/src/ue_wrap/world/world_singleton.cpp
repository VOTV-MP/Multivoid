// ue_wrap/world/world_singleton.cpp -- the world's one-of-a-kind objects, found through the object
// index. See world_singleton.h.
#include "ue_wrap/world/world_singleton.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile_names.h"

#include <string>
#include <vector>

namespace ue_wrap::world_singleton {
namespace {

namespace R = reflection;

// One live instance of the loaded class named `className`, never its default object and never one
// marked for death (the index lists those until their delete drains); null when the class is not
// loaded or has no such instance.
void* Resolve(const wchar_t* className) {
    void* cls = object_index::ClassByName(className);
    if (!cls) return nullptr;
    void* found = nullptr;
    object_index::ForEachInstance(cls, [](void* ctx, void* obj, int32_t index) {
        void*& out = *static_cast<void**>(ctx);
        if (out || !R::IsLiveByIndex(obj, index)) return;
        if (!R::NameStartsWith(R::NameOf(obj), L"Default__")) out = obj;
    }, &found);
    return found;
}

void* Cached(CachedObjRef& ref, const wchar_t* className) {
    if (void* live = ref.Get()) return live;
    void* obj = Resolve(className);
    ref.Set(obj);
    return obj;
}

// The tail of named singletons, a dozen at most, looked up by name.
struct Entry {
    std::wstring name;
    CachedObjRef inst;
};
std::vector<Entry> g_entries;   // game thread

}  // namespace

void* Find(const wchar_t* className) {
    UE_ASSERT_GAME_THREAD("world_singleton::Find");
    if (!className || !*className) return nullptr;
    for (Entry& e : g_entries)
        if (e.name == className) return Cached(e.inst, className);
    g_entries.push_back(Entry{className, {}});
    return Cached(g_entries.back().inst, className);
}

void* Gamemode() {
    UE_ASSERT_GAME_THREAD("world_singleton::Gamemode");
    static CachedObjRef s_ref;
    return Cached(s_ref, profile::name::GamemodeClass);
}

void* GameInstance() {
    UE_ASSERT_GAME_THREAD("world_singleton::GameInstance");
    static CachedObjRef s_ref;
    return Cached(s_ref, profile::name::GameInstanceClass);
}

}  // namespace ue_wrap::world_singleton
