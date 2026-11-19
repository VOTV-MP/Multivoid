// coop/dev/clean_drill.cpp -- [dev] grime cleaned by one player and the sounds of it, logged on every peer
// (VOTVCOOP_CLEAN_DRILL=1 on every peer).
//
// The anchor is the save's prop_bucket_C under VOTVCOOP_CLEAN_BUCKET; the targets, the same on every peer, are the decal
// of each of seven grime classes nearest it within 30 m. The ACTOR (VOTVCOOP_CLEAN_ACTOR, `client` = slot 1, or `host`)
// stands beside the bucket with a mop in hand 5 s before VOTVCOOP_CLEAN_DELAY, then cleans as the mop's notify does
// (grime_C::clean, the mop the sponge): t+2 s the first target away; from t+6 s each other target three times, 4 s
// apart; t+34 s three drops of its blood let fall through the blood master; t+42 s the marks within 3 m of it away;
// t+48 s the bucket filled and the view turned onto it, t+49 s the mop's left click on it (its own trace decides the
// dip); t+52 s a bucket of water taken into the hand and its left click, the water thrown out; t+60 s DONE. EVERY PEER:
// each target's process and every grime_blood decal within 8 m of the anchor each second, and every grime decal it
// holds 8 s before t and at t+70 s (`[CLN]`); the sound relay's own lines say which sounds crossed.

#include "coop/dev/clean_drill.h"

#include "coop/cheats/item_catalog.h"
#include "coop/config/config.h"
#include "coop/dev/director/director.h"
#include "coop/dev/drill_hand.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/session/teleport_client.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/engine/engine_nav.h"

