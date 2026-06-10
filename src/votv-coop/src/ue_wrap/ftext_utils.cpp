// ue_wrap/ftext_utils.cpp -- see header. Mirrors ue_wrap/fname_utils.cpp (same resolve + pin shape).

#include "ue_wrap/ftext_utils.h"

#include "ue_wrap/call.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"

#include <cstring>

namespace ue_wrap::ftext_utils {

namespace R = ue_wrap::reflection;
namespace P = ue_wrap::profile;

namespace {

void* g_ktlCdo = nullptr;          // KismetTextLibrary CDO
void* g_convStrToTextFn = nullptr; // Conv_StringToText(FString) -> FText
// Param-name drift: stock UE4 uses `InString` but some VOTV cooks emit lowercase `inString`
// (the same drift fname_utils handles for Conv_StringToName). Probe both via FindParamOffset.
const wchar_t* g_inputParam = nullptr;

bool g_cached = false;
uint8_t g_emptyBytes[kFTextSize] = {0};

bool Resolve() {
    if (g_ktlCdo && g_convStrToTextFn && g_inputParam) return true;
    if (!g_ktlCdo) g_ktlCdo = R::FindClassDefaultObject(P::name::KismetTextLibraryClass);
    if (g_ktlCdo && !g_convStrToTextFn) {
        if (void* kc = R::ClassOf(g_ktlCdo)) {
            g_convStrToTextFn = R::FindFunction(kc, P::name::ConvStringToTextFn);
        }
    }
    if (g_convStrToTextFn && !g_inputParam) {
        if (R::FindParamOffset(g_convStrToTextFn, L"InString") >= 0) {
            g_inputParam = L"InString";
        } else if (R::FindParamOffset(g_convStrToTextFn, L"inString") >= 0) {
            g_inputParam = L"inString";
        } else {
            UE_LOGW("ftext_utils: Conv_StringToText has neither 'InString' nor 'inString' -- "
                    "EmptyFText will fail every call");
        }
    }
    return g_ktlCdo && g_convStrToTextFn && g_inputParam;
}

// Mint + pin one valid empty FText, caching its bytes. Returns false if unresolvable.
bool MintEmpty() {
    if (g_cached) return true;
    if (!Resolve()) return false;
    // A valid empty FString param (Num counts the null terminator). Conv_StringToText("")
    // returns the empty FText (FText::GetEmpty() / an empty FTextData) -- always valid.
    static wchar_t s_empty[1] = {0};
    R::FString fs{};
    fs.Data = s_empty;
    fs.Num  = 1;
    fs.Max  = 1;
    ue_wrap::ParamFrame f(g_convStrToTextFn);
    if (!f.valid() || !f.SetRaw(g_inputParam, &fs, sizeof(fs))) {
        UE_LOGW("ftext_utils::MintEmpty: SetRaw('%ls') failed", g_inputParam ? g_inputParam : L"?");
        return false;
    }
    if (!ue_wrap::Call(g_ktlCdo, f)) {
        UE_LOGW("ftext_utils::MintEmpty: ProcessEvent call failed");
        return false;
    }
    // GetRaw bounds-checks against the frame size; the FText is the ReturnValue OUT param.
    // The frame is NOT UE-destructed on ~ParamFrame, so the returned FText's ref is pinned ->
    // these bytes stay valid for the whole process.
    if (!f.GetRaw(L"ReturnValue", g_emptyBytes, kFTextSize)) {
        UE_LOGW("ftext_utils::MintEmpty: GetRaw(ReturnValue) failed");
        return false;
    }
    g_cached = true;
    UE_LOGI("ftext_utils: pinned one empty FText (subcategory filler)");
    return true;
}

}  // namespace

bool EmptyFText(void* out) {
    if (!out) return false;
    if (!MintEmpty()) return false;
    std::memcpy(out, g_emptyBytes, kFTextSize);
    return true;
}

}  // namespace ue_wrap::ftext_utils
