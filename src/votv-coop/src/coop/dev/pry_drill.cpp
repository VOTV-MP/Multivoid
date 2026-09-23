// coop/dev/pry_drill.cpp -- see coop/dev/pry_drill.h.

#include "coop/dev/pry_drill.h"

#include "coop/config/config.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"  // kMaxPeers
#include "coop/player/roster.h"
#include "coop/props/prop_snapshot.h"
#include "coop/save/join_window_baseline.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace coop::dev::pry_drill {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace PR = ue_wrap::prop;
namespace sg = ue_wrap::script_gate;

enum class Arm { Off, Host, Client, Join };
enum class Step { WaitJoin, Rest, Done, Invalid };

Arm ArmOf() {
    static const Arm a = [] {
        const std::string v = coop::config::ResolveEnum(::coop::config_registry::rows::pry_drill);
        return v == "host" ? Arm::Host : v == "client" ? Arm::Client : v == "join" ? Arm::Join : Arm::Off;
    }();
    return a;
}

// The peer that pries: the client in the client arm, the host otherwise.
bool LocalActs() { return (ArmOf() == Arm::Client) != coop::roster::LocalIsHost(); }
char Who() { return coop::roster::LocalIsHost() ? 'H' : 'C'; }

constexpr int   kWatchEveryTicks = 15;
constexpr float kWatchMoveCm     = 3.f;
constexpr int   kRestMinTicks    = 30;
constexpr int   kRestMaxTicks    = 600;   // ~10 s for a pried sign to come to rest
constexpr int   kTagHint         = 0x48494E54;  // 'HINT'

float Dist(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

void* g_pryClass = nullptr;  // prop_wallAttachable_pryable_C

bool IsPryable(void* o) {
    void* cls = o ? R::ClassOf(o) : nullptr;
    return cls && g_pryClass && R::IsDescendantOfAny(cls, &g_pryClass, 1);
}

bool Stuck(void* o) { return PR::IsStatic(o) || PR::IsFrozen(o); }

// ---- The hint count: every addHint a Blueprint runs here, whatever shows it -------------------------

int g_hints = 0;
bool g_hintWatch = false;

sg::Verdict OnHintPre(const sg::Call&) {
    ++g_hints;
    return sg::Verdict::Run;
}

// ---- The watch: both peers, every pryable ------------------------------------------------------------

struct Watched {
    ue_wrap::CachedObjRef ref;
    std::wstring key;
    ue_wrap::FVector start{}, printed{};
    bool statiq = false, frozen = false, gone = false;
};
std::vector<Watched> g_watched;
bool g_watchArmed = false;
int  g_watchTick = 0;

// The first key, in order, among the pryables stuck at the watch's arm: both peers load one world,
// so both pick the same sign without a message between them.
std::wstring PickKey() {
    std::vector<std::wstring> keys;
    for (const auto& w : g_watched)
        if ((w.statiq || w.frozen) && !w.key.empty() && w.key != L"None") keys.push_back(w.key);
    if (keys.empty()) return {};
    std::sort(keys.begin(), keys.end());
    return keys.front();
}

void* FindWatched(const std::wstring& key) {
    for (const auto& w : g_watched)
        if (w.key == key) return w.ref.Get();
    return nullptr;
}

// One walk of the object array, when the watch arms, never again.
void ArmWatch(char who) {
    g_watched.clear();
    if (!g_pryClass) g_pryClass = R::FindClass(L"prop_wallAttachable_pryable_C");
    int stuck = 0;
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; g_pryClass && i < n; ++i) {
        void* o = R::ObjectAt(i);
        if (!o || !R::IsLive(o) || !IsPryable(o)) continue;
        if (R::NameStartsWith(R::NameOf(o), L"Default__")) continue;
        Watched w;
        w.ref.Set(o);
        w.key = PR::GetInteractableKeyString(o);
        w.start = w.printed = E::GetActorLocation(o);
        w.statiq = PR::IsStatic(o);
        w.frozen = PR::IsFrozen(o);
        if (w.statiq || w.frozen) ++stuck;
        UE_LOGI("[PRY-DRILL] [%c] HAS key='%ls' cls='%ls' at (%.1f, %.1f, %.1f) static=%d frozen=%d", who,
                w.key.c_str(), R::ClassNameOf(o).c_str(), w.start.X, w.start.Y, w.start.Z, w.statiq ? 1 : 0,
                w.frozen ? 1 : 0);
        g_watched.push_back(std::move(w));
    }
    UE_LOGI("[PRY-DRILL] [%c] watching %zu pryables (%d stuck)", who, g_watched.size(), stuck);
    g_watchArmed = true;
}

