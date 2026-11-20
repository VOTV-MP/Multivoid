// coop/dev/clean_drill_legs.cpp -- the clean drill's log-only legs after its verdict: the actor bleeds and cleans its blood,
// dips the mop in the bucket and throws a bucket of water out of its hand; see coop/dev/clean_drill.h. Each leg is one step a
// call, and a leg that waits on the world waits on what it needs (a blood mark to clean), with a bound that skips the leg.

#include "clean_drill_detail.h"

#include "coop/cheats/item_catalog.h"
#include "coop/dev/director/director.h"
#include "coop/dev/drill_hand.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"

#include <windows.h>

#include <cctype>
#include <string>

namespace coop::dev::clean_drill::detail {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

constexpr uint64_t kBloodBoundMs = 30000;

enum class Leg { Bleed, BloodMarks, BucketFill, BucketDip, WaterThrow, Done };
Leg g_leg = Leg::Bleed;
uint64_t g_legAt = 0;

uint64_t Now() { return ::GetTickCount64(); }

void Step(const char* what, bool ok) { UE_LOGI("[CLN] step %s ok=%d", what, ok ? 1 : 0); }

template <class T>
T Get(void* obj, const wchar_t* name, T dflt) {
    const int32_t off = obj ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    return off >= 0 ? *reinterpret_cast<const T*>(static_cast<const char*>(obj) + off) : dflt;
}
template <class T>
void Put(void* obj, const wchar_t* name, T v) {
    const int32_t off = obj ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    if (off >= 0) *reinterpret_cast<T*>(static_cast<char*>(obj) + off) = v;
}

void* Bucket(const FVector& anchor) {
    void* best = nullptr;
    float bd = 300.f * 300.f;
    for (void* b : All(L"prop_bucket_C")) {
        FVector p{};
        if (E::TryGetActorLocation(b, p) && Dist2(p, anchor) < bd) {
            bd = Dist2(p, anchor);
            best = b;
        }
    }
    return best;
}

constexpr const wchar_t* kBloodClasses[] = {L"grime_blood_C", L"grime_blood2_C", L"grime_blood2_drip_C"};

// The blood marks within 300 cm of `at`.
std::vector<void*> BloodNear(const FVector& at) {
    std::vector<void*> out;
    for (const wchar_t* cls : kBloodClasses)
        for (void* dec : All(cls)) {
            FVector p{};
            if (E::TryGetActorLocation(dec, p) && Dist2(p, at) <= 300.f * 300.f) out.push_back(dec);
        }
    return out;
}

}  // namespace

std::wstring CatalogRow(const char* part) {
    const auto names = coop::cheats::item_catalog::Names();
    if (!names) return L"";
    for (const std::string& n : *names) {
        std::string low = n;
        for (char& c : low) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
        if (low == part) return std::wstring(n.begin(), n.end());
    }
    return L"";
}

void* InHand(void* me) {
    E::MainPlayerGrabState gs{};
    return (me && E::ReadMainPlayerGrabState(me, gs)) ? gs.holdingActor : nullptr;
}

bool RunLegs(void* me, const FVector& anchor) {
    switch (g_leg) {
    case Leg::Bleed: {   // three drops let fall through the blood master
        void* master = nullptr;
        for (void* o : All(L"bloodDecalMaster_C")) master = o;
        void* const fn = master ? R::FindDispatchFunctionCached(R::ClassOf(master), L"addParticle") : nullptr;
        FVector at{};
        int ran = 0;
        if (fn && E::TryGetActorLocation(me, at))
            for (int k = 0; k < 3; ++k) {
                ue_wrap::ParamFrame f(fn);
                const FVector loc{at.X + 60.f + 40.f * static_cast<float>(k), at.Y + 60.f, at.Z + 60.f};
                if (f.valid() && f.Set<FVector>(L"location", loc) && f.Set<FVector>(L"velocity", {0.f, 0.f, -200.f}) &&
                    f.Set<float>(L"size", 1.f) && f.Set<int32_t>(L"type", 0) && ue_wrap::Call(master, f))
                    ++ran;
            }
        Step((std::string("bled ") + std::to_string(ran) + " drops").c_str(), ran == 3);
        g_leg = Leg::BloodMarks;
        g_legAt = Now();
        return false;
    }
    case Leg::BloodMarks: {   // its blood cleaned away, once the drops have landed as marks
        FVector at{};
        E::TryGetActorLocation(me, at);
        const std::vector<void*> marks = BloodNear(at);
        if (marks.empty() && Now() - g_legAt < kBloodBoundMs) return false;
        void* const mop = InHand(me);
        for (void* dec : marks) {
            int n = 0;
            while (n < 12 && R::IsLive(dec) && Clean(dec, mop, 50.f) && ProcessOf(dec) >= 0.f) ++n;
        }
        Step((std::string("its blood cleaned: ") + std::to_string(marks.size()) + " marks").c_str(), !marks.empty());
        g_leg = Leg::BucketFill;
        return false;
    }
    case Leg::BucketFill: {   // the bucket filled and the view turned onto its water
        void* const b = Bucket(anchor);
        Put<float>(b, L"Height", 35.f);
        CallNoArgs(b, L"upd");
        FVector at{};
        // The mop's click traces from the player's arm along its view, so the view looks at the bucket first.
        if (b && E::TryGetActorLocation(b, at))
            if (void* ctrl = E::GetController(me))
                E::SetControlRotation(ctrl, coop::director::LookAt(E::GetCameraLocation(), {at.X, at.Y, at.Z + 15.f}));
        g_leg = Leg::BucketDip;
        return false;
    }
    case Leg::BucketDip: {   // the mop dipped in the filled bucket
        void* const mop = InHand(me);
        void* const b = Bucket(anchor);
        FVector at{};
        bool ran = false;
        const float before = Get<float>(b, L"Height", -1.f);
        if (b && mop && E::TryGetActorLocation(b, at) &&
            E::WriteMainPlayerHitResult(me, b, ue_wrap::prop::GetStaticMesh(b), at)) {
            void* const fn = R::FindDispatchFunctionCached(R::ClassOf(mop), L"playerHandUse_LMB");
            ue_wrap::ParamFrame f(fn);
            ran = fn && f.valid() && f.Set<void*>(L"player", me) && ue_wrap::Call(mop, f);
        }
        UE_LOGI("[CLN] the bucket's water %.1f -> %.1f at the dip", before, Get<float>(b, L"Height", -1.f));
        Step("the mop dipped in the bucket", ran);
        g_leg = Leg::WaterThrow;
        return false;
    }
    case Leg::WaterThrow: {   // a bucket of water thrown out from the hand
        const std::wstring row = CatalogRow("bucket");
        bool ran = !row.empty() && coop::dev::drill_hand::TakeIntoHandNow(row.c_str(), L"prop_bucket_C");
        void* const hb = InHand(me);
        if (ran && hb) {
            Put<float>(hb, L"Height", 35.f);
            CallNoArgs(hb, L"upd");
            void* const fn = R::FindDispatchFunctionCached(R::ClassOf(hb), L"playerHandUse_LMB");
            ue_wrap::ParamFrame f(fn);
            E::WriteMainPlayerHitResult(me, me, nullptr, {0.f, 0.f, 0.f});
            ran = fn && f.valid() && f.Set<void*>(L"player", me) && ue_wrap::Call(hb, f);
        }
        Step("a bucket of water thrown out from the hand", ran && hb);
        g_leg = Leg::Done;
        return false;
    }
    case Leg::Done:
        return true;
    }
    return true;
}

}  // namespace coop::dev::clean_drill::detail
