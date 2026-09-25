// ue_wrap/world/world_singleton.cpp -- the world's one-of-a-kind objects, found through the object
// index. See world_singleton.h.
#include "ue_wrap/world/world_singleton.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/engine/world_identity.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ue_wrap::world_singleton {
namespace {

namespace R = reflection;
namespace P = profile;

constexpr int32_t kRF_ClassDefaultObject = 0x10;  // EObjectFlags (UE4.27), at UObject_ObjectFlags

// An actor belongs to a world, and a departed world's actors stay live until the purge; anything else
// (the game instance, a class, an asset) outlives worlds. Asked once per class.
bool IsActorClass(void* cls) {
    static void* s_actor = nullptr;
    if (!s_actor) s_actor = object_index::ClassByName(P::name::ActorClass);
    return s_actor && R::IsDescendantOfAny(cls, &s_actor, 1);
}

struct Pick {
    CachedObjRef* ref;
    bool actor;
    void* world;  // the running world, or null when it cannot be told (boot, mid-travel)
};

// Into `ref`, the newest instance of the loaded class named `className` that is live and readable (not
// marked for death, not still being loaded or constructed), not its default object, and, when it has
// a world, of the running one: a departed world's actor stays unmarked until the purge, and one whose
// level has already lost its world stamps no world at all, so an actor must stamp the running one.
// With no running world to judge against, the stamp only has to exist, the rule a held reference
// keeps. Null, with the ref reset, when the class is not loaded or has no such instance.
void* Resolve(const wchar_t* className, CachedObjRef& ref) {
    ref.Reset();
    void* cls = object_index::ClassByName(className);
    if (!cls) return nullptr;
    Pick pick{&ref, IsActorClass(cls), world_identity::CurrentWorld()};
    object_index::ForEachInstance(cls, [](void* ctx, void* obj, int32_t index) {
        Pick& p = *static_cast<Pick*>(ctx);
        if (p.ref->Raw()) return;
        if (R::SlotFlags(index) & (R::slot_flags::Dying | R::slot_flags::NotYetReadable)) return;
        if (*reinterpret_cast<const int32_t*>(static_cast<const uint8_t*>(obj) + P::off::UObject_ObjectFlags) &
            kRF_ClassDefaultObject)
            return;
        // Every instance that has a world is judged by it, not only an actor: the held reference rejects
        // one of another world on its first Get, so taking it would hide a later instance of the
        // running one (the menu's own UWorld read as absent behind another world's).
        void* const w = world_identity::WorldOf(obj);
        if (p.actor && !w) return;
        if (w && p.world && w != p.world) return;
        p.ref->Set(obj);
    }, &pick);
    return ref.Get();
}

void* Cached(CachedObjRef& ref, const wchar_t* className) {
    if (void* live = ref.Get()) return live;
    return Resolve(className, ref);
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
