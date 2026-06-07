// harness/harness_diag.cpp -- see harness/harness_diag.h.

#include "harness/harness_diag.h"

#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"

#include <cstdint>
#include <cwctype>
#include <string>

namespace harness::diag {
namespace {

namespace P = ue_wrap::profile;
namespace R = ue_wrap::reflection;

// Case-insensitive substring test (ASCII keywords against a wide string).
bool ContainsCI(const std::wstring& hay, const wchar_t* needle) {
    std::wstring h = hay, n = needle;
    auto lower = [](std::wstring& s) { for (auto& c : s) c = static_cast<wchar_t>(::towlower(c)); };
    lower(h); lower(n);
    return h.find(n) != std::wstring::npos;
}

// List the callable UFunctions a class (and its parents) expose, so we can see
// exactly which one to call (e.g. a Proceed/Skip on the OMEGA widget). Functions
// are UObjects of class "Function" whose Outer is one of the classes in the chain.
void DumpClassFunctions(void* cls, const wchar_t* tag) {
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj || R::ClassNameOf(obj) != L"Function") continue;
        void* outer = R::OuterOf(obj);
        void* c = cls;
        for (int d = 0; d < 16 && c; ++d) {
            if (outer == c) {
                UE_LOGI("  %ls fn: %ls", tag, R::ToString(R::NameOf(obj)).c_str());
                break;
            }
            c = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(c) + P::off::UStruct_SuperStruct);
        }
    }
}

}  // namespace

// Runs on the game thread (posted): dump a UFunction's parameter frame so we can
// verify the FProperty offsets (names/offsets/sizes) against the known UE4.27
// signature before we rely on them for marshaling. Temporary validation aid.
void DumpParams(const wchar_t* className, const wchar_t* funcName) {
    void* cls = R::FindClass(className);
    void* fn = cls ? R::FindFunction(cls, funcName) : nullptr;
    if (!fn) {
        UE_LOGW("paramdump: %ls::%ls not found (cls=%p)", className, funcName, cls);
        return;
    }
    const int32_t frame = R::FunctionFrameSize(fn);
    auto params = R::FunctionParams(fn);
    UE_LOGI("paramdump: %ls::%ls  frameSize=%d  params=%zu", className, funcName,
            frame, params.size());
    for (const auto& p : params) {
        UE_LOGI("    %-28ls off=0x%02x size=%-3d flags=0x%llx%s%s", p.name.c_str(),
                p.offset, p.size, static_cast<unsigned long long>(p.flags),
                (p.flags & ue_wrap::profile::cpf::OutParm) ? " OUT" : "",
                (p.flags & ue_wrap::profile::cpf::ReturnParm) ? " RET" : "");
    }
}

// Runs on the game thread (posted): log enough to confirm where we are.
void Report(const char* label) {
    // NumObjects is O(1). Avoid CountObjectsByClass here (a full GUObjectArray
    // walk) -- Report runs in the play path too, and per the post-ship audit we
    // don't pay a 100k-object scan just for a log line.
    const int32_t n = R::NumObjects();
    void* world = R::FindObjectByClass(P::name::WorldClass);
    std::wstring worldName = world ? R::ToString(R::NameOf(world)) : L"(none)";
    UE_LOGI("harness report [%s]: NumObjects=%d, world=%ls", label, n, worldName.c_str());
}

// Runs on the game thread: log an actor's default subobjects (its components),
// so we can find the mesh component(s) that carry the player's visible body.
void DumpComponents(const char* label, void* actor) {
    if (!actor) { UE_LOGW("components [%s]: null actor", label); return; }
    auto kids = R::ChildObjectsOf(actor);
    UE_LOGI("components [%s] of %p: %zu", label, actor, kids.size());
    for (const auto& k : kids) {
        UE_LOGI("    %-34ls : %ls", k.className.c_str(), k.name.c_str());
    }
}

// Diagnostic: log every live UUserWidget instance (name + class). Used to find
// the OMEGA WARNING startup widget's class so we can auto-Proceed past it. Fast:
// resolves the UserWidget UClass once and walks each object's super chain by
// POINTER compare (no per-object string work). Any widget whose name/class looks
// like an intro/warning gate is flagged and its UFunctions dumped (to find the
// Proceed/Skip to call next).
void DumpLiveWidgets() {
    void* userWidgetCls = R::FindClass(L"UserWidget");
    if (!userWidgetCls) { UE_LOGW("widgets: UserWidget class not found"); return; }
    static const wchar_t* kGateWords[] = {L"omega", L"warning", L"intro", L"disclaimer",
                                          L"epilepsy", L"splash", L"boot", L"startup",
                                          L"proceed", L"continue", L"title", L"legal"};
    const int32_t n = R::NumObjects();
    int found = 0, flagged = 0;
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj) continue;
        void* c = R::ClassOf(obj);
        bool isWidget = false;
        for (int d = 0; d < 16 && c; ++d) {
            if (c == userWidgetCls) { isWidget = true; break; }
            c = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(c) + P::off::UStruct_SuperStruct);
        }
        if (!isWidget) continue;
        const std::wstring nm = R::ToString(R::NameOf(obj));
        if (nm.rfind(L"Default__", 0) == 0) continue;  // skip CDOs
        const std::wstring cn = R::ClassNameOf(obj);
        ++found;
        bool gate = false;
        for (const wchar_t* w : kGateWords)
            if (ContainsCI(nm, w) || ContainsCI(cn, w)) { gate = true; break; }
        if (gate) {
            UE_LOGI("widget[GATE?]: %ls : %ls -- dumping its UFunctions:", cn.c_str(), nm.c_str());
            DumpClassFunctions(R::ClassOf(obj), cn.c_str());
            ++flagged;
        } else {
            UE_LOGI("widget: %ls : %ls", cn.c_str(), nm.c_str());
        }
    }
    UE_LOGI("widgets: %d live UserWidget instances (%d flagged as intro/gate candidates)", found, flagged);
}

}  // namespace harness::diag
