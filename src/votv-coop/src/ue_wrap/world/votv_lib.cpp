// ue_wrap/world/votv_lib.cpp -- see header.

#include "ue_wrap/world/votv_lib.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

namespace ue_wrap::votv_lib {
namespace {

namespace R = ue_wrap::reflection;

// One-time latched resolution (no per-call GUObjectArray walks). lib_C is a
// BlueprintFunctionLibrary -- static BP calls dispatch on its CDO.
void* g_libCdo  = nullptr;
void* g_stepFn  = nullptr;

bool Resolve() {
    if (g_libCdo && g_stepFn) return true;
    void* cls = R::FindClass(L"lib_C");
    if (!cls) return false;  // not loaded yet (menu) -- retry on a later call
    if (!g_libCdo) g_libCdo = R::FindClassDefaultObject(L"lib_C");
    if (!g_stepFn) g_stepFn = R::FindFunction(cls, L"step");
    if (g_libCdo && g_stepFn) {
        UE_LOGI("votv_lib: resolved lib_C step (cdo=%p fn=%p)", g_libCdo, g_stepFn);
        return true;
    }
    return false;
}

}  // namespace

bool CharacterStep(void* character, float volume) {
    if (!character || !Resolve()) return false;
    // Signature: step(ACharacter* Character, float Z_offset, AActor* callActor, float Volume, float
    // Pitch, float speedVolume, UAudioComponent* AudioComponent, UObject* __WorldContext,
    // FHitResult& OutHit). Volume is the caller's -- a puppet's steps are presentation and read too
    // loud at the native 1.0 -- and callActor is us, where the local player's own call passes null.
    // lib_C::step tests callActor for int_objects and calls its `stepped`; mainPlayer_C implements
    // the interface with an empty event, so ours costs one dispatch and does nothing.
    ue_wrap::ParamFrame f(g_stepFn);
    f.Set<void*>(L"Character", character);
    f.Set<float>(L"Z_offset", 0.f);
    f.Set<void*>(L"callActor", character);
    f.Set<float>(L"Volume", volume);
    f.Set<float>(L"Pitch", 1.0f);
    f.Set<float>(L"speedVolume", 400.0f);
    f.Set<void*>(L"AudioComponent", nullptr);
    f.Set<void*>(L"__WorldContext", character);
    // OutHit stays zeroed in the frame; the BP writes it, we ignore it.
    return ue_wrap::Call(g_libCdo, f);
}

}  // namespace ue_wrap::votv_lib
