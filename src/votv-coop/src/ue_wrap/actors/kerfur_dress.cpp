// ue_wrap/actors/kerfur_dress.cpp -- see ue_wrap/actors/kerfur_dress.h.

#include "ue_wrap/actors/kerfur_dress.h"

#include "ue_wrap/actors/kerfur.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"

#include <cstdint>
#include <cstring>

namespace ue_wrap::kerfur_dress {
namespace {

namespace R = ue_wrap::reflection;
namespace P = ue_wrap::profile;

constexpr int32_t kMaxRows = 64;   // a drip longer than this is unreadable garbage

struct ArrayView {
    R::FName* data;
    int32_t num;
    int32_t max;
};

int32_t DripOffset(void* kerfur) {
    static int32_t sOff = -2;
    if (sOff == -2) sOff = R::FindPropertyOffset(R::ClassOf(kerfur), L"drip");
    return sOff;
}

bool CallNamed(void* obj, const wchar_t* fn) {
    void* const f = R::FindDispatchFunctionCached(R::ClassOf(obj), fn);
    if (!f) return false;
    ue_wrap::ParamFrame pf(f);
    return pf.valid() && ue_wrap::Call(obj, pf);
}

void* AnimInstanceOf(void* kerfur) {
    void* const mesh = *reinterpret_cast<void**>(static_cast<uint8_t*>(kerfur) + P::off::ACharacter_Mesh);
    if (!mesh || !R::IsLive(mesh)) return nullptr;
    void* const anim = *reinterpret_cast<void**>(static_cast<uint8_t*>(mesh) + P::off::USkeletalMesh_AnimScriptInstance);
    return anim && R::IsLive(anim) ? anim : nullptr;
}

}  // namespace

bool ReadDrip(void* kerfur, std::vector<std::wstring>& out) {
    out.clear();
    if (!ue_wrap::kerfur::IsKerfurActor(kerfur)) return false;
    const int32_t off = DripOffset(kerfur);
    if (off < 0) return false;
    const auto* a = reinterpret_cast<const ArrayView*>(static_cast<const uint8_t*>(kerfur) + off);
    if (a->num < 0 || a->num > kMaxRows || (a->num > 0 && !a->data)) return false;
    for (int32_t i = 0; i < a->num; ++i) out.push_back(R::ToString(a->data[i]));
    return true;
}

bool ApplyDrip(void* kerfur, const std::vector<std::wstring>& names) {
    if (!ue_wrap::kerfur::IsKerfurActor(kerfur) || names.size() > static_cast<size_t>(kMaxRows)) return false;
    const int32_t off = DripOffset(kerfur);
    if (off < 0) return false;
    auto* a = reinterpret_cast<ArrayView*>(static_cast<uint8_t*>(kerfur) + off);
    R::FName* buf = nullptr;
    if (!names.empty()) {
        buf = static_cast<R::FName*>(R::EngineAlloc(names.size() * sizeof(R::FName)));
        if (!buf) return false;
        for (size_t i = 0; i < names.size(); ++i) buf[i] = ue_wrap::fname_utils::StringToFName(names[i]);
    }
    if (a->data) R::EngineFree(a->data);
    a->data = buf;
    a->num = a->max = static_cast<int32_t>(names.size());
    return CallNamed(kerfur, L"updateDrip");
}

bool Unequip(void* kerfur, const std::wstring& name) {
    if (!ue_wrap::kerfur::IsKerfurActor(kerfur) || name.empty()) return false;
    void* const f = R::FindDispatchFunctionCached(R::ClassOf(kerfur), L"unequipItem");
    if (!f) return false;
    ue_wrap::ParamFrame pf(f);
    if (!pf.valid()) return false;
    const int32_t n = static_cast<int32_t>(name.size()) + 1;
    struct { const wchar_t* data; int32_t num; int32_t max; } fs{name.c_str(), n, n};
    return pf.SetRaw(L"inputPin", &fs, sizeof(fs)) && ue_wrap::Call(kerfur, pf);
}

bool Meow(void* kerfur) { return ue_wrap::kerfur::IsKerfurActor(kerfur) && CallNamed(kerfur, L"makemeow"); }

bool PlayPat(void* kerfur) {
    if (!ue_wrap::kerfur::IsKerfurActor(kerfur)) return false;
    static void* sMontage = nullptr;
    static void* sPlay = nullptr;
    static void* sJump = nullptr;
    if (!sMontage) sMontage = R::FindObject(L"kerfurOmegaV1_Skeleton_Montage", L"AnimMontage");
    if (!sPlay || !sJump) {
        if (void* const cls = R::FindClass(L"AnimInstance")) {
            sPlay = R::FindFunction(cls, L"Montage_Play");
            sJump = R::FindFunction(cls, L"Montage_JumpToSection");
        }
    }
    void* const anim = AnimInstanceOf(kerfur);
    bool played = false;
    if (anim && sMontage && sPlay) {
        ue_wrap::ParamFrame pf(sPlay);
        played = pf.valid() && pf.Set<void*>(L"MontageToPlay", sMontage) && pf.Set<float>(L"InPlayRate", 1.f) &&
                 ue_wrap::Call(anim, pf);
        if (played && sJump) {
            const R::FName section = ue_wrap::fname_utils::StringToFName(L"pat");
            ue_wrap::ParamFrame jf(sJump);
            if (jf.valid() && jf.SetRaw(L"SectionName", &section, sizeof(section)) && jf.Set<void*>(L"Montage", sMontage))
                ue_wrap::Call(anim, jf);
        }
    } else {
        static bool sSaid = false;
        if (!sSaid) {
            sSaid = true;
            UE_LOGW("kerfur_dress: the pat montage did not resolve (anim=%p montage=%p play=%p) -- only its meow plays",
                    anim, sMontage, sPlay);
        }
    }
    Meow(kerfur);
    return played;
}

}  // namespace ue_wrap::kerfur_dress
