// coop/dev/atv_gate_drill_can.cpp -- the carried-can arm of the ATV drill (atv_gate_drill=carried_can): step 1c, a client pours a
// big full gas can it CARRIES by the grab, which no hotbar hand holds, into the ATV, and the host's judge must take it as it does
// the plain can in a hand. The host spawns the can on the ground beside itself and waits for it to rest; the client's director
// walks to it and the game's own grab takes it (director::AimedGrab, the use key's chain); the host's copy of the can is then
// driven by that hold, which is what its judge reads as `carried`; the pour is the ATV's gascanFuel called with the carried can,
// as the woodchipper drill's pour calls the chipper's. Step 1's events decide it (the host's accepted request and the client's
// answer, atv_gate_drill_steps.cpp).

#include "atv_gate_drill_detail.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/atv_drive.h"
#include "coop/dev/director/aimed_grab.h"
#include "coop/props/prop_key_index.h"
#include "coop/props/remote_prop.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <memory>
#include <sstream>
#include <string>

namespace coop::dev::atv_gate_drill::detail {
namespace {

namespace PS = harness::autotest::peer_signal;
namespace DR = coop::director;
namespace E  = ue_wrap::engine;
namespace R  = ue_wrap::reflection;
using ue_wrap::FVector;

constexpr const wchar_t* kBigCanPath = L"/Game/objects/prop_gascan_big_full.prop_gascan_big_full_C";
constexpr float kSpawnOutCm = 100.f;   // the can is set this far from the host, away from the ATV, and falls to the ground
constexpr float kRestCm = 2.f;         // a can that moved less than this between two readings is still
constexpr int kRestReadings = 6;       // this many still readings in a row, 100 ms apart, are a can at rest
constexpr float kCopyCm = 30.f;        // the client's copy of the can stands within this of the host's
// The grab's stand: 70 cm round the can, taken within 30 cm. The player's capsule and the can touch at about 60 cm, so the
// walker that arrives in contact still counts (a target tight on the contact ring was never reached: the walker pushed the can
// across it), and 100 cm is inside the use key's horizontal reach of about 113 cm.
constexpr float kRingCm = 70.f;
constexpr float kRingTolCm = 30.f;
constexpr int kGrabTries = 6;
constexpr int kGrabWaitS = 600;        // the host's wait for the client's walk and grab

std::wstring g_canKey;   // HOST: the spawned can's key

float Dist(const FVector& a, const FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// CLIENT: the director walks beside the can (a different side each try) and the game's grab takes it, ticked until it answers.
bool GrabCan(const std::wstring& key, std::string& why) {
    std::string last = "no try ran";
    for (int attempt = 0; attempt < kGrabTries && !Aborted(); ++attempt) {
        FVector at{}, mine{};
        bool read = false;
        OnGt([&] {
            void* p = coop::prop_key_index::Find(key);
            read = p && E::TryGetActorLocation(p, at) && E::TryGetActorLocation(Me(), mine);
        });
        if (!read) {
            why = "the big can's copy, or this player, is gone";
            return false;
        }
        // The stand is on the can's side facing the walker (a different side each try), so the route to it does not pass
        // through the can; the can is the walk's own prop, which the director's blocker delete leaves standing.
        const float a = std::atan2(mine.Y - at.Y, mine.X - at.X) + 0.6f * static_cast<float>(attempt);
        if (!coop::dev::atv_drive::WalkTo({at.X + kRingCm * std::cos(a), at.Y + kRingCm * std::sin(a), at.Z}, kRingTolCm,
                                          /*carry=*/false, "the big can", last, key))
            continue;
        std::shared_ptr<DR::AimedGrab> grab;
        OnGt([&] {
            void* me = Me();
            void* p = coop::prop_key_index::Find(key);
            if (me && p) grab = std::make_shared<DR::AimedGrab>(me, p);
        });
        DR::GrabState st = DR::GrabState::Failed;
        for (int i = 0; grab && i < 1200 && !Aborted(); ++i) {
            OnGt([&] { st = grab->Tick(); });
            if (st != DR::GrabState::Working) break;
            ::Sleep(16);
        }
        if (st == DR::GrabState::Grabbed) return true;
        last = grab ? grab->Why() : "no can or player";
        UE_LOGI("atv-drill: grab try %d of the big can failed (%s)", attempt + 1, last.c_str());
    }
    why = "the grab took no big can in " + std::to_string(kGrabTries) + " tries (" + last + ")";
    return false;
}

bool ClientGrabs(Ctx& c, std::string& why) {
    std::string t;
    // Not the step's host fact ("<id>_f_host"): the host's decision writes that one, for the observer, with the vitals.
    if (!AwaitMark(c.id + "_can", t, kEventS)) {
        why = "the host named no can";
        return false;
    }
    std::istringstream in(t);
    std::string k;
    FVector at{};
    if (!(in >> k >> at.X >> at.Y >> at.Z)) {
        why = "the host's fact about the can does not parse: " + t;
        return false;
    }
    const std::wstring key(k.begin(), k.end());
    const bool standing = WaitUntil("this client's copy of the big can stands where the host's does", 60, [&] {
        bool there = false;
        OnGt([&] {
            FVector now{};
            void* p = coop::prop_key_index::Find(key);
            there = p && E::TryGetActorLocation(p, now) && Dist(now, at) <= kCopyCm;
        });
        return there;
    });
    if (!standing) {
        why = "this client's copy of the big can does not stand where the host's does";
        return false;
    }
    return GrabCan(key, why);
}

// HOST: the client's hold is applied when the host's copy of the can is driven by slot `kClient`.
bool HostAwaitsHold(Ctx& c, std::string& why) {
    std::string t;
    if (!AwaitMark(c.id + "_grabbed", t, kGrabWaitS)) {
        why = "the client reported no grab";
        return false;
    }
    if (t != "ok") {
        why = t;
        return false;
    }
    const bool driven = WaitUntil("the host's copy of the big can is driven by the client's hold", 30, [&] {
        bool is = false;
        OnGt([&] {
            void* p = coop::prop_key_index::Find(g_canKey);
            is = p && coop::remote_prop::IsDrivenBy(p, static_cast<int>(kClient));
        });
        return is;
    });
    if (!driven) why = "the client's hold did not drive the host's copy of the big can";
    return driven;
}

}  // namespace

bool CanArm() { return coop::config::ResolveEnum(::coop::config_registry::rows::atv_gate_drill) == "carried_can"; }

bool PreludeCarriedCan(Ctx& c, std::string& why) {
    if (!SeedHost(c.key, 20.f, 50.f, why)) return false;
    ue_wrap::CachedObjRef can;
    OnGt([&] {
        void* me = Me();
        void* atv = Atv(c.key);
        void* cls = ue_wrap::asset_load::LoadObjectByPath(kBigCanPath);
        FVector mine{}, at{};
        if (!me || !atv || !cls || !E::TryGetActorLocation(me, mine) || !E::TryGetActorLocation(atv, at)) return;
        float dx = mine.X - at.X, dy = mine.Y - at.Y;
        const float len = std::sqrt(dx * dx + dy * dy);
        dx = len > 1.f ? dx / len : 1.f;
        dy = len > 1.f ? dy / len : 0.f;
        can.Set(E::SpawnActor(cls, {mine.X + dx * kSpawnOutCm, mine.Y + dy * kSpawnOutCm, mine.Z + 20.f}));
    });
    if (!can.Raw()) {
        why = "the big full gas can could not be spawned beside the host";
        return false;
    }
    std::wstring key;
    if (!WaitUntil("the big can's key", 10, [&] {
            OnGt([&] {
                if (void* p = can.Get()) key = ue_wrap::prop::GetKeyString(p);
            });
            return !key.empty() && key != L"None";
        })) {
        why = "the big can has no key";
        return false;
    }
    FVector last{}, now{};
    int still = 0;
    if (!WaitUntil("the big can comes to rest", 30, [&] {
            bool read = false;
            OnGt([&] {
                void* p = can.Get();
                read = p && E::TryGetActorLocation(p, now);
            });
            if (!read) return false;
            still = Dist(now, last) < kRestCm ? still + 1 : 0;
            last = now;
            return still >= kRestReadings;
        })) {
        why = "the big can did not come to rest";
        return false;
    }
    g_canKey = key;
    char text[160];
    std::snprintf(text, sizeof(text), "%s %.1f %.1f %.1f", std::string(key.begin(), key.end()).c_str(), now.X, now.Y, now.Z);
    UE_LOGI("atv-drill: the host spawned a big full gas can '%ls' at rest at %.0f,%.0f,%.0f", key.c_str(), now.X, now.Y, now.Z);
    PS::Mark((c.id + "_can").c_str(), text);
    return true;
}

bool SetupCarriedCan(Ctx& c, std::string& why) {
    if (c.me == kObserver) return true;
    if (c.me == kHost) return HostAwaitsHold(c, why);
    const bool ok = ClientGrabs(c, why);
    PS::Mark((c.id + "_grabbed").c_str(), ok ? std::string("ok") : "fail: " + why);
    return ok;
}

bool ActPourCarried(Ctx& c, std::string& why) {
    bool carried = false, ran = false;
    OnGt([&] {
        void* me = Me();
        void* can = me ? DR::Grabbing(me) : nullptr;
        void* atv = Atv(c.key);
        void* fn = atv ? R::FindDispatchFunctionCached(R::ClassOf(atv), L"gascanFuel") : nullptr;
        carried = can != nullptr;
        if (!can || !fn) return;
        ue_wrap::ParamFrame f(fn);
        ran = f.valid() && f.Set<void*>(L"gascan", can) && f.Set<void*>(L"player", me) && f.Set<bool>(L"back", false) &&
              ue_wrap::Call(atv, f);
    });
    if (!ran) {
        why = carried ? "the ATV's gascanFuel did not run with the carried can" : "the player carries nothing";
        return false;
    }
    UE_LOGI("atv-drill: this client poured the big can it carries into the ATV");
    return true;
}

}  // namespace coop::dev::atv_gate_drill::detail
