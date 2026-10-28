// coop/dev/cord_pos_drill_client.cpp -- see coop/dev/cord_pos_drill.h: the client's half. Each coffee maker that
// binds as a fresh mirror is read on every tick from its bind: while it is hidden its root collision and its lowest z,
// at the reveal the moment, and then its place against the host's rest. The host's word comes by the drill chat.

#include "cord_pos_drill_detail.h"  // co-located private header (src tree, not include/)

#include "coop/dev/drill_chat.h"
#include "coop/element/mirror_defer.h"
#include "coop/net/session.h"
#include "coop/props/prop_drive_stream.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace coop::dev::cord_pos_drill::detail {
namespace {

namespace DC = coop::dev::drill_chat;
namespace E = ue_wrap::engine;
namespace R = ue_wrap::reflection;

enum class St : uint8_t { Hidden, Reveal2s, AtRest, Finished };
enum class Verdict : uint8_t { Pass, Fail, Inconclusive };

constexpr int kMaxBindComps = 16;   // as mirror_defer's bound
struct BindComp {
    ue_wrap::CachedObjRef ref;
    int coll;
};

struct Tracked {
    ue_wrap::CachedObjRef actor;
    uint32_t eid = 0;
    std::string key;
    uint64_t bindMs = 0;
    St st = St::Hidden;

