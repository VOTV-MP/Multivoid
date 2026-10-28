// coop/dev/container_intent_drill_throw_hit.cpp -- the first blocking hit of a thrown prop, for the container intent drill's
// throw arms; see container_intent_drill_throw.h. The seam is the Blueprint's own handler of its mesh's OnComponentHit,
// prop_drive_C's BndEvt__StaticMesh_K2Node_ComponentBoundEvent_0_ComponentHitSignature__DelegateSignature (the engine
// dispatches the delegate to it), watched through the script gate the way atv_hit_guard watches the ATV's hit handlers.
// Observe-only: the handler always runs. The hits of the watched actors are recorded as they come; the judge picks the first
// one at or after the release and prints, once per throw per peer:
//   throw <role> first-hit eid= t= other=<actor> class= comp= profile= response=<ignore|overlap|block> at= normal= speed-before=
// or `first-hit none` when there is none within kFirstHitWindowMs of the release. The response is the other component's
// answer to the thrown body's collision object type.

#include "container_intent_drill_throw.h"

#include "coop/dev/container_route_census.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/hit_result.h"
#include "ue_wrap/facts/function_row.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace coop::dev::container_intent_drill::detail {
namespace {

namespace R   = ue_wrap::reflection;
namespace sg  = ue_wrap::script_gate;
namespace cen = coop::dev::container_route_census;
using ue_wrap::FVector;

// The class is the drive's, known to the site at run time (ResolveOn): the reference table keys this handler's name on the
// classes that declare it, and the drive is not yet among them.
UE_FACT_FN(kRowHit, "container_intent_drill_throw", nullptr,
           L"BndEvt__StaticMesh_K2Node_ComponentBoundEvent_0_ComponentHitSignature__DelegateSignature", RuntimeClass);

constexpr int kHitTag = 71;
constexpr int kMaxHits = 8;

struct HitRec {
    uint64_t t = 0;
    ue_wrap::CachedObjRef mover, otherActor, otherComp, comp;
    FVector at{}, normal{};
};
HitRec g_recs[kMaxHits];
int g_nRecs = 0;
const void* g_watched[2] = {nullptr, nullptr};
bool g_installed = false;
bool g_live = false;

void* ObjParam(const sg::Call& c, R::ParamOffset& param) {
    const int32_t off = c.function ? param.Of(c.function) : -1;
    if (off < 0 || !c.locals) return nullptr;
    void* p = nullptr;
    std::memcpy(&p, c.locals + off, sizeof(p));
    return p;
}

sg::Verdict HitPre(const sg::Call& c) {
    if (g_nRecs >= kMaxHits || !c.object || (c.object != g_watched[0] && c.object != g_watched[1])) return sg::Verdict::Run;
    static R::ParamOffset sComp(L"HitComponent"), sActor(L"OtherActor"), sOther(L"OtherComp"), sHit(L"Hit");
    HitRec& r = g_recs[g_nRecs++];
    r.t = cen::UnixMs();
    r.mover.Set(c.object);
    void* other = ObjParam(c, sActor);
    void* otherComp = ObjParam(c, sOther);
    void* comp = ObjParam(c, sComp);
    if (other && R::IsLive(other)) r.otherActor.Set(other);
    if (otherComp && R::IsLive(otherComp)) r.otherComp.Set(otherComp);
    if (comp && R::IsLive(comp)) r.comp.Set(comp);
    const int32_t off = sHit.Of(c.function);
    const int32_t size = R::FindParamSize(c.function, L"Hit");
    ue_wrap::hit_result::Hit hit;
    if (off >= 0 && size > 0 && c.locals && ue_wrap::hit_result::Parse(c.locals + off, size, &hit)) {
        r.at = hit.location;
        r.normal = hit.normal;
    }
    return sg::Verdict::Run;
}

// A uint8 return of a UPrimitiveComponent function, with an optional uint8 argument; -1 when it does not resolve or run.
int CompU8(void* comp, const wchar_t* fn, const wchar_t* argName, int arg) {
    void* f = comp ? R::FindDispatchFunctionCached(R::ClassOf(comp), fn) : nullptr;
    ue_wrap::ParamFrame frame(f);
    if (!f || !frame.valid()) return -1;
    if (argName && !frame.Set<uint8_t>(argName, static_cast<uint8_t>(arg))) return -1;
    if (!ue_wrap::Call(comp, frame)) return -1;
    return frame.Get<uint8_t>(L"ReturnValue");
}

std::string Narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t ch : w) s.push_back(ch < 128 ? static_cast<char>(ch) : '?');
    return s;
}

std::string NameText(void* obj) { return obj && R::IsLive(obj) ? Narrow(R::ToString(R::NameOf(obj))) : std::string("?"); }

