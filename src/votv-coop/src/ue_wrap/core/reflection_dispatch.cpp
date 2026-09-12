// ue_wrap/core/reflection_dispatch.cpp -- which function body an INSTANCE of a class runs.
//
// Its own translation unit because it is its own question. reflection.cpp answers "what does this
// class declare"; this answers "what would this object dispatch", and the two differ exactly where
// a hook or a reflected call goes wrong: a class that declares nothing resolves to null through
// the first and to its parent's body through the second. Everything here is built on the public
// reflection surface (FindFunction, SuperStructOf, IsLive), so it shares no state with that file.

#include "ue_wrap/core/reflection.h"

#include "ue_wrap/core/log.h"

#include <string>
#include <unordered_map>

namespace ue_wrap::reflection {

void* FindDispatchFunction(void* cls, const wchar_t* funcName, void** outDeclaringClass) {
    if (outDeclaringClass) *outDeclaringClass = nullptr;
    if (!cls || !funcName) return nullptr;
    // Bounded at 16 hops, the same number IsDescendantOfAny defaults to: a cyclic SuperStruct
    // would otherwise hang the walk. The deepest chain in this cook is 6, so the bound is not
    // reachable by a real class; it is logged once if it ever is, because silently returning
    // null there would read as "the name is declared nowhere", which is a different answer.
    int hops = 0;
    for (; hops < 16 && cls; ++hops) {
        if (void* fn = FindFunction(cls, funcName)) {
            if (outDeclaringClass) *outDeclaringClass = cls;
            return fn;
        }
        cls = SuperStructOf(cls);
    }
    if (hops == 16) {
        static bool s_warned = false;
        if (!s_warned) {
            s_warned = true;
            UE_LOGW("reflection: FindDispatchFunction('%ls') hit the 16-hop bound -- the answer is "
                    "'not found within the bound', not 'not declared'", funcName);
        }
    }
    return nullptr;
}

namespace {

// The resolve costs one object-array walk per hop, so a path that asks per call caches it. Keyed
// on the UClass and the name text -- not the literal's address, which would give two callers
// passing the same words two entries and one caller passing a built string a new entry per call.
//
// A cached pair is revalidated before use rather than stamped with a world generation: a package
// unloaded between worlds takes its UClass and its UFunctions with it, and both slot checks are
// O(1) and safe on a freed pointer. Game thread only, like every caller of it.
std::unordered_map<void*, std::unordered_map<std::wstring, void*>> g_cache;

}  // namespace

void* FindDispatchFunctionCached(void* cls, const wchar_t* funcName) {
    if (!cls || !funcName) return nullptr;
    auto& byName = g_cache[cls];
    const auto it = byName.find(funcName);
    if (it != byName.end()) {
        if (it->second && IsLive(cls) && IsLive(it->second)) return it->second;
        byName.erase(it);  // the package went away with its world; resolve against the new one
    }
    void* fn = FindDispatchFunction(cls, funcName, nullptr);
    byName[funcName] = fn;  // a miss is cached too: it costs a full walk to re-learn
    return fn;
}

}  // namespace ue_wrap::reflection