void TickWatch(char who) {
    if (!g_watchArmed || ++g_watchTick < kWatchEveryTicks) return;
    g_watchTick = 0;
    for (auto& w : g_watched) {
        if (w.gone) continue;
        void* o = w.ref.Get();
        if (!o) {
            w.gone = true;
            UE_LOGI("[PRY-DRILL] [%c] GONE key='%ls'", who, w.key.c_str());
            continue;
        }
        const ue_wrap::FVector at = E::GetActorLocation(o);
        const bool statiq = PR::IsStatic(o), frozen = PR::IsFrozen(o);
        if (Dist(at, w.printed) < kWatchMoveCm && statiq == w.statiq && frozen == w.frozen) continue;
        w.printed = at;
        w.statiq = statiq;
        w.frozen = frozen;
        UE_LOGI("[PRY-DRILL] [%c] key='%ls' at (%.1f, %.1f, %.1f) fromStart=%.1fcm static=%d frozen=%d", who,
                w.key.c_str(), at.X, at.Y, at.Z, Dist(at, w.start), statiq ? 1 : 0, frozen ? 1 : 0);
    }
}

// The hand's unstick on this peer's copy -- the component's unstick without the tool, which the
// receiver of a grab ran before -- on the sign the acting peer will pry. A pryable refuses it.
void RunHandUnstick(char who) {
    const std::wstring key = PickKey();
    void* o = key.empty() ? nullptr : FindWatched(key);
    if (!o) {
        UE_LOGW("[PRY-DRILL] [%c] HAND UNSTICK skipped -- no stuck pryable here", who);
        return;
    }
    const int32_t off = R::FindPropertyOffset(R::ClassOf(o), L"comp_wallAttachable");
    void* comp = off >= 0 ? *reinterpret_cast<void* const*>(reinterpret_cast<uint8_t*>(o) + off) : nullptr;
    void* fn = comp ? R::FindDispatchFunctionCached(R::ClassOf(comp), L"unstick") : nullptr;
    bool ok = false;
    const int before = g_hints;
    if (fn) {
        ue_wrap::ParamFrame f(fn);
        ok = f.valid() && f.Set<bool>(L"withTool", false) && ue_wrap::Call(comp, f);
    }
    UE_LOGI("[PRY-DRILL] [%c] HAND UNSTICK key='%ls' unstick(withTool=false)=%d -- static=%d frozen=%d after, "
            "hints shown %d", who, key.c_str(), ok ? 1 : 0, PR::IsStatic(o) ? 1 : 0, PR::IsFrozen(o) ? 1 : 0,
            g_hints - before);
}

// ---- The acting peer's steps -------------------------------------------------------------------

Step g_step = Step::WaitJoin;
ue_wrap::CachedObjRef g_target;
std::wstring g_targetKey;
ue_wrap::FVector g_targetStart{};
int g_stepTicks = 0;

void Invalid(const char* why) {
    UE_LOGW("[PRY-DRILL] INVALID %s", why);
    g_step = Step::Invalid;
}

void Go(Step s) {
    g_step = s;
    g_stepTicks = 0;
}

// When the acting peer may pry. host: a client's join and its join window are over, so the pry
// reaches it through the stick lane rather than as the join's position correction. join: a joiner
// whose world was captured and has not come up, so the pry lands inside its load. client: the
// client's own join is over.
bool ActorMayStart(coop::net::Session& s) {
    if (ArmOf() == Arm::Client)
        return coop::net_pump::HasAnnouncedWorldReady() &&
               coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle;
    for (int slot = 1; slot < static_cast<int>(coop::players::kMaxPeers); ++slot) {
        if (ArmOf() == Arm::Join) {
            if (coop::join_window_baseline::HasCapture(slot) && !s.IsSlotWorldReady(slot)) return true;
        } else if (s.IsSlotWorldReady(slot) && coop::prop_snapshot::IsBracketClosed(slot) &&
                   !coop::join_window_baseline::IsLateWindowOpen(slot)) {
            return true;
        }
    }
    return false;
}

