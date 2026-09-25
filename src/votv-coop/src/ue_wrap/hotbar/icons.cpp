// ue_wrap/hotbar/icons.cpp -- see ue_wrap/hotbar/icons.h.

#include "ue_wrap/hotbar/icons.h"

#include "ue_wrap/actors/inventory.h"
#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/world/world_singleton.h"
#include "ue_wrap/core/sdk_profile.h"

#include <chrono>

namespace ue_wrap::hotbar {
namespace {

namespace R   = ue_wrap::reflection;
namespace P   = ue_wrap::profile;
namespace INV = ue_wrap::inventory;
namespace SR  = ue_wrap::save_record;

constexpr const wchar_t* kUiClass    = L"ui_UI_C";
constexpr const wchar_t* kPropProc   = L"propProcessor_C";
constexpr const wchar_t* kGameInst   = L"mainGameInstance_C";
constexpr const wchar_t* kMatInst    = L"MaterialInstance";   // owns TextureParameterValues
constexpr const wchar_t* kTexParam   = L"tex";                // the parameter updateSlotInv writes
constexpr const wchar_t* kRefreshFn  = L"updateSlotInv";

struct Offsets {
    bool    ok              = false;
    int32_t playerInterface = -1;
    int32_t propRenderer    = -1;
    int32_t slotInv         = -1;
    int32_t slotTexts       = -1;
    int32_t iconNames       = -1;
    int32_t rendererPhases  = -1;
    int32_t texsGameInst    = -1;
    int32_t texParams       = -1;   // UMaterialInstance.TextureParameterValues
    int32_t texParamStride  = 0;    // = sizeof(FTextureParameterValue)
    int32_t texParamInfo    = -1;   // FMaterialParameterInfo (its first member is FName Name)
    int32_t texParamValue   = -1;   // UTexture*
};

Offsets g_off;
void*   g_matInstClass = nullptr;
void*   g_refreshFn    = nullptr;

bool Resolve() {
    if (g_off.ok) return true;
    // Throttled here rather than at each call site: every lookup below is a GUObjectArray walk,
    // and until the world's classes are loaded they all fail -- which, at the readers' 8 Hz, is a
    // per-frame full-array scan for the whole time the main menu is up.
    static std::chrono::steady_clock::time_point s_lastTry{};
    const auto now = std::chrono::steady_clock::now();
    if (s_lastTry != std::chrono::steady_clock::time_point{} &&
        now - s_lastTry < std::chrono::seconds(1))
        return false;
    s_lastTry = now;

    void* gmCls = R::FindClass(P::name::GamemodeClass);
    void* uiCls = R::FindClass(kUiClass);
    void* ppCls = R::FindClass(kPropProc);
    void* giCls = R::FindClass(kGameInst);
    g_matInstClass = R::FindClass(kMatInst);
    if (!gmCls || !uiCls || !ppCls || !giCls || !g_matInstClass) return false;

    Offsets o;
    o.playerInterface = R::FindPropertyOffset(gmCls, L"playerInterface");
    o.propRenderer    = R::FindPropertyOffset(gmCls, L"propRenderer");
    o.slotInv         = R::FindPropertyOffset(uiCls, L"slotInv");
    o.slotTexts       = R::FindPropertyOffset(uiCls, L"slotTexts");
    o.iconNames       = R::FindPropertyOffset(ppCls, L"names");
    o.rendererPhases  = R::FindPropertyOffset(ppCls, L"fins");
    o.texsGameInst    = R::FindPropertyOffset(giCls, L"texs_GAMEINST");
    o.texParams       = R::FindPropertyOffset(g_matInstClass, L"TextureParameterValues");
    // TextureParameterValues is an ARRAY property, so PropertyInnerStruct (which reads
    // FStructProperty::Struct) cannot reach its element type. The element is a plain engine
    // UScriptStruct, so it is looked up by name -- still resolved, never a hardcoded stride.
    if (void* elem = R::FindObject(L"TextureParameterValue", L"ScriptStruct")) {
        o.texParamStride = R::StructSize(elem);
        o.texParamInfo   = R::FindPropertyOffset(elem, L"ParameterInfo");
        o.texParamValue  = R::FindPropertyOffset(elem, L"ParameterValue");
    }
    g_refreshFn = R::FindFunction(uiCls, kRefreshFn);

    if (o.playerInterface < 0 || o.propRenderer < 0 || o.slotInv < 0 || o.slotTexts < 0 ||
        o.iconNames < 0 || o.texsGameInst < 0 || o.texParams < 0 || o.texParamStride <= 0 ||
        o.texParamInfo < 0 || o.texParamValue < 0 || !g_refreshFn) {
        static bool s_said = false;
        if (!s_said) {
            s_said = true;
            UE_LOGW("hotbar: unresolved -- the quick-slot icon edge is inert (playerInterface=%d "
                    "propRenderer=%d slotInv=%d slotTexts=%d names=%d texs=%d texParams=%d "
                    "stride=%d info=%d value=%d verb=%p)",
                    o.playerInterface, o.propRenderer, o.slotInv, o.slotTexts, o.iconNames,
                    o.texsGameInst, o.texParams, o.texParamStride, o.texParamInfo, o.texParamValue,
                    g_refreshFn);
        }
        return false;
    }
    o.ok = true;
    g_off = o;
    return true;
}

// The layer's own bounded TArray read, not a second one: save_record::ReadArr already rejects a
// negative or absurd count AND checks the data pointer is plausible, which a hand-rolled header
// read here did not. A TArray caught mid-realloc reads as empty rather than sending the walks
// below into arbitrary memory.
SR::Arr ArrayAt(const void* base, int32_t off) {
    if (!base || off < 0) return SR::Arr{};
    return SR::ReadArr(base, off);
}

}  // namespace

bool Read(State& out) {
    if (!Resolve()) return false;
    void* gm = world_singleton::Gamemode();
    if (!gm) return false;

    State s;
    s.ui       = *reinterpret_cast<void* const*>(static_cast<uint8_t*>(gm) + g_off.playerInterface);
    s.renderer = *reinterpret_cast<void* const*>(static_cast<uint8_t*>(gm) + g_off.propRenderer);
    if (s.ui && R::IsLive(s.ui)) {
        s.slots     = ArrayAt(s.ui, g_off.slotInv).num;
        s.slotTexts = ArrayAt(s.ui, g_off.slotTexts).num;
    } else {
        s.ui = nullptr;
    }
    if (s.renderer && R::IsLive(s.renderer)) {
        s.iconNames = ArrayAt(s.renderer, g_off.iconNames).num;
        if (g_off.rendererPhases >= 0)
            s.rendererPhases = *reinterpret_cast<const int32_t*>(
                static_cast<uint8_t*>(s.renderer) + g_off.rendererPhases);
    } else {
        s.renderer = nullptr;
    }
    // texs_GAMEINST lives on the GAME INSTANCE, which outlives the world: that is why a second
    // world load in one process finds the icons already built and never shows this defect.
    if (void* gi = world_singleton::GameInstance()) s.iconTextures = ArrayAt(gi, g_off.texsGameInst).num;
    s.carried = INV::LivePersonalStoreCount();

    out = s;
    return true;
}

std::wstring SlotTexture(const State& s, int i) {
    if (!g_off.ok || !s.ui || i < 0 || i >= s.slots || !R::IsLive(s.ui)) return std::wstring();
    const SR::Arr slots = ArrayAt(s.ui, g_off.slotInv);
    if (i >= slots.num) return std::wstring();
    void* img = reinterpret_cast<void* const*>(slots.data)[i];
    if (!img || !R::IsLive(img)) return std::wstring();

    const uint8_t* brush = static_cast<const uint8_t*>(img) + P::off::UImage_Brush;
    void* res = *reinterpret_cast<void* const*>(brush + P::off::FSlateBrush_ResourceObject);
    if (!res || !R::IsLive(res)) return L"no-material";
    // Only a material instance carries the parameter array; the designer's own material does not,
    // and a slot still holding it has never been through a rebuild.
    bool isMat = false;
    for (void* c = R::ClassOf(res); c; c = R::SuperStructOf(c))
        if (c == g_matInstClass) { isMat = true; break; }
    if (!isMat) return L"no-material";

    const SR::Arr params = ArrayAt(res, g_off.texParams);
    for (int j = 0; j < params.num; ++j) {
        const uint8_t* e = params.data + static_cast<size_t>(j) * g_off.texParamStride;
        const R::FName& pn = *reinterpret_cast<const R::FName*>(e + g_off.texParamInfo);
        if (!R::NameEquals(pn, kTexParam)) continue;
        void* tex = *reinterpret_cast<void* const*>(e + g_off.texParamValue);
        return tex ? R::ToString(R::NameOf(tex)) : L"null";
    }
    return L"no-parameter";
}

bool Rebuild(const State& s) {
    if (!g_off.ok || !g_refreshFn || !s.ui || !R::IsLive(s.ui)) return false;
    ue_wrap::ParamFrame f(g_refreshFn);
    return f.valid() && ue_wrap::Call(s.ui, f);
}

}  // namespace ue_wrap::hotbar
