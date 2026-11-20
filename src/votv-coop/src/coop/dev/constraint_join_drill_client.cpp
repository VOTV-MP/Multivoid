// coop/dev/constraint_join_drill_client.cpp -- the constraint-join drill's client half: the host's set found by element id, the
// carry of box A (c2), each End applied to a member judged where it lands, and the rejoined copies (c2). See
// coop/dev/constraint_join_drill.h.

#include "constraint_join_drill_detail.h"

#include "coop/dev/drill_chat.h"
#include "coop/dev/drill_ground.h"
#include "coop/net/net_clock.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_drive_stream.h"
#include "coop/props/remote_prop.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace coop::dev::constraint_join_drill::detail::client {
namespace {

namespace DC = coop::dev::drill_chat;
namespace DG = coop::dev::drill_ground;
namespace E  = ue_wrap::engine;
namespace R  = ue_wrap::reflection;
using ue_wrap::FVector;

constexpr const char* kLog = "[CONSTRAINT-JOIN-DRILL] [C]";

char Letter(size_t i) { return static_cast<char>('A' + i); }

enum class Step : uint8_t { WaitPeer, WaitReady, Resolve, Act, WaitFinal, Settle, Done };

Step     g_step = Step::WaitPeer;
uint64_t g_stepMs = 0;
bool     g_armed = false;
SetInfo  g_set;
ue_wrap::CachedObjRef g_box[kMaxBoxes];
uint64_t g_readyMs = 0;                       // an End counts only from here (GetTickCount64, the clock RecentEnds stamps in)
CarryAct g_carry;
FVector  g_final[kMaxBoxes];
bool     g_haveFinal[kMaxBoxes] = {};
// What the Ends applied to each box said: the last one's pose and how many.
FVector  g_lastEnd[kMaxBoxes];
uint32_t g_ends[kMaxBoxes] = {};
struct SeenEnd {
    uint32_t eid;
    uint64_t atMs;
};
std::vector<SeenEnd> g_seen;
DG::Settle g_rest[kMaxBoxes];

void To(Step s) {
    g_step = s;
    g_stepMs = coop::net::NowMs();
}
bool Expired() { return coop::net::NowMs() - g_stepMs > kBoundMs; }
void InvalidWaiting(const char* what) { Invalid('C', "%s never came", what); }

// Each member's copy, by the element id the host told. All resolved when this is true.
bool ResolveBoxes() {
    bool all = true;
    for (size_t i = 0; i < g_set.n; ++i) {
        if (g_box[i].Get()) continue;
        if (void* a = coop::remote_prop::ResolveLiveActorByEid(g_set.eid[i])) g_box[i].Set(a);
        else all = false;
    }
    return all;
}

// The Ends this peer applied since the set was told, each judged where it landed: the copy of a member stands within
// kEndPoseCm of the End's pose. In c2 box A is the client's own hold and its let-go, not a member of the host's stream.
void PollEnds() {
    coop::prop_drive_stream::AppliedEnd ends[coop::prop_drive_stream::kRecentEnds];
    const size_t n = coop::prop_drive_stream::RecentEnds(ends, coop::prop_drive_stream::kRecentEnds);
    for (size_t k = n; k-- > 0;) {   // oldest first
        const auto& e = ends[k];
        if (e.atMs < g_readyMs) continue;
        for (size_t i = 0; i < g_set.n; ++i) {
            if (e.eid != g_set.eid[i]) continue;
            bool seen = false;
            for (const SeenEnd& s : g_seen) seen = seen || (s.eid == e.eid && s.atMs == e.atMs);
            if (seen) continue;
            g_seen.push_back({e.eid, e.atMs});
            g_lastEnd[i] = e.pose;
            ++g_ends[i];
            if (ArmOf() == Arm::C2 && i == 0) continue;
            FVector at{};
            void* b = g_box[i].Get();
            if (!b || !E::TryGetActorLocation(b, at)) { Invalid('C', "box %c's copy could not be read at its End", Letter(i)); return; }
            const float d = DG::Dist(at, e.pose);
            UE_LOGI("%s End #%u applied to box %c: its copy stands %.1f cm from the End pose", kLog,
                    static_cast<unsigned>(g_ends[i]), Letter(i), static_cast<double>(d));
            if (d > kEndPoseCm) {
                Fail('C', "client-end", "box %c's copy stands %.1f cm from the End pose it was just given (bound %.0f cm)",
                     Letter(i), static_cast<double>(d), static_cast<double>(kEndPoseCm));
                return;
            }
        }
    }
}

// The host's final lines, read from the chat history: true when every box's place is known.
bool ReadFinals() {
    std::vector<std::string> rests;
    DC::SeenRestsOf(kFinalPrefix, &rests);
    for (const std::string& r : rests) {
        size_t i = 0;
        uint32_t eid = 0;
        FVector at{};
        if (!ParseFinal(r, i, eid, at) || i >= g_set.n || eid != g_set.eid[i]) continue;
        g_final[i] = at;
        g_haveFinal[i] = true;
    }
    for (size_t i = 0; i < g_set.n; ++i)
        if (!g_haveFinal[i]) return false;
    return true;
}

void StepWaitReady() {
    std::string rest;
    if (!DC::SeenRestOf(kReadyPrefix, &rest)) {
        Wait('C', "the host's ready line");
        if (Expired()) InvalidWaiting("the host's ready line");
        return;
    }
    if (!ParseReady(rest, g_set)) { Invalid('C', "the host's ready line could not be read (%s)", rest.c_str()); return; }
    g_readyMs = ::GetTickCount64();
    UE_LOGI("%s the host's set: %zu boxes, box A stands at (%.0f,%.0f,%.0f)", kLog, g_set.n, static_cast<double>(g_set.a.X),
            static_cast<double>(g_set.a.Y), static_cast<double>(g_set.a.Z));
    To(Step::Resolve);
}

void StepResolve() {
    if (!ResolveBoxes()) {
        Wait('C', "this client's copy of every box");
        if (Expired()) InvalidWaiting("this client's copy of every box");
        return;
    }
    for (size_t i = 0; i < g_set.n; ++i) DG::Restart(g_rest[i]);
    UE_LOGI("%s found its copy of every box by element id", kLog);
    if (ArmOf() == Arm::C2 && !SecondLife()) {
        g_carry.Begin(g_box[0].Get(), FVector{-1.f, 0.f, 0.f}, 'C');
        To(Step::Act);
    } else {
        To(Step::WaitFinal);
    }
}

void StepAct() {
    PollEnds();
    if (Over()) return;
    if (g_carry.Tick() == CarryAct::Status::Done) To(Step::WaitFinal);
}

// c1 and c3: PASS once every box's last End is the host's final place. c2's first life: the carry is done and the host has
// judged; it ends here for the rejoin. c2's second life: the copies the snapshot made are judged at their rest.
void StepWaitFinal() {
    PollEnds();
    if (Over()) return;
    if (!ReadFinals()) {
        Wait('C', "the host's final places");
        if (Expired()) InvalidWaiting("the host's final places");
        return;
    }
    if (ArmOf() == Arm::C2) {
        if (SecondLife()) {
            for (size_t i = 0; i < g_set.n; ++i) DG::Restart(g_rest[i]);
            To(Step::Settle);
            return;
        }
        UE_LOGI("constraint-join: client life 1 done");
        To(Step::Done);
        return;
    }
    size_t worst = 0;
    float worstCm = 0.f;
    bool all = true;
    for (size_t i = 0; i < g_set.n; ++i) {
        const float d = g_ends[i] ? DG::Dist(g_lastEnd[i], g_final[i]) : 1e9f;
        if (d > kEndPoseCm) all = false;
        if (d >= worstCm) { worstCm = d; worst = i; }
    }
    if (!all) {
        char w[160];
        std::snprintf(w, sizeof(w), "every box's last End to be the host's final place (box %c: %u End(s), %.1f cm off)",
                      Letter(worst), static_cast<unsigned>(g_ends[worst]), static_cast<double>(worstCm));
        Wait('C', "%s", w);
        if (Expired()) InvalidWaiting(w);
        return;
    }
    UE_LOGI("%s DONE: every box's last End is the host's final place", kLog);
    Pass("every box's last End applied is the host's final place, within %.0f cm (%zu boxes)", static_cast<double>(kEndPoseCm),
         g_set.n);
    To(Step::Done);
}

void StepSettle() {
    bool all = true;
    for (size_t i = 0; i < g_set.n; ++i) {
        FVector at{};
        void* b = g_box[i].Get();
        if (!b || !E::TryGetActorLocation(b, at)) { Invalid('C', "box %c's copy died before its rest", Letter(i)); return; }
        all = DG::Stable(g_rest[i], at, DG::Quiet(b), kQuietTicks) && all;
    }
    if (!all) {
        Wait('C', "the rejoined copies to rest");
        if (Expired()) InvalidWaiting("the rejoined copies' rest");
        return;
    }
    for (size_t i = 0; i < g_set.n; ++i) {
        FVector at{};
        E::TryGetActorLocation(g_box[i].Get(), at);
        const float d = DG::Dist(at, g_final[i]);
        UE_LOGI("%s rejoined: box %c stands %.1f cm from the host's final place", kLog, Letter(i), static_cast<double>(d));
        if (d > kEndPoseCm) {
            Fail('C', "rejoin", "box %c's copy in the rejoined world stands %.1f cm from the host's final place (bound %.0f cm)",
                 Letter(i), static_cast<double>(d), static_cast<double>(kEndPoseCm));
            return;
        }
    }
    UE_LOGI("%s DONE: the rejoined copies stand where the host's rest", kLog);
    Pass("the rejoined copies of all %zu boxes stand within %.0f cm of the host's final places", g_set.n,
         static_cast<double>(kEndPoseCm));
    To(Step::Done);
}

}  // namespace

