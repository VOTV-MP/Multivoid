// coop/dev/container_intent_drill_throw_into_probe.cpp -- what the throwinto arm measures of the drive and of the target's trigger,
// by reflection, at each release: for the drive's root and static-mesh component and for the target's `overlap` box, whether it
// generates overlap events, its collision-enabled state, its object type and its responses to the trigger channel
// (ECC_GameTraceChannel2), the physics-body channel and the world-dynamic channel; for the box, its WORLD place and scaled extent.
// Then, once the copy settled, the path's closest approach to that box. Measuring only: nothing here changes the drive or the box.

#include "container_intent_drill_throw_into.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/engine/engine_component.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace coop::dev::container_intent_drill::detail::into {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

constexpr int kChanWorldDynamic = 1;    // ECollisionChannel
constexpr int kChanPhysicsBody  = 5;
constexpr int kChanTrigger      = 15;   // ECC_GameTraceChannel2, "trigger"
constexpr int kSegSteps         = 8;    // sub-points taken along each segment of the path

// A uint8 return of a component function, with an optional uint8 argument; -1 when it does not resolve or run.
int CompU8(void* comp, const wchar_t* fn, const wchar_t* argName, int arg) {
    void* f = comp ? R::FindDispatchFunctionCached(R::ClassOf(comp), fn) : nullptr;
    ue_wrap::ParamFrame frame(f);
    if (!f || !frame.valid()) return -1;
    if (argName && !frame.Set<uint8_t>(argName, static_cast<uint8_t>(arg))) return -1;
    if (!ue_wrap::Call(comp, frame)) return -1;
    return frame.Get<uint8_t>(L"ReturnValue");
}

int CompBool(void* comp, const wchar_t* fn) {
    void* f = comp ? R::FindDispatchFunctionCached(R::ClassOf(comp), fn) : nullptr;
    ue_wrap::ParamFrame frame(f);
    if (!f || !frame.valid() || !ue_wrap::Call(comp, frame)) return -1;
    return frame.Get<bool>(L"ReturnValue") ? 1 : 0;
}

std::string Narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t ch : w) s.push_back(ch < 128 ? static_cast<char>(ch) : '?');
    return s;
}

void PrintComp(const char* role, int n, int t, const char* when, const char* label, void* comp) {
    if (!comp || !R::IsLive(comp)) {
        UE_LOGI("[CI-DRILL] %s %s n=%d try %d %s: %s: no component", role, ArmName(), n, t, when, label);
        return;
    }
    UE_LOGI("[CI-DRILL] %s %s n=%d try %d %s: %s %s (%s): generateOverlapEvents=%d collisionEnabled=%d objectType=%d; response "
            "(0 ignore, 1 overlap, 2 block) to the trigger channel %d, to physics body %d, to world dynamic %d", role, ArmName(), n, t, when,
            label, Narrow(R::ToString(R::NameOf(comp))).c_str(), Narrow(R::ClassNameOf(comp)).c_str(),
            CompBool(comp, L"GetGenerateOverlapEvents"), CompU8(comp, L"GetCollisionEnabled", nullptr, 0),
            CompU8(comp, L"GetCollisionObjectType", nullptr, 0), CompU8(comp, L"GetCollisionResponseToChannel", L"Channel", kChanTrigger),
            CompU8(comp, L"GetCollisionResponseToChannel", L"Channel", kChanPhysicsBody),
            CompU8(comp, L"GetCollisionResponseToChannel", L"Channel", kChanWorldDynamic));
}

void* RootOf(void* actor) {
    void* f = R::FindDispatchFunctionCached(R::ClassOf(actor), L"K2_GetRootComponent");
    ue_wrap::ParamFrame frame(f);
    if (!f || !frame.valid() || !ue_wrap::Call(actor, frame)) return nullptr;
    return frame.Get<void*>(L"ReturnValue");
}

void* MeshOf(void* actor) {
    const int32_t off = R::FindPropertyOffset(R::ClassOf(actor), L"StaticMesh");
    return off >= 0 ? *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(actor) + off) : nullptr;
}