void Pry() {
    g_targetKey = PickKey();
    void* t = g_targetKey.empty() ? nullptr : FindWatched(g_targetKey);
    if (!t) { Invalid("no stuck pryable with a key"); return; }
    g_target.Set(t);
    g_targetStart = E::GetActorLocation(t);
    UE_LOGI("[PRY-DRILL] [%c] target key='%ls' cls='%ls' at (%.1f, %.1f, %.1f)", Who(), g_targetKey.c_str(),
            R::ClassNameOf(t).c_str(), g_targetStart.X, g_targetStart.Y, g_targetStart.Z);
    // crowbarOpen(pryingCrowbar): no crowbar, so the kick along its axis is zero.
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(t), L"crowbarOpen");
    bool ok = false;
    if (fn) {
        ue_wrap::ParamFrame f(fn);
        ok = f.valid() && ue_wrap::Call(t, f);
    }
    UE_LOGI("[PRY-DRILL] [%c] PRIED key='%ls' crowbarOpen=%d -- static=%d frozen=%d after", Who(),
            g_targetKey.c_str(), ok ? 1 : 0, PR::IsStatic(t) ? 1 : 0, PR::IsFrozen(t) ? 1 : 0);
    if (!ok || Stuck(t)) { Invalid("the pry did not free the sign here"); return; }
    Go(Step::Rest);
}

void ActStep(coop::net::Session& s) {
    ++g_stepTicks;
    switch (g_step) {
    case Step::WaitJoin:
        if (!ActorMayStart(s)) return;
        if (!g_watchArmed) ArmWatch(Who());
        Pry();
        return;
    case Step::Rest: {
        void* t = g_target.Get();
        const bool rested = t && g_stepTicks > kRestMinTicks && E::IsActorRootBodyAtRest(t);
        if (!rested && g_stepTicks < kRestMaxTicks) return;
        const ue_wrap::FVector at = t ? E::GetActorLocation(t) : ue_wrap::FVector{};
        UE_LOGI("[PRY-DRILL] [%c] %s key='%ls' at (%.1f, %.1f, %.1f) fromStart=%.1fcm", Who(),
                rested ? "RESTED" : "NOT AT REST after the wait", g_targetKey.c_str(), at.X, at.Y, at.Z,
                Dist(at, g_targetStart));
        UE_LOGI("[PRY-DRILL] ACTOR DONE");
        Go(Step::Done);
        return;
    }
    case Step::Done:
    case Step::Invalid:
        return;
    }
}

}  // namespace

bool IsEnabled() { return ArmOf() != Arm::Off; }

void Tick(coop::net::Session* session) {
    if (!IsEnabled() || !session || !session->connected()) return;
    if (!g_hintWatch) g_hintWatch = sg::WatchName(L"addHint", kTagHint, &OnHintPre, nullptr);
    if (LocalActs()) {
        ActStep(*session);
        TickWatch(Who());
        return;
    }
    // The watching peer arms at its own join's end, or at once on a host (its world is up), and runs
    // the hand's unstick once, before the acting peer pries (the join arm has nothing left to refuse),
    // and only once the hint count is live: a name watch is inert until its name resolves.
    if (!g_watchArmed) {
        if (!coop::roster::LocalIsHost() &&
            (!coop::net_pump::HasAnnouncedWorldReady() ||
             coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle))
            return;
        if (ArmOf() != Arm::Join && !sg::NameWatchLive(L"addHint", kTagHint)) return;
        ArmWatch(Who());
        if (ArmOf() != Arm::Join) RunHandUnstick(Who());
    }
    TickWatch(Who());
}

void OnDisconnect() {
    g_watched.clear();
    g_watchArmed = false;
    g_watchTick = 0;
    g_step = Step::WaitJoin;
    g_stepTicks = 0;
    g_target.Reset();
    g_targetKey.clear();
}

}  // namespace coop::dev::pry_drill
