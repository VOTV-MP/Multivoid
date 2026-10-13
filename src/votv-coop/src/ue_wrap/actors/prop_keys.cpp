// ue_wrap/actors/prop_keys.cpp -- the keys of the keyed interactables: a prop's is a direct field, a trash
// pile's is the actor_save key field, and a chip pile's or clump's comes from the GetKey blueprint function
// through ProcessEvent, cached per class. See ue_wrap/actors/prop.h.

#include "ue_wrap/actors/prop.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/object_index.h"  // ClassByName, the trash base in one lookup
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace ue_wrap::prop {
namespace {

namespace P = profile;
namespace R = reflection;

template <typename T>
inline T ReadField(void* base, size_t off) {
    return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(base) + off);
}

constexpr size_t kAactorSaveKeyOff = 0x0230;

// The actor_save base that owns the key field GetActorSaveKeyString reads, resolved on demand
// and cached; null means not resolvable yet, never no key.
std::atomic<void*> g_actorSaveCls{nullptr};
inline void* ActorSaveCls() {
    void* c = g_actorSaveCls.load(std::memory_order_acquire);
    if (!c) {
        c = R::FindClass(L"actor_save_C");
        if (c) g_actorSaveCls.store(c, std::memory_order_release);
    }
    return c;
}

// The per-class GetKey cache, under a mutex: observers on a parallel-anim worker would race the
// game thread, and lookups are per spawn or destroy.
std::mutex g_getKeyFnMutex;
std::unordered_map<void*, void*> g_getKeyFnByClass;  // UClass* -> UFunction*

void* ResolveGetKeyFn(void* cls) {
    if (!cls) return nullptr;
    std::lock_guard<std::mutex> lk(g_getKeyFnMutex);
    auto it = g_getKeyFnByClass.find(cls);
    if (it != g_getKeyFnByClass.end()) return it->second;
    void* fn = R::FindFunction(cls, L"GetKey");
    g_getKeyFnByClass[cls] = fn;  // cache even null so we don't re-walk
    return fn;
}

R::FName CallGetKeyUFunction(void* obj) {
    void* cls = R::ClassOf(obj);
    void* fn = ResolveGetKeyFn(cls);
    if (!fn) return R::FName{0, 0};
    ue_wrap::ParamFrame f(fn);
    if (!ue_wrap::Call(obj, f)) return R::FName{0, 0};
    return f.Get<R::FName>(L"Key");
}
}  // namespace

R::FName GetInteractableKey(void* obj) {
    if (!obj) return R::FName{0, 0};
    if (IsDescendantOfProp(obj)) {
        return ReadField<R::FName>(obj, P::off::Aprop_Key);
    }
    // The trash pile: a direct field.
    void* cls = R::ClassOf(obj);
    if (WalksToBase(cls, TrashBitsPileClass())) {
        return ReadField<R::FName>(obj, kAactorSaveKeyOff);
    }
    // The chip pile and the clump: the blueprint function.
    if (WalksToBase(cls, GarbageClumpClass()) || WalksToBase(cls, ChipPileClass())) {
        return CallGetKeyUFunction(obj);
    }
    return R::FName{0, 0};
}

std::wstring GetActorSaveKeyString(void* obj) {
    if (!obj) return {};
    void* base = ActorSaveCls();
    if (!base) return {};
    if (!WalksToBase(R::ClassOf(obj), base)) return {};   // NOT an actor_save: the offset is not its key
    return R::ToString(ReadField<R::FName>(obj, kAactorSaveKeyOff));
}

std::wstring GetInteractableKeyString(void* obj) {
    if (!obj) return {};
    const R::FName key = GetInteractableKey(obj);
    return R::ToString(key);
}

KeyField KeyFieldOf(void* cls) {
    if (!cls) return KeyField::None;
    // Both bases through the object index, which answers a class that is not loaded in one lookup; FindClass
    // would walk the object array on every such miss, once per class asked.
    void* prop = object_index::ClassByName(P::name::PropClass);
    if (prop && WalksToBase(cls, prop)) return KeyField::Prop;
    void* bits = object_index::ClassByName(L"trashBitsPile_C");
    return (bits && WalksToBase(cls, bits)) ? KeyField::ActorSave : KeyField::None;
}

R::FName ReadKeyField(void* obj, KeyField where) {
    if (!obj) return R::FName{0, 0};
    switch (where) {
        case KeyField::Prop:      return ReadField<R::FName>(obj, P::off::Aprop_Key);
        case KeyField::ActorSave: return ReadField<R::FName>(obj, kAactorSaveKeyOff);
        default:                  return R::FName{0, 0};
    }
}

R::FName GetKey(void* prop) {
    if (!prop) return R::FName{0, 0};
    return ReadField<R::FName>(prop, P::off::Aprop_Key);
}

std::wstring GetKeyString(void* prop) {
    if (!prop) return {};
    const R::FName key = GetKey(prop);
    return R::ToString(key);
}

}  // namespace ue_wrap::prop
