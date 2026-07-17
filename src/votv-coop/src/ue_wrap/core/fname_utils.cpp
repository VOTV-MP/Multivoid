// ue_wrap/fname_utils.cpp -- see header.

#include "ue_wrap/core/fname_utils.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/sdk_profile.h"

#include <cstdint>

namespace ue_wrap::fname_utils {

namespace R = ue_wrap::reflection;
namespace P = ue_wrap::profile;

namespace {

void* g_kslCdo = nullptr;
void* g_convStrToNameFn = nullptr;
// Param-name resolution: stock UE4 uses `InString` but some VOTV cooks emit
// lowercase `inString` (same drift as Conv_StringToText). Audit H6
// (2026-05-27): probe both via FindParamOffset (doesn't log on miss).
const wchar_t* g_convStrToNameInputParam = nullptr;

bool Resolve() {
    if (g_kslCdo && g_convStrToNameFn && g_convStrToNameInputParam) return true;
    if (!g_kslCdo) g_kslCdo = R::FindClassDefaultObject(P::name::KismetStringLibraryClass);
    if (g_kslCdo && !g_convStrToNameFn) {
        if (void* kc = R::ClassOf(g_kslCdo)) {
            g_convStrToNameFn = R::FindFunction(kc, P::name::ConvStringToNameFn);
        }
    }
    if (g_convStrToNameFn && !g_convStrToNameInputParam) {
        if (R::FindParamOffset(g_convStrToNameFn, L"InString") >= 0) {
            g_convStrToNameInputParam = L"InString";
        } else if (R::FindParamOffset(g_convStrToNameFn, L"inString") >= 0) {
            g_convStrToNameInputParam = L"inString";
        } else {
            UE_LOGW("fname_utils: Conv_StringToName has neither 'InString' nor "
                    "'inString' -- StringToFName will fail every call");
        }
    }
    return g_kslCdo && g_convStrToNameFn && g_convStrToNameInputParam;
}

}  // namespace

R::FName StringToFName(const std::wstring& s) {
    if (s.empty() || !Resolve()) return R::FName{0, 0};
    using ue_wrap::ParamFrame;
    using ue_wrap::Call;
    std::wstring buf(s);
    R::FString fs{};
    fs.Data = buf.data();
    fs.Num  = static_cast<int32_t>(buf.size()) + 1;  // FString::Num counts the null
    fs.Max  = fs.Num;
    ParamFrame f(g_convStrToNameFn);
    if (!f.SetRaw(g_convStrToNameInputParam, &fs, sizeof(fs))) {
        UE_LOGW("fname_utils::StringToFName: SetRaw failed on '%ls'",
                g_convStrToNameInputParam);
        return R::FName{0, 0};
    }
    if (!Call(g_kslCdo, f)) {
        UE_LOGW("fname_utils::StringToFName: ProcessEvent call failed");
        return R::FName{0, 0};
    }
    return f.Get<R::FName>(L"ReturnValue");
}

}  // namespace ue_wrap::fname_utils
