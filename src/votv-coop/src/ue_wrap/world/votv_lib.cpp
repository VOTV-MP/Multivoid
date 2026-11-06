// ue_wrap/world/votv_lib.cpp -- see header.

#include "ue_wrap/world/votv_lib.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/ftext_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

namespace ue_wrap::votv_lib {
namespace {

namespace R = ue_wrap::reflection;

// lib_C is a BlueprintFunctionLibrary -- static BP calls dispatch on its CDO. The default object is held by slot and
// serial, so a lib_C loaded again is found again rather than served dead; a class whose default object did not read
// is not walked for it again. Each function comes through the memoised lookup, a miss served as a miss while its
// class lives, so one that stops resolving costs its own callers alone and nothing per call.
ue_wrap::CachedObjRef g_libCdo;
ue_wrap::CachedObjRef g_cdoMissFor;  // the lib_C class whose default object did not read

void* Cdo() {
    if (g_libCdo.Alive()) return g_libCdo.Raw();
    void* cls = R::FindClass(L"lib_C");
    if (!cls || g_cdoMissFor.Is(cls)) return nullptr;  // not loaded yet (menu), or this class's did not read
    g_libCdo.Set(R::FindClassDefaultObject(L"lib_C"));
    if (!g_libCdo.Alive()) {
        g_cdoMissFor.Set(cls);
        UE_LOGW("votv_lib: the lib_C class %p has no live default object -- its calls fail until it loads again", cls);
        return nullptr;
    }
    UE_LOGI("votv_lib: resolved the lib_C default object (%p)", g_libCdo.Raw());
    return g_libCdo.Raw();
}

void* Fn(const wchar_t* name) {
    void* cdo = Cdo();
    return cdo ? R::FindDispatchFunctionCached(R::ClassOf(cdo), name) : nullptr;
}

}  // namespace

bool CharacterStep(void* character, float volume) {
    void* fn = character ? Fn(L"step") : nullptr;
    if (!fn) return false;
    // Signature: step(ACharacter* Character, float Z_offset, AActor* callActor, float Volume, float
    // Pitch, float speedVolume, UAudioComponent* AudioComponent, UObject* __WorldContext,
    // FHitResult& OutHit). Volume is the caller's -- a puppet's steps are presentation and read too
    // loud at the native 1.0 -- and callActor is us, where the local player's own call passes null.
    // lib_C::step tests callActor for int_objects and calls its `stepped`; mainPlayer_C implements
    // the interface with an empty event, so ours costs one dispatch and does nothing.
    ue_wrap::ParamFrame f(fn);
    f.Set<void*>(L"Character", character);
    f.Set<float>(L"Z_offset", 0.f);
    f.Set<void*>(L"callActor", character);
    f.Set<float>(L"Volume", volume);
    f.Set<float>(L"Pitch", 1.0f);
    f.Set<float>(L"speedVolume", 400.0f);
    f.Set<void*>(L"AudioComponent", nullptr);
    f.Set<void*>(L"__WorldContext", character);
    // OutHit stays zeroed in the frame; the BP writes it, we ignore it.
    return ue_wrap::Call(g_libCdo.Raw(), f);
}

bool AddGloss(const std::wstring& name, int32_t level, void* worldContext) {
    void* fn = !name.empty() && worldContext ? Fn(L"addGloss") : nullptr;
    if (!fn) return false;
    const R::FName fname = ue_wrap::fname_utils::StringToFName(name);
    if (fname.ComparisonIndex == 0) return false;
    ue_wrap::ParamFrame f(fn);
    return f.valid() && f.Set<R::FName>(L"name", fname) && f.Set<int32_t>(L"level", level) &&
           f.Set<void*>(L"__WorldContext", worldContext) && ue_wrap::Call(g_libCdo.Raw(), f);
}

bool AddHint(const std::wstring& text, uint8_t type, void* worldContext) {
    void* fn = !text.empty() && worldContext ? Fn(L"addHint") : nullptr;
    if (!fn) return false;
    uint8_t ftext[ue_wrap::ftext_utils::kFTextSize] = {};
    if (!ue_wrap::ftext_utils::MintFText(text.c_str(), ftext)) return false;
    ue_wrap::ParamFrame f(fn);
    // `debug` is false, as every caller in the game passes it (lib.cpp's addHint returns at once for a true one
    // outside the editor).
    return f.valid() && f.SetRaw(L"InText", ftext, ue_wrap::ftext_utils::kFTextSize) &&
           f.Set<uint8_t>(L"type", type) && f.Set<bool>(L"debug", false) &&
           f.Set<void*>(L"__WorldContext", worldContext) && ue_wrap::Call(g_libCdo.Raw(), f);
}

}  // namespace ue_wrap::votv_lib
