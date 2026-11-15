// coop/creatures/centipede_pups.cpp -- see coop/creatures/centipede_pups.h.

#include "coop/creatures/centipede_pups.h"

#include "coop/world/spawn_authority.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/facts/function_row.h"

#include <cstdint>
#include <cstring>

namespace coop::centipede_pups {
namespace {

namespace R  = ue_wrap::reflection;
namespace SG = ue_wrap::script_gate;
namespace UH = ue_wrap::ufunction_hook;
namespace P  = ue_wrap::profile;

UE_FACT_FN(kRowBeginDeferred, "centipede_pups", L"GameplayStatics", L"BeginDeferredActorSpawnFromClass", Declared);

void*    g_head = nullptr;      // the copy whose begin play is running, by slot and serial
int32_t  g_headIdx = -1;
void*    g_family = nullptr;    // centipedeHead_C: the class the refused spawns derive from
void*    g_hookFn = nullptr;
int32_t  g_classOff = -2;       // the ActorClass parameter's offset in the hook's frame
bool     g_hooked = false;
bool     g_armed = false;
uint32_t g_refused = 0;

// Before any deferred spawn, while armed: one whose source is the head being begun and whose class is the head's family
// (the pups) is refused; the head's segments, which are another class, pass.
SG::Verdict OnBeginDeferredPre(void* /*context*/, void* source, const uint8_t* parms);

void Disarm() {
    if (g_armed && UH::SetArmed(g_hookFn, &OnBeginDeferredPre, false)) g_armed = false;
    g_head = nullptr;
    g_headIdx = -1;
}

SG::Verdict OnBeginDeferredPre(void* /*context*/, void* source, const uint8_t* parms) {
    if (!g_head || source != g_head || g_classOff < 0 || !g_family || !parms) return SG::Verdict::Run;
    if (!R::IsLiveByIndex(g_head, g_headIdx)) {  // the copy ended before its begin play was seen to finish
        Disarm();
        return SG::Verdict::Run;
    }
    if (!coop::spawn_authority::IsActiveClientSession()) return SG::Verdict::Run;
    void* cls = nullptr;
    std::memcpy(&cls, parms + g_classOff, sizeof(cls));
    if (!cls || !R::IsDescendantOfAny(cls, &g_family, 1)) return SG::Verdict::Run;
    ++g_refused;
    return SG::Verdict::Cancel;
}

// The head family's root class on `head`'s chain: centipedeHead_C, which a pup (wolfgangJr_C) derives from.
void* FamilyOf(void* head) {
    int hops = 0;
    for (void* cls = R::ClassOf(head); cls && hops < 16; cls = R::SuperStructOf(cls), ++hops)
        if (R::NameEquals(R::NameOf(cls), P::name::NpcClass_Centipede)) return cls;
    return nullptr;
}

}  // namespace

void OnMirrorBorn(void* head) {
    if (!head || !R::IsLive(head)) return;
    void* family = FamilyOf(head);
    if (!family) return;  // not the head's family
    if (!g_hooked) {
        const ue_wrap::facts::Resolved fn = ue_wrap::facts::Resolve(kRowBeginDeferred);
        if (!fn || !UH::InstallPreHook(fn, &OnBeginDeferredPre, /*armed=*/false)) {
            UE_LOGE("centipede_pups: the deferred spawn's pre hook did not install -- a copy of Wolfgang may mint pups "
                    "beside the host's");
            return;
        }
        g_hooked = true;
        g_hookFn = fn.Fn();
        g_classOff = R::FindParamOffset(g_hookFn, L"ActorClass");
        if (g_classOff < 0)
            UE_LOGE("centipede_pups: the deferred spawn has no ActorClass parameter -- the hook refuses nothing");
    }
    g_family = family;
    g_head = head;
    g_headIdx = R::InternalIndexOf(head);
    g_refused = 0;
    g_armed = UH::SetArmed(g_hookFn, &OnBeginDeferredPre, true);
}

void OnMirrorBegun(void* head) {
    if (!head || head != g_head) return;
    if (g_refused > 0)
        UE_LOGI("centipede_pups: the copy %p's begin play minted %u pup(s) of its own -- refused (the host's are the pups)",
                head, g_refused);
    Disarm();
}

void OnDisconnect() {
    Disarm();
    g_refused = 0;
}

}  // namespace coop::centipede_pups
