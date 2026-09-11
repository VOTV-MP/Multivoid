// ue_wrap/world/keyed_objects.cpp -- see the header.

#include "ue_wrap/world/keyed_objects.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"

namespace ue_wrap::keyed_objects {
namespace {

namespace R = ue_wrap::reflection;
namespace P = ue_wrap::profile;

ue_wrap::CachedObjRef g_gm;
void* g_fn = nullptr;

void* Gamemode() {
    if (g_gm.Alive()) return g_gm.Raw();
    g_gm.Set(R::FindObjectByClass(P::name::GamemodeClass));
    return g_gm.Raw();
}

}  // namespace

void* Resolve(const wchar_t* key) {
    if (!key || !*key) return nullptr;
    void* gm = Gamemode();
    if (!gm) return nullptr;
    if (!g_fn) {
        g_fn = R::FindFunction(R::ClassOf(gm), L"getObjectFromKey");
        if (!g_fn) {
            static bool sSaid = false;
            if (!sSaid) {
                sSaid = true;
                UE_LOGW("keyed_objects: mainGamemode_C::getObjectFromKey unresolved -- a caller that "
                        "needs to predict what the game will attach to cannot, and must refuse "
                        "rather than guess");
            }
            return nullptr;
        }
    }
    ParamFrame f(g_fn);
    if (!f.valid()) return nullptr;
    R::FName n = ue_wrap::fname_utils::StringToFName(key);
    if (!f.SetRaw(L"ItemToFind", &n, sizeof(n))) return nullptr;
    if (!Call(gm, f)) return nullptr;
    void* out = f.Get<void*>(L"Output");
    // The game hands back whatever its map holds; a stale entry is possible after a destroy, and
    // this is the one place to catch it rather than in every caller.
    return (out && R::IsLive(out)) ? out : nullptr;
}

void ResetCache() {
    g_gm.Reset();
    g_fn = nullptr;
}

}  // namespace ue_wrap::keyed_objects
