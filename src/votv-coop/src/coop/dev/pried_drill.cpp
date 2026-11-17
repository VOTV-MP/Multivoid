// coop/dev/pried_drill.cpp -- see coop/dev/pried_drill.h.

#include "coop/dev/pried_drill.h"

#include "coop/config/config.h"
#include "coop/dev/drill_chat.h"
#include "coop/net/net_clock.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/roster.h"
#include "coop/props/prop_key_index.h"
#include "coop/props/prop_pry_sync.h"
#include "coop/props/prop_snapshot.h"
#include "coop/save/join_window_baseline.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace coop::dev::pried_drill {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace PR = ue_wrap::prop;
namespace DC = coop::dev::drill_chat;
using ue_wrap::FVector;

enum class Kind { Off, Plank, Radiator };
struct Arm {
    Kind kind;
    bool clientActs;
};

Arm ArmOf() {
    static const Arm a = [] {
        const std::string v = coop::config::ResolveEnum(::coop::config_registry::rows::pried_drill);
        const bool client = v.size() > 7 && v.compare(v.size() - 7, 7, "_client") == 0;
        const Kind k = v.rfind("plank", 0) == 0 ? Kind::Plank
                     : v.rfind("radiator", 0) == 0 ? Kind::Radiator : Kind::Off;
        return Arm{k, client};
    }();
    return a;
}

bool LocalActs() { return ArmOf().clientActs != coop::roster::LocalIsHost(); }
char Who() { return coop::roster::LocalIsHost() ? 'H' : 'C'; }

constexpr const wchar_t* kPlankClass    = L"prop_pryablePlank_C";
constexpr const wchar_t* kRadiatorClass = L"radiator_C";

constexpr uint64_t kRestMinMs   = 1500;
constexpr uint64_t kRestMaxMs   = 25000;  // a radiator's four constraints break up to 3 s apart
constexpr uint64_t kSettleMs    = 3000;
constexpr uint64_t kJudgeWaitMs = 20000;
constexpr uint64_t kBoundMs     = 90000;  // a step that waits on the other peer
// Each peer simulates a freed prop's fall itself: measured plank pairs rested 1 to 65 cm apart after
// falls of 13 to 137 cm. A radiator hangs on its constraints for seconds after its pry, so only a
// plank is asked to have moved.
constexpr float    kApartTolCm  = 100.f;
constexpr float    kMovedMinCm  = 5.f;

const char* const kSayTarget = "pried target ";
const char* const kSayReady  = "pried ready";
const char* const kSayRest   = "pried rest ";

enum class Step { WaitJoin, Named, Acted, Settle, Judge, Done };
Step g_step = Step::WaitJoin;
uint64_t g_stepMs = 0;
ue_wrap::CachedObjRef g_target;
std::wstring g_key;
FVector g_start{};
coop::prop_pry_sync::Stats g_statsAtStart{};

float Dist(const FVector& a, const FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool Stuck(void* o) { return PR::IsStatic(o) || PR::IsFrozen(o); }

void Go(Step s) {
    g_step = s;
    g_stepMs = coop::net::NowMs();
}
uint64_t InStepMs() { return coop::net::NowMs() - g_stepMs; }

void Verdict(bool pass, const char* fmt, ...) {
    char msg[320];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (pass) UE_LOGI("[PRIED-DRILL] [%c] PASS %s", Who(), msg);
    else      UE_LOGW("[PRIED-DRILL] [%c] FAIL %s", Who(), msg);
    Go(Step::Done);
}

// This peer's join is over: a client's own, or on the host one client's with its window closed.
bool JoinOver(coop::net::Session& s) {
    if (!coop::roster::LocalIsHost())
        return coop::net_pump::HasAnnouncedWorldReady() &&
               coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle;
    for (int slot = 1; slot < static_cast<int>(coop::players::kMaxPeers); ++slot)
        if (s.IsSlotWorldReady(slot) && coop::prop_snapshot::IsBracketClosed(slot) &&
            !coop::join_window_baseline::IsLateWindowOpen(slot))
            return true;
    return false;
}

// One walk of the object array: the first key, in order, among the live props of `cls` or below,
// the stuck ones first.
std::wstring PickByClass(const wchar_t* clsName) {
    void* cls = R::FindClass(clsName);
    std::vector<std::wstring> stuck, loose;
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; cls && i < n; ++i) {
        void* o = R::ObjectAt(i);
        if (!o || !R::IsLive(o)) continue;
        void* oc = R::ClassOf(o);
        if (!oc || !(oc == cls || R::IsDescendantOfAny(oc, &cls, 1))) continue;
        if (R::NameStartsWith(R::NameOf(o), L"Default__")) continue;
        std::wstring key = PR::GetInteractableKeyString(o);
        if (key.empty() || key == L"None") continue;
        (Stuck(o) ? stuck : loose).push_back(std::move(key));
    }
    auto& from = stuck.empty() ? loose : stuck;
    UE_LOGI("[PRIED-DRILL] [%c] %ls: %zu stuck, %zu loose", Who(), clsName, stuck.size(), loose.size());
    if (from.empty()) return {};
    std::sort(from.begin(), from.end());
    return from.front();
}

std::string Narrow(const std::wstring& w) { return std::string(w.begin(), w.end()); }
std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

bool BindTarget(const std::wstring& key) {
    void* o = coop::prop_key_index::Find(key);
    if (!o || !R::IsLive(o) || !E::TryGetActorLocation(o, g_start)) return false;
    g_target.Set(o);
    g_key = key;
    UE_LOGI("[PRIED-DRILL] [%c] target key='%ls' cls='%ls' at (%.1f, %.1f, %.1f) static=%d frozen=%d", Who(),
            key.c_str(), R::ClassNameOf(o).c_str(), g_start.X, g_start.Y, g_start.Z, PR::IsStatic(o) ? 1 : 0,
            PR::IsFrozen(o) ? 1 : 0);
    return true;
}