void Tick(coop::net::Session& /*s*/) {
    void* player = coop::players::Registry::Get().Local();
    if (!coop::net_pump::HasAnnouncedWorldReady() ||
        coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle || !player || !R::IsLive(player)) {
        Wait('C', "this client's join to end and its pawn");
        return;
    }
    if (!g_armed) {
        // The first Seen* read starts the chat history: lines reach a peer while its join window is still open, and the feed
        // retires a row after about ten seconds.
        g_armed = true;
        DC::SeenHas(kReadyPrefix);
        To(Step::WaitReady);
        UE_LOGI("%s started (arm c%d, life %d): reading the chat history", kLog, static_cast<int>(ArmOf()),
                SecondLife() ? 2 : 1);
        return;
    }
    switch (g_step) {
    case Step::WaitPeer:  To(Step::WaitReady); return;
    case Step::WaitReady: StepWaitReady(); return;
    case Step::Resolve:   StepResolve(); return;
    case Step::Act:       StepAct(); return;
    case Step::WaitFinal: StepWaitFinal(); return;
    case Step::Settle:    StepSettle(); return;
    case Step::Done:      return;
    }
}

void Reset() {
    g_step = Step::WaitPeer;
    g_stepMs = 0;
    g_armed = false;
    g_set = SetInfo{};
    g_readyMs = 0;
    g_carry.Reset();
    g_seen.clear();
    for (size_t i = 0; i < kMaxBoxes; ++i) {
        g_box[i].Reset();
        g_final[i] = {};
        g_haveFinal[i] = false;
        g_lastEnd[i] = {};
        g_ends[i] = 0;
        DG::Restart(g_rest[i]);
    }
}

}  // namespace coop::dev::constraint_join_drill::detail::client
