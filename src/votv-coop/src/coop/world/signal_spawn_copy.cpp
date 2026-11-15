// coop/world/signal_spawn_copy.cpp -- see coop/world/signal_spawn_copy.h. The Tardis's birth value, from the 0.10.0 bytecode:
//   analogDScreenTest.autoSave: the "tardis" signal mints tardis_C at (1650, 267, 6100), yaw 35, sets `delay` to 60 by name
//     before the finish.
//   tardis_C.ReceiveBeginPlay: makes its dynamic material, setEvent(true), Delay(`delay`), then appear(true): the sphere's and
//     the mesh's collision off, the effect, the spawn sound and the fade-in timeline. Its sphere's overlap later starts a 10 s
//     Delay and appear(false), which fades out and destroys it.

#include "coop/world/signal_spawn_copy.h"

#include "coop/net/net_clock.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"

#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace coop::signal_spawn_copy {
namespace {

namespace R = ue_wrap::reflection;

constexpr const wchar_t* kTardisName = L"tardis_C";
// A delay outside this is not the desk's 60 or a count down from it: the copy keeps the class default.
constexpr float kMaxDelaySeconds = 3600.f;

std::mutex g_mx;
// HOST: the first ask for each Tardis, by pointer and the object array's slot (a recycled address names another slot), cleared with
// the session.
struct Born { int32_t slot; uint64_t ms; };
std::unordered_map<void*, Born> g_born;

float ReadFloatMember(void* actor, const wchar_t* name, bool& ok) {
    ok = false;
    const int32_t off = actor ? R::FindPropertyOffset(R::ClassOf(actor), name) : -1;
    if (off < 0) return 0.f;
    float v = 0.f;
    std::memcpy(&v, static_cast<const uint8_t*>(actor) + off, sizeof(v));
    ok = std::isfinite(v);
    return v;
}

bool ReadFlag(void* actor, const wchar_t* name) {
    int32_t off = -1;
    uint8_t mask = 0;
    return actor && R::FindBoolProperty(R::ClassOf(actor), name, off, mask) && off >= 0 &&
           (*(static_cast<const uint8_t*>(actor) + off) & mask) != 0;
}

}  // namespace

bool IsTardisClass(void* cls) { return cls && R::NameEquals(R::NameOf(cls), kTardisName); }

bool IsTardisName(const wchar_t* className) { return className && std::wcscmp(className, kTardisName) == 0; }

uint8_t FillBirth(void* cls, void* actor, uint8_t* out, size_t cap) {
    if (!actor || !out || cap < sizeof(float) || !IsTardisClass(cls)) return 0;
    const uint64_t now = coop::net::NowMs();
    uint64_t born = now;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        const int32_t slot = R::InternalIndexOf(actor);
        Born& b = g_born.emplace(actor, Born{slot, now}).first->second;
        if (b.slot != slot) b = Born{slot, now};
        born = b.ms;
    }
    bool ok = false;
    const float delay = ReadFloatMember(actor, L"delay", ok);
    if (!ok || delay < 0.f || delay > kMaxDelaySeconds) {
        UE_LOGW("signal_spawn_copy: tardis_C %p has an unreadable delay (%.1f) -- its copy is born with the class default", actor,
                static_cast<double>(delay));
        return 0;
    }
    float remaining = delay - static_cast<float>(now - born) / 1000.f;
    if (remaining < 0.f || ReadFlag(actor, L"appeared")) remaining = 0.f;
    std::memcpy(out, &remaining, sizeof(remaining));
    return static_cast<uint8_t>(sizeof(remaining));
}

bool SeedTardisCopy(void* actor, const uint8_t* birth, uint8_t birthLen) {
    if (!actor || !IsTardisClass(R::ClassOf(actor))) return false;
    if (birthLen != sizeof(float)) {
        UE_LOGW("signal_spawn_copy: tardis_C birthLen=%u, expected %zu -- the copy runs on the class default delay",
                static_cast<unsigned>(birthLen), sizeof(float));
        return false;
    }
    float remaining = 0.f;
    std::memcpy(&remaining, birth, sizeof(remaining));
    if (!std::isfinite(remaining) || remaining < 0.f || remaining > kMaxDelaySeconds) {
        UE_LOGW("signal_spawn_copy: tardis_C birth delay %.1f is not usable -- the copy runs on the class default",
                static_cast<double>(remaining));
        return false;
    }
    const int32_t off = R::FindPropertyOffset(R::ClassOf(actor), L"delay");
    if (off < 0) {
        UE_LOGW("signal_spawn_copy: tardis_C::delay does not resolve on the copy -- it runs on the class default");
        return false;
    }
    std::memcpy(static_cast<uint8_t*>(actor) + off, &remaining, sizeof(remaining));
    UE_LOGI("signal_spawn_copy: tardis_C copy %p born with %.1f s before it appears (the host's remaining delay)", actor,
            static_cast<double>(remaining));
    return true;
}

void OnDisconnect() {
    std::lock_guard<std::mutex> lk(g_mx);
    g_born.clear();
}

}  // namespace coop::signal_spawn_copy