// `p` in the box's own frame (UE's rotation matrix for the box's FRotator, undone).
FVector ToLocal(const TriggerBox& b, const FVector& p) {
    constexpr float kRad = 3.14159265358979f / 180.f;
    const float sp = std::sin(b.rot.Pitch * kRad), cp = std::cos(b.rot.Pitch * kRad);
    const float sy = std::sin(b.rot.Yaw * kRad), cy = std::cos(b.rot.Yaw * kRad);
    const float sr = std::sin(b.rot.Roll * kRad), cr = std::cos(b.rot.Roll * kRad);
    const float dx = p.X - b.loc.X, dy = p.Y - b.loc.Y, dz = p.Z - b.loc.Z;
    const FVector ax{cp * cy, cp * sy, sp};
    const FVector ay{sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp};
    const FVector az{-(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp};
    return {dx * ax.X + dy * ax.Y + dz * ax.Z, dx * ay.X + dy * ay.Y + dz * ay.Z, dx * az.X + dy * az.Y + dz * az.Z};
}

// How far `p` is outside the box (0 inside).
float OutsideCm(const TriggerBox& b, const FVector& p) {
    const FVector l = ToLocal(b, p);
    const float ox = std::max(std::fabs(l.X) - b.ext.X, 0.f), oy = std::max(std::fabs(l.Y) - b.ext.Y, 0.f),
                oz = std::max(std::fabs(l.Z) - b.ext.Z, 0.f);
    return std::sqrt(ox * ox + oy * oy + oz * oz);
}

}  // namespace

bool PointInBox(const TriggerBox& box, const FVector& p) { return OutsideCm(box, p) == 0.f; }

StepStats StepsNearBox(const Track& tr, const TriggerBox& box, float withinCm) {
    StepStats out;
    uint64_t dtSum = 0;
    int pairs = 0;
    for (size_t i = 0; i < tr.samples.size(); ++i) {
        if (OutsideCm(box, tr.samples[i].pos) > withinCm) continue;
        ++out.nearCount;
        if (i == 0) continue;
        const float step = Dist3(tr.samples[i].pos, tr.samples[i - 1].pos);
        const uint64_t dt = tr.samples[i].t - tr.samples[i - 1].t;
        dtSum += dt;
        ++pairs;
        if (step > out.step) {
            out.step = step;
            out.dtMs = dt;
        }
    }
    if (out.dtMs) out.speed = out.step * 1000.f / static_cast<float>(out.dtMs);
    if (pairs) out.frameMs = static_cast<float>(dtSum) / static_cast<float>(pairs);
    out.perFrame = out.speed * out.frameMs / 1000.f;
    return out;
}

bool ReadTriggerBox(void* container, TriggerBox& out) {
    out = TriggerBox{};
    void* comp = TriggerComponent(container);
    void* sf = comp ? R::FindDispatchFunctionCached(R::ClassOf(comp), L"GetScaledBoxExtent") : nullptr;
    void* rf = comp ? R::FindDispatchFunctionCached(R::ClassOf(comp), L"K2_GetComponentRotation") : nullptr;
    ue_wrap::ParamFrame ext(sf), rot(rf);
    if (!comp || !sf || !rf || !ext.valid() || !rot.valid() || !ue_wrap::Call(comp, ext) || !ue_wrap::Call(comp, rot) ||
        !E::TryGetComponentLocation(comp, out.loc))
        return false;
    out.ext = ext.Get<FVector>(L"ReturnValue");
    out.rot = rot.Get<ue_wrap::FRotator>(L"ReturnValue");
    // The local axes' world Z components, as ToLocal's matrix has them: x -> sp, y -> -sr*cp, z -> cr*cp.
    constexpr float kRad = 3.14159265358979f / 180.f;
    const float sp = std::sin(out.rot.Pitch * kRad), cp = std::cos(out.rot.Pitch * kRad);
    const float sr = std::sin(out.rot.Roll * kRad), cr = std::cos(out.rot.Roll * kRad);
    out.zReach = std::fabs(sp) * out.ext.X + std::fabs(sr * cp) * out.ext.Y + std::fabs(cr * cp) * out.ext.Z;
    out.ok = true;
    return true;
}