#include <windows.h>

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace coop::dev::clean_drill {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace OI = ue_wrap::object_index;
using ue_wrap::FVector;

bool On() {
    static const bool on = coop::config::ReadEnv("VOTVCOOP_CLEAN_DRILL") == "1";
    return on;
}

uint64_t EnvMs(const char* name, uint64_t dflt) {
    const std::string v = coop::config::ReadEnv(name);
    return v.empty() ? dflt : std::strtoull(v.c_str(), nullptr, 10);
}

uint64_t Now() { return ::GetTickCount64(); }
unsigned long long WallMs() {
    FILETIME ft{};
    ::GetSystemTimePreciseAsFileTime(&ft);
    return (((static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) / 10000ull);
}

std::vector<void*> All(const wchar_t* cls) {
    std::vector<void*> out;
    void* const c = OI::ClassByName(cls);
    if (c)
        OI::ForEachInstance(c, [](void* ctx, void* obj, int32_t) {
            if (obj && R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__"))
                static_cast<std::vector<void*>*>(ctx)->push_back(obj);
        }, &out);
    return out;
}

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

bool CallNoArgs(void* obj, const wchar_t* fn) {
    void* const f = obj ? R::FindDispatchFunctionCached(R::ClassOf(obj), fn) : nullptr;
    ue_wrap::ParamFrame frame(f);
    return f && frame.valid() && ue_wrap::Call(obj, frame);
}

float Dist2(const FVector& a, const FVector& b) {
    const float x = a.X - b.X, y = a.Y - b.Y, z = a.Z - b.Z;
    return x * x + y * y + z * z;
}

constexpr const wchar_t* kTargetClasses[] = {L"grime_dyn_C",      L"grime_dusty_C", L"grime_leaky_C",
                                             L"grime_blood2_drip_C", L"grime_coffee_C", L"grime_fallenLeaves_C",
                                             L"grime_grainy_C"};
constexpr int kTargets = static_cast<int>(sizeof(kTargetClasses) / sizeof(kTargetClasses[0]));

struct Target {
    std::wstring cls;
    FVector at{};
    void* actor = nullptr;   // found again at its place each time (a decal that ended is gone)
};
Target g_targets[kTargets];
FVector g_anchor{};
bool g_anchored = false;

// The live decal of `cls` standing at `at` (within 1 cm), or null.
void* DecalAt(const std::wstring& cls, const FVector& at) {
    for (void* d : All(cls.c_str())) {
        FVector p{};
        if (E::TryGetActorLocation(d, p) && Dist2(p, at) < 1.f) return d;
    }
    return nullptr;
}

bool Anchor() {
    const std::string want = coop::config::ReadEnv("VOTVCOOP_CLEAN_BUCKET").empty()
                                 ? std::string("upAbhZM6weqmeddpJE6vHg")
                                 : coop::config::ReadEnv("VOTVCOOP_CLEAN_BUCKET");
    for (void* b : All(L"prop_bucket_C")) {
        const std::wstring k = ue_wrap::prop::GetInteractableKeyString(b);
        if (std::string(k.begin(), k.end()) == want && E::TryGetActorLocation(b, g_anchor)) return true;
    }
    return false;
}

void ChooseTargets() {
    for (int i = 0; i < kTargets; ++i) {
        Target& t = g_targets[i];
        t.cls = kTargetClasses[i];
        float best = 3000.f * 3000.f;
        for (void* d : All(kTargetClasses[i])) {
            FVector p{};
            if (!E::TryGetActorLocation(d, p)) continue;
            const float d2 = Dist2(p, g_anchor);
            if (d2 < best) {
                best = d2;
                t.at = p;
                t.actor = d;
            }
        }
        UE_LOGI("[CLN] target %d %ls at (%.1f,%.1f,%.1f) process=%.1f", i, kTargetClasses[i], t.at.X, t.at.Y, t.at.Z,
                t.actor ? Get<float>(t.actor, L"process", -1.f) : -1.f);
    }
}

void* Bucket() {
    void* best = nullptr;
    float bd = 300.f * 300.f;
    for (void* b : All(L"prop_bucket_C")) {
        FVector p{};
        if (E::TryGetActorLocation(b, p) && Dist2(p, g_anchor) < bd) {
            bd = Dist2(p, g_anchor);
            best = b;
        }
    }
    return best;
}

void* InHand(void* me) {
    E::MainPlayerGrabState gs{};
    return (me && E::ReadMainPlayerGrabState(me, gs)) ? gs.holdingActor : nullptr;
}

// grime_C::clean(sponge, sub, nosound), as the mop's clean notify calls it. True when it ran.
bool Clean(void* decal, void* sponge, float sub) {
    void* const fn = decal ? R::FindDispatchFunctionCached(R::ClassOf(decal), L"clean") : nullptr;
    ue_wrap::ParamFrame f(fn);
    return fn && f.valid() && f.Set<void*>(L"sponge", sponge) && f.Set<float>(L"sub", sub) &&
           f.Set<bool>(L"nosound", false) && ue_wrap::Call(decal, f);
}

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

void Report() {
    const unsigned long long w = WallMs();
    for (int i = 0; i < kTargets; ++i) {
        const Target& t = g_targets[i];
        void* const d = DecalAt(t.cls, t.at);
        if (d) UE_LOGI("[CLN] wall=%llu target %d %ls process=%.2f", w, i, t.cls.c_str(), Get<float>(d, L"process", -1.f));
        else UE_LOGI("[CLN] wall=%llu target %d %ls gone", w, i, t.cls.c_str());
    }
    for (const wchar_t* cls : {L"grime_blood_C", L"grime_blood2_C", L"grime_blood2_drip_C"})
        for (void* d : All(cls)) {
            FVector p{};
            if (!E::TryGetActorLocation(d, p) || Dist2(p, g_anchor) > 800.f * 800.f) continue;
            UE_LOGI("[CLN] wall=%llu blood %ls at (%.0f,%.0f,%.0f) process=%.2f", w, cls, p.X, p.Y, p.Z,
                    Get<float>(d, L"process", -1.f));
        }
}

// Every grime decal this peer holds, by class and place, once (`[CLN] all`), for a set compare across peers.
constexpr const wchar_t* kAllClasses[] = {
    L"grime_asoLogoShadow_C", L"grime_beer_C",  L"grime_blood_C",        L"grime_blood2_C",         L"grime_blood2_drip_C",
    L"grime_coffee_C",        L"grime_crack_C", L"grime_dusty_C",        L"grime_dyn_C",            L"grime_explosionScorch_C",
    L"grime_fallenLeaves_C",  L"grime_grainy_C", L"grime_leaky_C",       L"grime_leaky_rusty_C",    L"grime_leaky_wet_C",
    L"grime_light_C",         L"grime_oil_C",   L"grime_poo_C",          L"grime_C"};
void DumpAll(const char* when) {
    size_t n = 0;
    for (const wchar_t* cls : kAllClasses)
        for (void* dec : All(cls)) {
            FVector p{};
            if (!E::TryGetActorLocation(dec, p)) continue;
            UE_LOGI("[CLN] all %s %ls %.0f %.0f %.0f %.1f", when, cls, p.X, p.Y, p.Z, Get<float>(dec, L"process", -1.f));
            ++n;
        }
    UE_LOGI("[CLN] all %s: %zu decals", when, n);
}
bool g_dumpedBefore = false, g_dumpedAfter = false;

uint64_t g_armedAt = 0, g_reportAt = 0;
int g_step = 0, g_cleanRound = 0;
uint64_t g_nextAt = 0;

void Step(const char* what, bool ok) {
    UE_LOGI("[CLN] step %s ok=%d wall=%llu", what, ok ? 1 : 0, WallMs());
}

}  // namespace

void Tick(coop::net::Session* session) {
    if (!On() || !session || !session->running() || !session->connected()) return;
    const bool host = session->role() == coop::net::Role::Host;
    if (!g_armedAt) {
        g_armedAt = Now();
        UE_LOGI("[CLN] drill armed role=%s", host ? "host" : "client");
        coop::cheats::item_catalog::RequestRefresh();
    }
    if (!g_anchored) {
        if (!Anchor()) return;
        g_anchored = true;
        ChooseTargets();
    }
    const uint64_t t = Now() - g_armedAt;
    const uint64_t d = EnvMs("VOTVCOOP_CLEAN_DELAY", 140000);
    void* const me = coop::players::Registry::Get().Local();
    const std::string who = coop::config::ReadEnv("VOTVCOOP_CLEAN_ACTOR");
    const bool acts = (who == "host") ? host : (!host && coop::players::Registry::Get().LocalPeerId() == 1);

    if (acts && me && g_step == 0 && t + 5000 >= d) {   // beside the bucket, the mop in hand
        g_step = 1;
        FVector floor{};
        const float half = E::GetActorCharacterHalfHeight(me);
        coop::teleport_client::ApplyArgs args{};
        const bool nav = E::ProjectToNav(me, {g_anchor.X + 150.f, g_anchor.Y, g_anchor.Z}, {150.f, 150.f, 400.f}, &floor);
        args.locX = floor.X;
        args.locY = floor.Y;
        args.locZ = floor.Z + half;
        const bool moved = nav && half > 0.f && coop::teleport_client::ApplyLocally(args);
        const std::wstring mop = CatalogRow("mop");
        const bool inHand = !mop.empty() && coop::dev::drill_hand::TakeIntoHandNow(mop.c_str(), L"prop_mop_C");
        UE_LOGI("[CLN] the actor stands beside the bucket (moved=%d) with the mop '%ls' in hand (ok=%d)", moved ? 1 : 0,
                mop.c_str(), inHand ? 1 : 0);
        g_nextAt = d + 2000;
    }
    if (acts && me && g_step >= 1 && g_step < 100 && t >= g_nextAt) {
        void* const mop = InHand(me);
        if (g_step == 1) {   // the first target cleaned away
            void* const dec = DecalAt(g_targets[0].cls, g_targets[0].at);
            int n = 0;
            while (dec && n < 12 && Clean(dec, mop, 50.f) && R::IsLive(dec) && Get<float>(dec, L"process", -1.f) >= 0.f) ++n;
            Step("target 0 cleaned away", dec != nullptr);
            g_step = 2;
            g_cleanRound = 0;
            g_nextAt = d + 6000;
        } else if (g_step >= 2 && g_step < 1 + kTargets) {   // each other target: three cleans, 400 ms apart
            const int i = g_step - 1;
            void* const dec = DecalAt(g_targets[i].cls, g_targets[i].at);
            const bool ok = dec && Clean(dec, mop, 10.f);
            if (++g_cleanRound >= 3) {
                Step((std::string("target ") + std::to_string(i) + " cleaned three times").c_str(), ok);
                g_cleanRound = 0;
                ++g_step;
                g_nextAt = d + 6000 + 4000ull * static_cast<uint64_t>(g_step - 2);
            } else {
                g_nextAt = t + 400;
            }
        } else if (g_step == 1 + kTargets) {   // bleed
            g_step = 50;
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
            g_nextAt = d + 42000;
        } else if (g_step == 50) {   // its blood cleaned away
            g_step = 51;
            FVector at{};
            E::TryGetActorLocation(me, at);
            int cleaned = 0;
            for (const wchar_t* cls : {L"grime_blood_C", L"grime_blood2_C", L"grime_blood2_drip_C"})
                for (void* dec : All(cls)) {
                    FVector p{};
                    if (!E::TryGetActorLocation(dec, p) || Dist2(p, at) > 300.f * 300.f) continue;
                    int n = 0;
                    while (n < 12 && R::IsLive(dec) && Clean(dec, mop, 50.f) && Get<float>(dec, L"process", -1.f) >= 0.f) ++n;
                    ++cleaned;
                }
            Step((std::string("its blood cleaned: ") + std::to_string(cleaned) + " marks").c_str(), cleaned > 0);
            g_nextAt = d + 48000;
        } else if (g_step == 51) {   // the bucket filled and the view turned onto its water
            g_step = 54;
            void* const b = Bucket();
            Put<float>(b, L"Height", 35.f);
            CallNoArgs(b, L"upd");
            FVector at{};
            // The mop's click traces from the player's arm along its view, so the view looks at the bucket first.
            if (b && E::TryGetActorLocation(b, at))
                if (void* ctrl = E::GetController(me))
                    E::SetControlRotation(ctrl, coop::director::LookAt(E::GetCameraLocation(), {at.X, at.Y, at.Z + 15.f}));
            g_nextAt = d + 49000;
        } else if (g_step == 54) {   // the mop dipped in the filled bucket
            g_step = 52;
            void* const b = Bucket();
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
            g_nextAt = d + 52000;
        } else if (g_step == 52) {   // a bucket of water thrown out from the hand
            g_step = 53;
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
            g_nextAt = d + 60000;
        } else if (g_step == 53) {
            g_step = 100;
            UE_LOGI("[CLN] DONE wall=%llu", WallMs());
        }
    }
    if (!g_dumpedBefore && t + 8000 >= d) {
        g_dumpedBefore = true;
        DumpAll("before");
    }
    if (!g_dumpedAfter && t >= d + 70000) {
        g_dumpedAfter = true;
        DumpAll("after");
    }
    if (t + 10000 < d || Now() < g_reportAt) return;
    g_reportAt = Now() + 1000;
    Report();
}

}  // namespace coop::dev::clean_drill