// ---- The plank and the radiator: a prop both worlds loaded ------------------------------------

void TickPlaced(coop::net::Session& s) {
    const bool acts = LocalActs();
    const wchar_t* cls = ArmOf().kind == Kind::Plank ? kPlankClass : kRadiatorClass;
    switch (g_step) {
    case Step::WaitJoin: {
        if (!JoinOver(s)) return;
        if (!acts) {
            const std::wstring key = PickByClass(cls);
            if (key.empty() || !BindTarget(key)) return Verdict(false, "no keyed prop of the class stands here");
            g_statsAtStart = coop::prop_pry_sync::GetStats();
            DC::Say(std::string(kSayTarget) + Narrow(key));
            return Go(Step::Named);
        }
        std::string rest;
        if (!DC::SeenRestOf(kSayTarget, &rest)) return;
        if (!BindTarget(Widen(rest))) return Verdict(false, "the named prop does not resolve here");
        const bool ran = PR::CallCrowbarOpen(g_target.Get());
        void* t = g_target.Get();
        UE_LOGI("[PRIED-DRILL] [%c] PRIED key='%ls' crowbarOpen=%d -- static=%d frozen=%d after", Who(),
                g_key.c_str(), ran ? 1 : 0, t && PR::IsStatic(t) ? 1 : 0, t && PR::IsFrozen(t) ? 1 : 0);
        if (!ran || !t || Stuck(t)) return Verdict(false, "the pry did not free the prop on the acting peer");
        return Go(Step::Acted);
    }
    case Step::Named: {  // the watching peer waits for the acting peer's rest
        std::string rest;
        if (!DC::SeenRestOf(kSayRest, &rest)) {
            if (InStepMs() > kBoundMs + kRestMaxMs) Verdict(false, "the acting peer never said where its copy rests");
            return;
        }
        return Go(Step::Judge);
    }
    case Step::Acted: {  // the acting peer waits for its copy to rest
        void* t = g_target.Get();
        if (!t) return Verdict(false, "the pried prop is gone on the acting peer");
        const bool rested = InStepMs() > kRestMinMs && E::IsActorRootBodyAtRest(t);
        if (!rested && InStepMs() < kRestMaxMs) return;
        UE_LOGI("[PRIED-DRILL] [%c] %s", Who(), rested ? "RESTED" : "NOT AT REST after the wait");
        return Go(Step::Settle);
    }
    case Step::Settle: {
        if (InStepMs() < kSettleMs) return;
        void* t = g_target.Get();
        FVector at{};
        if (!t || !E::TryGetActorLocation(t, at)) return Verdict(false, "the pried prop does not read at its rest");
        char line[96];
        std::snprintf(line, sizeof(line), "%s%.0f %.0f %.0f", kSayRest, at.X, at.Y, at.Z);
        DC::Say(line);
        UE_LOGI("[PRIED-DRILL] [%c] ACTOR DONE at (%.1f, %.1f, %.1f), %.1f cm from its start", Who(), at.X, at.Y, at.Z,
                Dist(at, g_start));
        return Go(Step::Done);
    }
    case Step::Judge: {
        void* t = g_target.Get();
        if (!t) return Verdict(false, "this copy is gone");
        if (!E::IsActorRootBodyAtRest(t) && InStepMs() < kJudgeWaitMs) return;
        std::string rest;
        float ax = 0.f, ay = 0.f, az = 0.f;
        DC::SeenRestOf(kSayRest, &rest);
        if (std::sscanf(rest.c_str(), "%f %f %f", &ax, &ay, &az) < 3) return Verdict(false, "the rest line does not parse");
        FVector at{};
        if (!E::TryGetActorLocation(t, at)) return Verdict(false, "this copy's place does not read");
        const auto now = coop::prop_pry_sync::GetStats();
        const unsigned long long applied = now.applied - g_statsAtStart.applied;
        const float moved = Dist(at, g_start), apart = Dist(at, FVector{ax, ay, az});
        if (applied < 1) return Verdict(false, "the pry lane replayed nothing here (moved %.1f cm)", moved);
        if (Stuck(t)) return Verdict(false, "this copy is still static or frozen after the replay");
        if (ArmOf().kind == Kind::Plank && moved < kMovedMinCm)
            return Verdict(false, "this copy never moved: %.1f cm, apart %.1f cm", moved, apart);
        if (apart > kApartTolCm)
            return Verdict(false, "the copies rest %.1f cm apart (tolerance %.0f) after moving %.1f cm", apart,
                           kApartTolCm, moved);
        return Verdict(true, "replayed %llu, freed, moved %.1f cm, the copies rest %.1f cm apart (tolerance %.0f)",
                       applied, moved, apart, kApartTolCm);
    }
    case Step::Done:
        return;
    }
}

}  // namespace

bool IsEnabled() { return ArmOf().kind != Kind::Off; }

void Tick(coop::net::Session* session) {
    if (!IsEnabled() || !session || !session->connected() || g_step == Step::Done) return;
    // The chat history records from its first read on, so it is read from the first pass: a line the
    // other peer says before this one is ready to act on it is still found.
    DC::SeenHas(kSayReady);
    TickPlaced(*session);
}

void OnDisconnect() {
    g_step = Step::WaitJoin;
    g_stepMs = 0;
    g_target.Reset();
    g_key.clear();
}

}  // namespace coop::dev::pried_drill