void ProbeRelease(const char* role, int n, int t, const char* when, void* drive, void* container, TriggerBox& box) {
    PrintComp(role, n, t, when, "drive root", drive ? RootOf(drive) : nullptr);
    PrintComp(role, n, t, when, "drive static mesh", drive ? MeshOf(drive) : nullptr);
    PrintComp(role, n, t, when, "target overlap box", TriggerComponent(container));
    if (!ReadTriggerBox(container, box)) {
        UE_LOGI("[CI-DRILL] %s %s n=%d try %d %s: the target's overlap box does not read (place, rotation or extent)", role, ArmName(), n, t, when);
        return;
    }
    UE_LOGI("[CI-DRILL] %s %s n=%d try %d %s: target overlap box in the world at (%.1f,%.1f,%.1f), rotation (pitch %.1f yaw %.1f "
            "roll %.1f), scaled half-extent (%.1f,%.1f,%.1f), world-Z reach %.1f", role, ArmName(), n, t, when, static_cast<double>(box.loc.X),
            static_cast<double>(box.loc.Y), static_cast<double>(box.loc.Z), static_cast<double>(box.rot.Pitch),
            static_cast<double>(box.rot.Yaw), static_cast<double>(box.rot.Roll), static_cast<double>(box.ext.X),
            static_cast<double>(box.ext.Y), static_cast<double>(box.ext.Z), static_cast<double>(box.zReach));
}

void BoxApproachPrint(const char* role, int n, int t, const Track& tr, uint64_t pressMs, const TriggerBox& box) {
    const std::vector<Sample>& s = tr.samples;
    if (!box.ok || s.empty()) {
        UE_LOGI("[CI-DRILL] %s %s n=%d try %d box approach: %s", role, ArmName(), n, t, s.empty() ? "no sample of the drive" : "no box read");
        return;
    }
    float best = OutsideCm(box, s[0].pos);
    double bestT = static_cast<double>(s[0].t);
    FVector bestP = s[0].pos;
    bool crossed = best == 0.f;
    int insideSamples = best == 0.f ? 1 : 0;
    for (size_t i = 0; i + 1 < s.size(); ++i) {
        if (OutsideCm(box, s[i + 1].pos) == 0.f) ++insideSamples;
        for (int k = 1; k <= kSegSteps; ++k) {
            const float u = static_cast<float>(k) / static_cast<float>(kSegSteps);
            const FVector p{s[i].pos.X + (s[i + 1].pos.X - s[i].pos.X) * u, s[i].pos.Y + (s[i + 1].pos.Y - s[i].pos.Y) * u,
                            s[i].pos.Z + (s[i + 1].pos.Z - s[i].pos.Z) * u};
            const float d = OutsideCm(box, p);
            if (d == 0.f) crossed = true;
            if (d >= best) continue;
            best = d;
            bestP = p;
            bestT = static_cast<double>(s[i].t) + static_cast<double>(u) * static_cast<double>(s[i + 1].t - s[i].t);
        }
    }
    UE_LOGI("[CI-DRILL] %s %s n=%d try %d box approach: the drive came within %.1f cm of the overlap box (0 is inside) at (%.0f,%.0f,%.0f), "
            "%.0f ms after the release; %d of %zu samples inside the box; the path (each segment taken in %d steps) %s the box", role,
            ArmName(), n, t, static_cast<double>(best), static_cast<double>(bestP.X), static_cast<double>(bestP.Y),
            static_cast<double>(bestP.Z), bestT - static_cast<double>(pressMs), insideSamples, s.size(), kSegSteps,
            crossed ? "entered" : "never entered");
}

}  // namespace coop::dev::container_intent_drill::detail::into