std::string ProfileOf(void* comp) {
    void* f = comp ? R::FindDispatchFunctionCached(R::ClassOf(comp), L"GetCollisionProfileName") : nullptr;
    ue_wrap::ParamFrame frame(f);
    if (!f || !frame.valid() || !ue_wrap::Call(comp, frame)) return "?";
    return Narrow(R::ToString(frame.Get<R::FName>(L"ReturnValue")));
}

const char* ResponseText(int r) { return r == 0 ? "ignore" : r == 1 ? "overlap" : r == 2 ? "block" : "?"; }

float SpeedBefore(const Track& t, uint64_t at) {
    for (size_t i = t.samples.size(); i-- > 0;)
        if (t.samples[i].t <= at) return t.samples[i].speed;
    return -1.f;
}

void Report(const HitRec& r, const Track& t) {
    void* other = r.otherActor.Get();
    void* otherComp = r.otherComp.Get();
    void* comp = r.comp.Get();
    const int objType = CompU8(comp, L"GetCollisionObjectType", nullptr, 0);
    const int response = objType >= 0 ? CompU8(otherComp, L"GetCollisionResponseToChannel", L"Channel", objType) : -1;
    UE_LOGI("[CI-DRILL] throw %s first-hit eid=%u t=%llu other=%s class=%s comp=%s profile=%s response=%s at=(%.1f,%.1f,%.1f) "
            "normal=(%.2f,%.2f,%.2f) speed-before=%.0f", t.role, t.eid, static_cast<unsigned long long>(r.t),
            NameText(other).c_str(), other ? Narrow(R::ClassNameOf(other)).c_str() : "?", NameText(otherComp).c_str(),
            ProfileOf(otherComp).c_str(), ResponseText(response), static_cast<double>(r.at.X), static_cast<double>(r.at.Y),
            static_cast<double>(r.at.Z), static_cast<double>(r.normal.X), static_cast<double>(r.normal.Y),
            static_cast<double>(r.normal.Z), static_cast<double>(SpeedBefore(t, r.t)));
}

}  // namespace

void HitWatchInstall() {
    if (g_installed) return;
    void* cls = R::FindClass(L"prop_drive_C");   // not loaded until the first drive: asked again at the next call
    if (!cls) return;
    const ue_wrap::facts::Resolved fn = ue_wrap::facts::ResolveOn(kRowHit, cls);
    if (!fn.Fn()) {
        UE_LOGW("[CI-DRILL] prop_drive_C has no component-hit handler by that name -- the first hit is not watched");
        g_installed = true;
        return;
    }
    g_installed = true;
    g_live = sg::Watch(fn, kHitTag, &HitPre, nullptr);
    if (!g_live) UE_LOGW("[CI-DRILL] the hit watch on prop_drive_C was refused");
}

bool HitWatchLive() {
    HitWatchInstall();
    return g_live;
}

void HitWatchBegin(void* actor) {
    g_nRecs = 0;
    g_watched[0] = actor;
    g_watched[1] = nullptr;
}

void HitWatchAdd(void* actor) {
    if (actor && actor != g_watched[0]) g_watched[1] = actor;
}

void HitWatchEnd() {
    g_watched[0] = g_watched[1] = nullptr;
    g_nRecs = 0;
}

void HitJudge::Begin(bool releaseKnown) {
    said_ = false;
    scanned_ = 0;
    releaseMs_ = releaseKnown ? cen::UnixMs() : 0;
}

void HitJudge::Tick(const Track& t) {
    if (said_) return;
    // A peer that did not throw sees the release as the prop's speed jumping: the sample before the speed window began.
    for (; !releaseMs_ && scanned_ < t.samples.size(); ++scanned_)
        if (t.samples[scanned_].speed > kReleaseSpeedCmS) releaseMs_ = t.samples[scanned_].t - kSpeedWindowMs - 20;
    if (!releaseMs_) return;
    for (int i = 0; i < g_nRecs; ++i)
        if (g_recs[i].t >= releaseMs_) {
            Report(g_recs[i], t);
            said_ = true;
            return;
        }
    if (cen::UnixMs() > releaseMs_ + kFirstHitWindowMs) {
        UE_LOGI("[CI-DRILL] throw %s first-hit none", t.role);
        said_ = true;
    }
}

void HitJudge::Finish(const Track& t) {
    Tick(t);
    if (!said_) {
        UE_LOGI("[CI-DRILL] throw %s first-hit none%s", t.role, releaseMs_ ? "" : " (no release seen)");
        said_ = true;
    }
}

}  // namespace coop::dev::container_intent_drill::detail