    // the hidden window
    bool prevHidden = false, zeroColl = false;
    int hiddenReads = 0, collMin = 4, collMax = -1;
    // every primitive the maker owns and its collision at the bind, before the hide: what the reveal must put back
    std::vector<BindComp> bindComps;
    bool hideLogged = false;
    float lowestZ = 0.f;
    // the reveal and the drive end, in the client's ticks
    bool revealed = false, wasParked = false, endSeen = false;
    uint64_t revealMs = 0, revealTick = 0, endTick = 0;
    // the host's word
    bool hostKnown = false;
    float hostZ = 0.f, hostX = 0.f, hostY = 0.f;
    RestClock clock;
};
std::vector<Tracked> g_tracked;
uint64_t g_tick = 0, g_firstBindMs = 0;
int g_pass = 0, g_fail = 0, g_inconclusive = 0;
bool g_done = false;

// AActor::bHidden (SetActorHiddenInGame's flag): 1, 0, or -1 unread.
int HiddenOf(void* actor) {
    static int32_t s_off = -2;
    static uint8_t s_mask = 0;
    if (s_off == -2) {
        int32_t off = -1;
        uint8_t mask = 0;
        if (!R::FindBoolProperty(R::ClassOf(actor), L"bHidden", off, mask)) off = -1;
        s_off = off;
        s_mask = mask;
    }
    if (s_off < 0) return -1;
    return (static_cast<const uint8_t*>(actor)[s_off] & s_mask) ? 1 : 0;
}

float Dist3(const ue_wrap::FVector& a, float x, float y, float z) {
    const float dx = a.X - x, dy = a.Y - y, dz = a.Z - z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

const char* OrderText(const Tracked& t) {
    if (!t.revealed) return "no reveal seen";
    if (!t.endSeen) return "no drive end seen (a pended end lands at the bind, unseen here)";
    if (t.endTick < t.revealTick) return "drive end before the reveal";
    if (t.endTick == t.revealTick) return "drive end on the reveal's tick";
    return "drive end after the reveal";
}

void Finish(Tracked& t, Verdict v, const char* line) {
    if (v == Verdict::Pass) { ++g_pass; UE_LOGI("[CORDPOS-DRILL] %s", line); }
    else if (v == Verdict::Fail) { ++g_fail; UE_LOGW("[CORDPOS-DRILL] FAIL: %s", line); }
    else { ++g_inconclusive; UE_LOGW("[CORDPOS-DRILL] INCONCLUSIVE: %s", line); }
    UE_LOGI("[CORDPOS-DRILL] %s order: %s", t.key.c_str(), OrderText(t));
    t.st = St::Finished;
}

// The hidden window's verdict, once the host's rest is known and the maker is shown. False when it ended the maker.
bool JudgeHidden(Tracked& t) {
    char line[200];
    if (t.zeroColl) {
        std::snprintf(line, sizeof(line), "%s collision 0 while hidden", t.key.c_str());
        Finish(t, Verdict::Fail, line);
        return false;
    }
    if (t.hiddenReads == 0) {
        std::snprintf(line, sizeof(line), "%s no hidden read", t.key.c_str());
        Finish(t, Verdict::Inconclusive, line);
        return false;
    }
    if (t.lowestZ < t.hostZ - kFellCm) {
        std::snprintf(line, sizeof(line), "%s fell %.1f below the host's rest", t.key.c_str(), t.hostZ - t.lowestZ);
        Finish(t, Verdict::Fail, line);
        return false;
    }
    UE_LOGI("[CORDPOS-DRILL] %s hidden window ok: %d reads, lowest z %.1f, host rest z %.1f, collision %d..%d",
            t.key.c_str(), t.hiddenReads, t.lowestZ, t.hostZ, t.collMin, t.collMax);
    return true;
}

// A parity read: the maker's place within kParityCm of the host's rest. False when it ended the maker.
bool JudgeParity(Tracked& t, void* actor, const char* when) {
    ue_wrap::FVector at{};
    char line[200];
    // The reveal must have put every primitive's collision back to what the bind found.
    if (t.bindComps.empty()) {
        std::snprintf(line, sizeof(line), "%s %s: no primitive's collision read at the bind", t.key.c_str(), when);
        Finish(t, Verdict::Inconclusive, line);
        return false;
    }
    for (size_t i = 0; i < t.bindComps.size(); ++i) {
        const BindComp& b = t.bindComps[i];
        const int coll = E::GetComponentCollisionEnabled(b.ref.Get());
        if (coll < 0 || b.coll < 0) {
            std::snprintf(line, sizeof(line), "%s %s: primitive %zu did not read (bind %d)", t.key.c_str(), when, i,
                          b.coll);
            Finish(t, Verdict::Inconclusive, line);
            return false;
        }
        if (coll != b.coll) {
            std::snprintf(line, sizeof(line), "%s collision not restored (%d vs %d) on primitive %zu", t.key.c_str(),
                          coll, b.coll, i);
            Finish(t, Verdict::Fail, line);
            return false;
        }
    }
    if (!E::TryGetActorLocation(actor, at)) {
        std::snprintf(line, sizeof(line), "%s %s: the place did not read", t.key.c_str(), when);
        Finish(t, Verdict::Inconclusive, line);
        return false;
    }
    const float d = Dist3(at, t.hostX, t.hostY, t.hostZ);
    UE_LOGI("[CORDPOS-DRILL] %s %s: client (%.1f,%.1f,%.1f) host (%.1f,%.1f,%.1f) off by %.1f", t.key.c_str(), when, at.X,
            at.Y, at.Z, t.hostX, t.hostY, t.hostZ, d);
    if (d <= kParityCm) return true;
    std::snprintf(line, sizeof(line), "%s %s: %.1f cm from the host's rest", t.key.c_str(), when, d);
    Finish(t, Verdict::Fail, line);
    return false;
}

void ReadHost(Tracked& t) {
    std::string rest;
    if (!DC::LiveRestOf(RestPrefix(t.key), &rest)) return;
    // "<z> <x> <y>", the host's line
    char* p = nullptr;
    const char* s = rest.c_str();
    const float z = std::strtof(s, &p);
    if (p == s) return;
    s = p;
    const float x = std::strtof(s, &p);
    if (p == s) return;
    s = p;
    const float y = std::strtof(s, &p);
    if (p == s) return;
    t.hostZ = z;
    t.hostX = x;
    t.hostY = y;
    t.hostKnown = true;
}

// One read of a hidden maker: the hide it got, its root collision and its height. Every tick while it is hidden,
// and once at the hide itself (OnHidden), so a mirror the lift reveals at once still has its read.
void ReadHidden(Tracked& t, void* actor, bool placed, const ue_wrap::FVector& at) {
    if (!t.hideLogged) {
        coop::mirror_defer::HideInfo hi{};
        if (coop::mirror_defer::HideInfoOf(t.eid, hi)) {
            t.hideLogged = true;
            if (hi.fallbackWhy) UE_LOGI("[CORDPOS-DRILL] %s fell back to Off: %s", t.key.c_str(), hi.fallbackWhy);
            else if (hi.applied == coop::mirror_defer::HideCollision::PhysicsOnly)
                UE_LOGI("[CORDPOS-DRILL] %s hid %d primitives", t.key.c_str(), hi.primitives);
            else UE_LOGI("[CORDPOS-DRILL] %s hidden with mode %d", t.key.c_str(), static_cast<int>(hi.applied));
        }
    }
    const int coll = E::GetActorRootCollisionEnabled(actor);
    if (placed && coll >= 0) {
        if (t.hiddenReads == 0 || at.Z < t.lowestZ) t.lowestZ = at.Z;
        ++t.hiddenReads;
        if (coll < t.collMin) t.collMin = coll;
        if (coll > t.collMax) t.collMax = coll;
        if (coll == 0 && !t.zeroColl) {
            t.zeroColl = true;
            UE_LOGI("[CORDPOS-DRILL] %s hidden tick with root collision 0 at z %.1f", t.key.c_str(), at.Z);
        }
    }
}

void TickOne(Tracked& t, uint64_t now, bool poll) {
    void* actor = t.actor.Get();
    if (!actor) {
        char line[120];
        std::snprintf(line, sizeof(line), "%s gone before it was judged", t.key.c_str());
        return Finish(t, Verdict::Fail, line);
    }
    if (now - t.bindMs > kFailMs) {
        char line[160];
        std::snprintf(line, sizeof(line), "%s timed out at step %d (host word %s)", t.key.c_str(), static_cast<int>(t.st),
                      t.hostKnown ? "heard" : "never heard");
        return Finish(t, t.hostKnown ? Verdict::Fail : Verdict::Inconclusive, line);
    }
    // the reads, every tick
    const int hidden = HiddenOf(actor);
    ue_wrap::FVector at{};
    const bool placed = E::TryGetActorLocation(actor, at);
    if (hidden == 1) {
        ReadHidden(t, actor, placed, at);
    } else if (hidden == 0 && t.prevHidden && !t.revealed) {
        t.revealed = true;
        t.revealMs = now;
        t.revealTick = g_tick;
        UE_LOGI("[CORDPOS-DRILL] %s revealed at z %.1f after %d hidden reads", t.key.c_str(), placed ? at.Z : 0.f,
                t.hiddenReads);
    }
    t.prevHidden = hidden == 1;
    const bool parked = coop::prop_drive_stream::IsParked(actor);
    if (parked) {
        t.wasParked = true;
    } else if (t.wasParked && !t.endSeen) {
        t.endSeen = true;
        t.endTick = g_tick;
    }
    if (!t.hostKnown && poll) ReadHost(t);

    if (t.st == St::Hidden) {
        if (!t.hostKnown || hidden != 0) return;
        if (!JudgeHidden(t)) return;
        t.st = St::Reveal2s;
    }
    if (t.st == St::Reveal2s) {
        if (now - t.revealMs < kRevealReadMs) return;
        if (!JudgeParity(t, actor, "at reveal+2s")) return;
        t.st = St::AtRest;
        return;
    }
    if (t.st == St::AtRest && t.clock.Update(actor, now, kClientQuietMs)) {
        if (!JudgeParity(t, actor, "after 10s of rest")) return;
        char line[120];
        std::snprintf(line, sizeof(line), "%s PASS", t.key.c_str());
        Finish(t, Verdict::Pass, line);
    }
}

void Conclude(uint64_t now) {
    if (g_done || g_tracked.empty()) return;
    int open = 0;
    for (const Tracked& t : g_tracked) open += t.st != St::Finished;
    if (open) return;
    if (static_cast<int>(g_tracked.size()) < kMakers) {
        if (now - g_firstBindMs <= kFailMs) return;
        const int missing = kMakers - static_cast<int>(g_tracked.size());
        UE_LOGW("[CORDPOS-DRILL] INCONCLUSIVE: no bind for %d of %d makers", missing, kMakers);
        ++g_inconclusive;
    }
    g_done = true;
    const char* verdict = g_fail ? "FAIL" : g_inconclusive ? "INCONCLUSIVE" : "PASS";
    UE_LOGI("[CORDPOS-DRILL] client DONE -- %s (pass=%d fail=%d inconclusive=%d)", verdict, g_pass, g_fail,
            g_inconclusive);
}

}  // namespace

void ClientOnBound(uint32_t eid, void* actor) {
    if (!actor || g_done || static_cast<int>(g_tracked.size()) >= kMakers) return;
    if (R::ClassNameOf(actor) != L"prop_coffeeMaker_C") return;
    for (const Tracked& t : g_tracked)
        if (t.actor.Raw() == actor) return;
    Tracked t;
    t.actor.Set(actor);
    t.eid = eid;
    t.key = KeyOf(actor);
    void* comps[kMaxBindComps];
    const int n = E::OwnPrimitiveComponents(actor, comps, kMaxBindComps);
    for (int i = 0; i < n; ++i) {
        BindComp b;
        b.ref.Set(comps[i]);
        b.coll = E::GetComponentCollisionEnabled(comps[i]);
        t.bindComps.push_back(b);
    }
    t.bindMs = NowMs();
    if (g_tracked.empty()) g_firstBindMs = t.bindMs;
    UE_LOGI("[CORDPOS-DRILL] client bound eid=%u key=%s", eid, t.key.c_str());
    g_tracked.push_back(t);
}

void ClientOnHidden(uint32_t eid, void* actor) {
    if (!actor || g_done) return;
    for (Tracked& t : g_tracked) {
        if (t.eid != eid || t.actor.Raw() != actor || HiddenOf(actor) != 1) continue;
        ue_wrap::FVector at{};
        const bool placed = E::TryGetActorLocation(actor, at);
        ReadHidden(t, actor, placed, at);
        t.prevHidden = true;
    }
}

void ClientTick(coop::net::Session*) {
    if (g_done) return;
    const bool poll = DC::PollDue();
    if (poll && DC::ChatHasLive(kInvalidLine)) {
        g_done = true;
        UE_LOGW("[CORDPOS-DRILL] INVALID (client): the host could not stage the drill");
        UE_LOGI("[CORDPOS-DRILL] client DONE -- INVALID");
        return;
    }
    if (g_tracked.empty()) return;
    ++g_tick;
    const uint64_t now = NowMs();
    for (Tracked& t : g_tracked)
        if (t.st != St::Finished) TickOne(t, now, poll);
    Conclude(now);
}

void ClientReset() {
    g_tracked.clear();
    g_tick = g_firstBindMs = 0;
    g_pass = g_fail = g_inconclusive = 0;
    g_done = false;
}

}  // namespace coop::dev::cord_pos_drill::detail
