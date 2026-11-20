// coop/dev/nail_act.cpp -- see coop/dev/nail_act.h.

#include "coop/dev/nail_act.h"

#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace coop::dev::nail_act {
namespace {

namespace E  = ue_wrap::engine;
namespace R  = ue_wrap::reflection;
namespace SR = ue_wrap::save_record;

constexpr float kTipGapCm = 1.f;    // a nail's tip starts this far before the first prop's face, as the held verb's does
constexpr float kTailCm = 10.f;     // the projectile verb's length reaches this far past the second prop
constexpr float kRadToDeg = 57.29577951f;

void* FnOf(void* obj, const wchar_t* name) {
    return obj && SR::PlausibleObjPtr(obj) && R::IsLive(obj) ? R::FindDispatchFunctionCached(R::ClassOf(obj), name) : nullptr;
}

bool WriteFloatField(void* obj, const wchar_t* name, float value) {
    if (!obj || !SR::PlausibleObjPtr(obj) || !R::IsLive(obj)) return false;
    const int32_t off = R::FindPropertyOffset(R::ClassOf(obj), name);
    if (off < 0) return false;
    std::memcpy(static_cast<uint8_t*>(obj) + off, &value, sizeof(value));
    return true;
}

void Say(void (Voice::*say)(const char*), Voice& v, const char* fmt, ...) {
    char buf[400];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    (v.*say)(buf);
}

// A deferred spawn of the nail class at `loc`, a `length` written before the finish when positive. Null when it did not spawn.
void* SpawnNail(Voice& v, void* nailCls, const char* label, const ue_wrap::FVector& loc, float length,
                const ue_wrap::FRotator& rot) {
    void* nail = E::BeginDeferredSpawn(nailCls, loc, rot);
    if (!nail) return nullptr;
    if (length > 0.f && !WriteFloatField(nail, L"length", length)) Say(&Voice::Warn, v, "nail %s: no `length` field to write", label);
    if (!E::FinishDeferredSpawn(nail, loc, rot)) return nullptr;
    return nail;
}

// nailNail takes `a` and answers `fail`. False when the verb did not run.
bool CallVerb(void* nail, const wchar_t* name, bool& fail) {
    void* fn = FnOf(nail, name);
    if (!fn) return false;
    ue_wrap::ParamFrame f(fn);
    if (!f.valid()) return false;
    f.Set<bool>(L"a", false);
    if (!ue_wrap::Call(nail, f)) return false;
    fail = f.Get<bool>(L"fail");
    return true;
}

// The nail's own n(): where its segment runs, said as an Info line.
void SaySegment(Voice& v, void* nail) {
    void* fn = FnOf(nail, L"n");
    ue_wrap::ParamFrame f(fn);
    if (!fn || !f.valid() || !ue_wrap::Call(nail, f)) {
        Say(&Voice::Info, v, "segment of %s: n() did not answer", v.LabelOf(nail).c_str());
        return;
    }
    const ue_wrap::FVector a = f.Get<ue_wrap::FVector>(L"A"), b = f.Get<ue_wrap::FVector>(L"B");
    Say(&Voice::Info, v, "segment of %s: from (%.1f,%.1f,%.1f) to (%.1f,%.1f,%.1f)", v.LabelOf(nail).c_str(),
        static_cast<double>(a.X), static_cast<double>(a.Y), static_cast<double>(a.Z), static_cast<double>(b.X),
        static_cast<double>(b.Y), static_cast<double>(b.Z));
}

}  // namespace

bool NailThePair(Voice& v, const char* act, const char* label, void* nailCls, void* a, void* b, void*& nail) {
    auto unmeasured = [&](const char* fmt, ...) {
        char buf[400];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        v.Unmeasured(act, buf);
    };
    ue_wrap::FVector ca{}, ea{}, cb{}, eb{};
    if (!a || !b || !E::GetActorBounds(a, true, ca, ea) || !E::GetActorBounds(b, true, cb, eb) || !(ea.X > 1.f)) {
        unmeasured("the two props are not there to nail (a=%d b=%d)", a ? 1 : 0, b ? 1 : 0);
        return false;
    }
    // The line runs from a's centre to b's, flat. The tip starts kTipGapCm before a's outer face on it (the face away from b,
    // where the ray from the centre backwards leaves a's bounds), and the length reaches past b as the projectile's does.
    const float dx = cb.X - ca.X, dy = cb.Y - ca.Y;
    const float dist = std::sqrt(dx * dx + dy * dy);
    if (!(dist > 1.f)) {
        unmeasured("the two props stand on one spot (%.2f cm apart flat)", static_cast<double>(dist));
        return false;
    }
    const float ux = dx / dist, uy = dy / dist;
    const float faceX = std::fabs(ux) > 1e-4f ? ea.X / std::fabs(ux) : 1e9f;
    const float faceY = std::fabs(uy) > 1e-4f ? ea.Y / std::fabs(uy) : 1e9f;
    const float face = (std::min)(faceX, faceY);
    const float length = kTipGapCm + dist + 2.f * face + kTailCm;
    const ue_wrap::FRotator rot{0.f, std::atan2(uy, ux) * kRadToDeg, 0.f};
    nail = SpawnNail(v, nailCls, label, {ca.X - ux * (face + kTipGapCm), ca.Y - uy * (face + kTipGapCm), ca.Z}, length, rot);
    if (!nail) {
        unmeasured("the nail did not spawn");
        return false;
    }
    Say(&Voice::Event, v, "act %s: %s spawned at %s", act, label, v.TransformOf(nail).c_str());
    SaySegment(v, nail);
    bool fail = false;
    if (!CallVerb(nail, L"nailNail", fail)) {
        unmeasured("nailNail did not run on %s", label);
        return false;
    }
    Say(&Voice::Event, v, "act %s: nailNail returned fail=%d, intended ends %s and %s: %s", act, fail ? 1 : 0,
        v.LabelOf(a).c_str(), v.LabelOf(b).c_str(), v.NailState(nail).c_str());
    return true;
}

}  // namespace coop::dev::nail_act
