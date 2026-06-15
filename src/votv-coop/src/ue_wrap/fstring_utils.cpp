// ue_wrap/fstring_utils.cpp -- see header. Mirrors ftext_utils (resolve + pin shape).

#include "ue_wrap/fstring_utils.h"

#include "ue_wrap/call.h"
#include "ue_wrap/log.h"

#include <cstring>

namespace ue_wrap::fstring_utils {

namespace R = ue_wrap::reflection;

namespace {

void* g_kslCdo = nullptr;        // KismetStringLibrary CDO
void* g_concatFn = nullptr;      // Concat_StrStr(FString A, FString B) -> FString

bool Resolve() {
    if (g_kslCdo && g_concatFn) return true;
    if (!g_kslCdo) g_kslCdo = R::FindClassDefaultObject(L"KismetStringLibrary");
    if (g_kslCdo && !g_concatFn) {
        if (void* kc = R::ClassOf(g_kslCdo))
            g_concatFn = R::FindFunction(kc, L"Concat_StrStr");
    }
    return g_kslCdo && g_concatFn;
}

}  // namespace

bool MintFString(const std::wstring& s, void* outHeader16) {
    if (!outHeader16) return false;
    if (s.empty()) {
        // The null FString is a valid empty value the engine handles everywhere.
        std::memset(outHeader16, 0, sizeof(R::FString));
        return true;
    }
    if (!Resolve()) return false;
    R::FString a{};  // empty A
    R::FString b{};
    b.Data = const_cast<wchar_t*>(s.c_str());
    b.Num  = static_cast<int32_t>(s.size() + 1);
    b.Max  = b.Num;
    ue_wrap::ParamFrame f(g_concatFn);
    if (!f.valid()) return false;
    if (!f.SetRaw(L"A", &a, sizeof(a)) || !f.SetRaw(L"B", &b, sizeof(b))) return false;
    if (!ue_wrap::Call(g_kslCdo, f)) return false;
    // The ReturnValue's buffer is engine-allocated; hand its header to the
    // caller's field and never free it ourselves (~ParamFrame raw-frees the
    // frame only -- the deliberate pin/leak doctrine; the engine's later
    // reassign/destroy of the field releases it with the matching allocator).
    R::FString ret{};
    if (!f.GetRaw(L"ReturnValue", &ret, sizeof(ret))) return false;
    std::memcpy(outHeader16, &ret, sizeof(ret));
    return true;
}

}  // namespace ue_wrap::fstring_utils
