// coop/dev/deskaudio_probe.cpp -- [dev] whether the signal desk's sound components are heard in space
// (row deskaudio_probe), for Ghezra/Multivoid issue 29. Once the desk exists and the desk_snd lane has tried
// the cursor loop's falloff for it: for each of the desk's audio components the desk_snd lane replays, and its signal
// sound, the component's bAllowSpatialization, bOverrideAttenuation and AttenuationSettings, and its sound's own
// AttenuationSettings (`[DESKAUDIO]`), then the verdict `rig: VERDICT deskaudio PASS|FAIL|INVALID`: PASS when the cursor
// loop (corrds_loop) holds the same attenuation settings as the ping loop, FAIL when it holds none or others, INVALID when
// the ping loop has none to compare or the desk never came.

#include "coop/dev/deskaudio_probe.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/interactables/desk_snd_fx.h"
#include "coop/net/session.h"
#include "coop/session/rig_verdict.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/desk/console_desk.h"

#include <windows.h>

#include <cstdint>
#include <string>

namespace coop::dev::deskaudio_probe {
namespace {

namespace R  = ue_wrap::reflection;
namespace CD = ue_wrap::console_desk;
namespace RV = coop::rig_verdict;

constexpr const char* kTag = "deskaudio";
// The desk and its lane's decision come with the world; the bound only says INVALID, it never decides a verdict.
constexpr uint64_t kBoundMs = 300000;

void* Obj(void* obj, const wchar_t* name) {
    const int32_t off = obj ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    return off >= 0 ? *reinterpret_cast<void* const*>(static_cast<const char*>(obj) + off) : nullptr;
}

int Bool(void* obj, const wchar_t* name) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!obj || !R::FindBoolProperty(R::ClassOf(obj), name, off, mask)) return -1;
    return (static_cast<const uint8_t*>(obj)[off] & mask) ? 1 : 0;
}

std::wstring NameOf(void* obj) { return obj ? R::ToString(R::NameOf(obj)) : std::wstring(L"none"); }

uint64_t g_startAt = 0;
bool g_done = false;

// The cursor loop's attenuation settings against the ping loop's, said as the verdict.
void Judge(void* desk) {
    void* const ping = Obj(Obj(desk, L"audio_coord_pingLoop"), L"AttenuationSettings");
    void* const cursor = Obj(Obj(desk, L"corrds_loop"), L"AttenuationSettings");
    if (!ping)
        RV::Say(kTag, RV::Outcome::Invalid, "the ping loop holds no attenuation settings to compare the cursor loop's with");
    else if (!cursor)
        RV::Say(kTag, RV::Outcome::Fail, "the cursor loop (corrds_loop) holds no attenuation settings: it is heard at full "
                "volume across the map");
    else if (cursor != ping)
        RV::Say(kTag, RV::Outcome::Fail, "the cursor loop holds %ls, not the ping loop's %ls", NameOf(cursor).c_str(),
                NameOf(ping).c_str());
    else
        RV::Say(kTag, RV::Outcome::Pass, "the cursor loop holds the ping loop's attenuation settings (%ls)",
                NameOf(ping).c_str());
}

}  // namespace

void Tick(coop::net::Session* session) {
    static const bool on = coop::config::ResolveFlag(coop::config_registry::rows::deskaudio_probe);
    if (!on || g_done || !session || !session->running()) return;
    if (!g_startAt) g_startAt = ::GetTickCount64();
    // The event: the desk stands and the desk_snd lane has tried its cursor loop's falloff (it took, was left without, or failed).
    void* const desk = CD::EnsureResolved() ? CD::Instance() : nullptr;
    if (!desk || coop::desk_snd_fx::CursorLoopDesk() != desk) {
        if (::GetTickCount64() - g_startAt > kBoundMs) {
            g_done = true;
            RV::Say(kTag, RV::Outcome::Invalid, "the desk and its cursor loop's falloff did not come in %llu ms",
                    static_cast<unsigned long long>(kBoundMs));
        }
        return;
    }
    g_done = true;
    for (const wchar_t* field : {L"audio_coordKeyPress", L"audio_coordFail", L"audio_coordButtonSound",
                                 L"audio_coord_pingSound", L"corrds_loop", L"audio_coord_pingLoop", L"deny",
                                 L"signalSound"}) {
        void* const comp = Obj(desk, field);
        void* const sound = Obj(comp, L"Sound");
        UE_LOGI("[DESKAUDIO] %ls: comp %ls spatial=%d overrideAtt=%d compAtt=%ls sound=%ls soundAtt=%ls", field,
                comp ? R::ClassNameOf(comp).c_str() : L"none", Bool(comp, L"bAllowSpatialization"),
                Bool(comp, L"bOverrideAttenuation"), NameOf(Obj(comp, L"AttenuationSettings")).c_str(),
                NameOf(sound).c_str(), NameOf(Obj(sound, L"AttenuationSettings")).c_str());
    }
    // The falloff itself: att_default's Attenuation struct, its falloff distance and its shape's extents.
    void* const att = Obj(Obj(desk, L"audio_coord_pingLoop"), L"AttenuationSettings");
    void* const inner = att ? R::PropertyInnerStruct(R::ClassOf(att), L"Attenuation") : nullptr;
    const int32_t at = att ? R::FindPropertyOffset(R::ClassOf(att), L"Attenuation") : -1;
    if (inner && at >= 0) {
        const uint8_t* const base = static_cast<const uint8_t*>(att) + at;
        // The struct and its parents (FSoundAttenuationSettings < FBaseAttenuationSettings), each one's own members.
        for (void* st = inner; st; st = R::SuperStructOf(st)) {
            for (const R::StructFieldInfo& f : R::EnumerateStructFields(st)) {
                if (f.name == L"FalloffDistance" || f.name == L"AttenuationShapeExtents" || f.name == L"DistanceAlgorithm" ||
                    f.name == L"AttenuationShape") {
                    const float* v = reinterpret_cast<const float*>(base + f.offset);
                    UE_LOGI("[DESKAUDIO] %ls.%ls = %.1f %.1f %.1f (size %d, byte %u)", NameOf(att).c_str(), f.name.c_str(),
                            f.size >= 4 ? v[0] : 0.f, f.size >= 12 ? v[1] : 0.f, f.size >= 12 ? v[2] : 0.f, f.size,
                            static_cast<unsigned>(base[f.offset]));
                }
            }
            int32_t off = -1;
            uint8_t mask = 0;
            if (R::FindBoolProperty(st, L"bAttenuate", off, mask))
                UE_LOGI("[DESKAUDIO] %ls.bAttenuate = %d", NameOf(att).c_str(), (base[off] & mask) ? 1 : 0);
        }
    }
    Judge(desk);
}

void OnDisconnect() {
    g_done = false;
    g_startAt = 0;
}

}  // namespace coop::dev::deskaudio_probe
