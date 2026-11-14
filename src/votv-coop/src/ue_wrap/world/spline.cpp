// ue_wrap/world/spline.cpp -- see ue_wrap/world/spline.h.

#include "ue_wrap/world/spline.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/reflection.h"

#include <algorithm>

namespace ue_wrap::spline {
namespace {

namespace R = reflection;

// FindFunction reads one class's own list, so each call is found on the class that declares it.
void* SplineFn(const wchar_t* name) {
    void* cls = R::FindClass(L"SplineComponent");
    return cls ? R::FindFunction(cls, name) : nullptr;
}

}  // namespace

bool ReadLocalPoints(void* spline, int32_t maxPoints, std::vector<FVector>& out) {
    out.clear();
    if (!spline) return false;
    ParamFrame num(SplineFn(L"GetNumberOfSplinePoints"));
    if (!num.valid() || !Call(spline, num)) return false;
    const int32_t n = (std::min)(num.Get<int32_t>(L"ReturnValue"), maxPoints);
    void* const atFn = SplineFn(L"GetLocationAtSplinePoint");
    for (int32_t i = 0; i < n; ++i) {
        ParamFrame at(atFn);
        if (!at.valid()) return false;
        at.Set<int32_t>(L"PointIndex", i);
        at.Set<uint8_t>(L"CoordinateSpace", 0);  // ESplineCoordinateSpace::Local
        if (!Call(spline, at)) return false;
        out.push_back(at.Get<FVector>(L"ReturnValue"));
    }
    return n > 0;
}

bool WriteLocalPoints(void* spline, const std::vector<FVector>& pts) {
    if (!spline) return false;
    {
        ParamFrame clr(SplineFn(L"ClearSplinePoints"));
        if (!clr.valid()) return false;
        clr.Set<bool>(L"bUpdateSpline", false);
        Call(spline, clr);
    }
    void* const addFn = SplineFn(L"AddSplinePoint");
    for (const FVector& v : pts) {
        ParamFrame add(addFn);
        if (!add.valid()) return false;
        add.Set<FVector>(L"Position", v);
        add.Set<uint8_t>(L"CoordinateSpace", 0);
        add.Set<bool>(L"bUpdateSpline", false);
        Call(spline, add);
    }
    void* const upd = SplineFn(L"UpdateSpline");
    return upd && R::CallFunction(spline, upd, nullptr);
}

bool Length(void* spline, float& out) {
    ParamFrame len(SplineFn(L"GetSplineLength"));
    if (!spline || !len.valid() || !Call(spline, len)) return false;
    out = len.Get<float>(L"ReturnValue");
    return true;
}

}  // namespace ue_wrap::spline
