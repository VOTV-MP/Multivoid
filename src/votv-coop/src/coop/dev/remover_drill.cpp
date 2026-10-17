// coop/dev/remover_drill.cpp -- see coop/dev/remover_drill.h.

#include "coop/dev/remover_drill.h"
#include "remover_drill_detail.h"

#include "coop/comms/chat_feed.h"
#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/hook_plant.h"
#include "coop/dev/remover_drill_layout.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/element/registry.h"
#include "coop/player/players_registry.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"  // HasAnnouncedWorldReady

#include "ue_wrap/actors/hook.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/trace.h"
#include "ue_wrap/desk/dish_writers.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"
#include "ue_wrap/engine/hit_result.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace coop::dev::remover_drill {
using namespace detail;
namespace {

namespace E  = ue_wrap::engine;
namespace R  = ue_wrap::reflection;
namespace H  = ue_wrap::hook;
namespace DW = ue_wrap::dish_writers;
namespace EL = coop::element;
using ue_wrap::FVector;
namespace layout = coop::dev::remover_drill::layout;
using layout::Dist;
using layout::kRing;
using layout::kSingleClass;
using layout::kMashClass;
using layout::kSackClass;
using layout::kNpcClass;
using layout::kHookedClass;

// Three plain props of three classes (none trash, kerfur or keyed-special), one per role: the single prop a medium sack,
// the mashed one a small sack, the ring plain sacks. Sacks settle at a spawn at floor height; a dumbbell and a beer
// bottle spawned the same way fall through the floor on the host and are destroyed far below it.
constexpr const wchar_t* kToolPath    = L"/Game/objects/tool_remover.tool_remover_C";
constexpr const wchar_t* kToolClass   = L"tool_remover_C";
constexpr const wchar_t* kToolgunPath = L"/Game/objects/prop_toolgun.prop_toolgun_C";

// The notices the remover shows, by their text: the screen's own words, the drill's reading of them.
constexpr const char* kNoRight     = "The host does not allow the remover";
constexpr const char* kOutOfSight  = "You can't see that from there";
constexpr const char* kUnsupported = "The remover cannot remove that in multiplayer";
constexpr const char* kHeldNotice  = "That is being held";

// The host's layout about its client's puppet, in centimetres along its facing: the single and the mashed props side by
// side ahead of it, the creature straight ahead beyond them, a ring of kRing about it clear of the cone it faces. The
// client tells the roles by class (kSingleClass, kMashClass, the sacks), never by where physics left them.
constexpr int   kMashShots    = 6;
constexpr float kFindRadiusCm = 700.f;
constexpr float kAimPastCm    = 50.f;   // the press aims this far past the target's middle
constexpr float kRestMoveCm   = 1.f;    // a prop that moved less than this between two reads is at rest
constexpr int   kRestChecks   = 4;      // ...that many reads in a row, a check apart

constexpr uint64_t kFindBoundMs  = 60000;   // CLIENT: the join's end to every prop the host spawned being here
constexpr uint64_t kLegBoundMs   = 30000;   // CLIENT: a shot to what it must show (a failure bound, never a wait)

// ---- client -----------------------------------------------------------------------------------------------------

enum class Step : uint8_t {
    Arm, Find, Settle, Relay, Cleared, Single, SingleWait, Mash, MashWait, Sweep, SweepWait, Npc, NpcWait,
    HostHooked, HostHookedWait, SelfHooked, SelfHookedWait, Finish
};
Step g_step = Step::Arm;
ue_wrap::CachedObjRef g_toolgun, g_single, g_mash, g_npc;
ue_wrap::CachedObjRef g_hostHooked, g_selfHooked;   // the crowbars: the host's hook pulls the first, this client's the second
ue_wrap::CachedObjRef g_selfHook;                   // the hook this client planted on the second
ue_wrap::CachedObjRef g_ring[kRing];    // every ring prop the host laid
int g_ringN = 0;
ue_wrap::CachedObjRef g_sweep[kRing];   // the ring props the camera's trace meets first: the shots of the sweep
int g_sweepN = 0;
int g_sweepAt = 0;
int g_noticeBase = 0;   // the leg's notice count before its shot
char g_aimWhy[420] = "";   // why the last AimedHit or press failed
FVector g_restAt[kRing + 2];   // where the pair and the ring stood at the last rest read
int g_restReads = 0;           // reads in a row at which none of them had moved
int g_relay = 0;                 // relays asked of the host for this run
std::string g_relayWhy;          // what each direction's blocked trace met, for the abandon that ends the fan
FVector g_aimCam{}, g_aimBlkAt{};   // the last miss: the camera, and where the actor its trace met stands
uint32_t g_aimBlkEid = 0;

void Next(Step s) { g_step = s; g_stepMs = ::GetTickCount64(); }
void Fail(const char* what) { End("client", "FAIL", what, true); }
void Abandon(const char* why) { End("client", "ABANDONED", why, true); }

// An abandon that carries the aim's own reason after `what`.
void AbandonAim(const char* what) {
    char why[560];
    std::snprintf(why, sizeof(why), "%s: %s", what, g_aimWhy);
    Abandon(why);
}

void LegPass(const char* name, Step next) {
    UE_LOGI("[REMOVER-DRILL] client leg %s PASS", name);
    Next(next);
}

// How many lines of this client's feed carry `text`.
int NoticeCount(const char* text) {
    int n = 0;
    coop::chat_feed::ForEachRow([&](const coop::chat_feed::RowView& r) {
        if (r.line.find(text) != std::string_view::npos) ++n;
    });
    return n;
}

void DestroyIfLive(void* actor) {
    if (actor && R::IsLive(actor)) E::DestroyActor(actor);
}

// The hit the game itself would write for a press at `target`: the first thing a trace from this player's camera toward the
// target's middle (and a little past it, so the ray crosses its body) meets, over the four object types the toolgun's
// shot takes. Every player body is ignored, as the host's judge ignores them: with both players at one spawn point a native
// trace would stop at the other's capsule. True when that first thing is the target and the hit names a component.
bool AimedHit(void* player, void* target, ue_wrap::trace::Hit* hit) {
    g_aimCam = {};
    g_aimBlkAt = {};
    g_aimBlkEid = 0;
    FVector cam{}, at{};
    if (!E::ReadMainPlayerCameraLocation(player, cam) || !E::TryGetActorLocation(target, at)) {
        std::snprintf(g_aimWhy, sizeof(g_aimWhy), "the camera or the target's location did not read");
        return false;
    }
    const float len = Dist(cam, at);
    if (!(len > 1.f)) {
        std::snprintf(g_aimWhy, sizeof(g_aimWhy), "the target is at the camera");
        return false;
    }
    const float past = (len + kAimPastCm) / len;
    const FVector end{cam.X + (at.X - cam.X) * past, cam.Y + (at.Y - cam.Y) * past, cam.Z + (at.Z - cam.Z) * past};
    const uint8_t types[4] = {0, 1, 2, 3};
    void* ignore[coop::players::kMaxPeers];
    const int nIgnore = coop::players::Registry::Get().LiveBodies(ignore, coop::players::kMaxPeers);
    if (!ue_wrap::trace::LineHitForObjects(player, cam, end, types, 4, ignore, nIgnore, hit)) {
        std::snprintf(g_aimWhy, sizeof(g_aimWhy), "the trace did not run");
        return false;
    }
    if (hit->blocked && hit->actor == target && hit->component) return true;
    FVector bat{};   // where the actor the trace met stands, to set against where the other peer holds it
    if (hit->actor) E::TryGetActorLocation(hit->actor, bat);
    g_aimCam = cam;
    g_aimBlkAt = bat;
    g_aimBlkEid = hit->actor ? static_cast<uint32_t>(coop::element::Registry::Get().EidForActor(hit->actor)) : 0u;
    if (!hit->blocked)
        std::snprintf(g_aimWhy, sizeof(g_aimWhy), "the trace met nothing, %.0f cm to the target", len);
    else if (hit->actor == target)
        std::snprintf(g_aimWhy, sizeof(g_aimWhy), "the hit on the target names no component");
    else
        std::snprintf(g_aimWhy, sizeof(g_aimWhy),
                      "the trace met %ls (element %u, at %.0f, %.0f, %.0f) at %.0f cm of the %.0f cm to the target's middle "
                      "(camera %.0f, %.0f, %.0f; target %.0f, %.0f, %.0f)",
                      hit->actor ? R::ClassNameOf(hit->actor).c_str() : L"(no actor)",
                      static_cast<unsigned>(coop::element::Registry::Get().EidForActor(hit->actor)), bat.X, bat.Y,
                      bat.Z, Dist(cam, hit->point), len, cam.X, cam.Y, cam.Z, at.X, at.Y, at.Z);
    return false;
}

// One press of the remover on `target` with the hit the toolgun is to hold: a fresh tool, outHit written, and the tool's
// init(toolgun) -- the body the toolgun's press runs. The tool is gone after it on every path (the game's body destroys
// it, the lane's watch destroys it); one left standing is destroyed here.
bool PressWith(void* player, void* target, const ue_wrap::trace::Hit& hit) {
    void* toolgun = g_toolgun.Get();
    void* cls = ue_wrap::object_index::ClassByName(kToolClass);
    FVector me{};
    if (!toolgun || !cls || !E::TryGetActorLocation(player, me)) {
        std::snprintf(g_aimWhy, sizeof(g_aimWhy), "the toolgun, the tool class or the player's location is missing");
        return false;
    }
    void* tool = DW::SpawnTool(cls, {me.X, me.Y, me.Z + 90.f});
    if (!tool) std::snprintf(g_aimWhy, sizeof(g_aimWhy), "the tool did not spawn");
    const bool ran = tool && ue_wrap::hit_result::WriteField(toolgun, L"outHit", target, hit.component, hit.point) &&
                     DW::CallToolInit(tool, toolgun);
    if (tool && !ran) std::snprintf(g_aimWhy, sizeof(g_aimWhy), "outHit could not be written or init did not run");
    DestroyIfLive(tool);
    return ran;
}

// One press at `target` aimed afresh: the real hit, then the press. False with g_aimWhy saying which part failed.
bool Press(void* player, void* target) {
    ue_wrap::trace::Hit hit;
    return AimedHit(player, target, &hit) && PressWith(player, target, hit);
}

struct Seen { void* actor; float cm; };

// The host's props of one class about `from`, nearest first.
std::vector<Seen> PropsAbout(const wchar_t* className, const FVector& from) {
    std::vector<Seen> out;
    void* cls = ue_wrap::object_index::ClassByName(className);
    if (!cls) return out;
    struct Ctx { const FVector* from; std::vector<Seen>* out; } ctx{&from, &out};
    ue_wrap::object_index::ForEachInstance(cls, [](void* c, void* obj, int32_t) {
        auto* x = static_cast<Ctx*>(c);
        FVector at{};
        if (R::NameStartsWith(R::NameOf(obj), L"Default__") || !E::TryGetActorLocation(obj, at)) return;
        const float cm = Dist(at, *x->from);
        if (cm <= kFindRadiusCm) x->out->push_back({obj, cm});
    }, &ctx);
    std::sort(out.begin(), out.end(), [](const Seen& a, const Seen& b) { return a.cm < b.cm; });
    return out;
}

void* NearestNpc(const FVector& from) {
    void* cls = ue_wrap::object_index::ClassByName(kNpcClass);
    if (!cls) return nullptr;
    struct Ctx { const FVector* from; void* best; float cm; } ctx{&from, nullptr, kFindRadiusCm};
    ue_wrap::object_index::ForEachInstance(cls, [](void* c, void* obj, int32_t) {
        auto* x = static_cast<Ctx*>(c);
        FVector at{};
        if (R::NameStartsWith(R::NameOf(obj), L"Default__") || !E::TryGetActorLocation(obj, at)) return;
        const float cm = Dist(at, *x->from);
        if (cm < x->cm) { x->best = obj; x->cm = cm; }
    }, &ctx);
    return ctx.best;
}

// The host's roles, each a class of its own (the single prop a medium sack, the mashed one a small sack, the ring plain
// sacks, the creature a kerfurOmega, the two crowbars the nearer and the farther of their class), so a role is found by
// what it is and not by where physics left it. Fills the client's roles; false while the host's props are not all here.
// Whether the camera can press each is Choose's, once they rest.
bool Find(void* player) {
    FVector me{};
    if (!E::TryGetActorLocation(player, me)) return false;
    const bool granted = PhaseOf() == Phase::Granted;
    const std::vector<Seen> single = PropsAbout(kSingleClass, me);
    if (single.empty()) return false;
    if (!granted) {
        g_single.Set(single[0].actor);
        return true;
    }
    const std::vector<Seen> mash = PropsAbout(kMashClass, me);
    const std::vector<Seen> ring = PropsAbout(kSackClass, me);
    const std::vector<Seen> hooked = PropsAbout(kHookedClass, me);
    void* npc = NearestNpc(me);
    if (mash.empty() || ring.size() < static_cast<size_t>(kRing) || hooked.size() < 2 || !npc) return false;
    g_single.Set(single[0].actor);
    g_mash.Set(mash[0].actor);
    g_hostHooked.Set(hooked[0].actor);
    g_selfHooked.Set(hooked[1].actor);
    g_npc.Set(npc);
    g_ringN = kRing;
    for (int i = 0; i < kRing; ++i) g_ring[i].Set(ring[static_cast<size_t>(i)].actor);
    return true;
}

// Have the pair and the ring stopped moving: no one of them has moved more than a centimetre since the last read, as
// sack_drill waits for its sack. A press at a prop still falling or coasting is not a press at the layout the host laid.
bool AtRest() {
    bool still = true;
    int n = 0;
    const auto visit = [&](const ue_wrap::CachedObjRef& ref) {
        FVector at{};
        void* a = ref.Get();
        if (!a || !E::TryGetActorLocation(a, at)) { still = false; ++n; return; }
        if (Dist(at, g_restAt[n]) >= kRestMoveCm) still = false;
        g_restAt[n++] = at;
    };
    visit(g_single);
    if (PhaseOf() == Phase::Granted) {
        visit(g_mash);
        for (int i = 0; i < g_ringN; ++i) visit(g_ring[i]);
    }
    return still;
}

// Says `text` to the host as a chat line, through the session's own reliable lane (the host reads it from its lobby record).
bool SayToHost(coop::net::Session* s, const std::string& text) {
    coop::net::ChatMessagePayload p{};
    if (text.empty() || text.size() > sizeof(p.text)) return false;
    std::memcpy(p.text, text.data(), text.size());
    p.len = static_cast<uint8_t>(text.size());
    return s->SendReliable(coop::net::ReliableKind::ChatMessage, &p, sizeof(p));
}

// A target the camera's own trace does not meet first: the host's trace judged the segment clear from its eye, and the
// client is the judge of its own shot. It asks the host, in a chat line that carries its camera and what its trace met, to
// lay the layout again in the next fan direction, up to the fan's size; then it abandons with every direction's reason.
// False: the run goes on only after the relay, or it has ended.
bool Blocked(coop::net::Session* s, const char* which) {
    char one[460];
    std::snprintf(one, sizeof(one), "%s%s: %s", g_relayWhy.empty() ? "" : "; ", which, g_aimWhy);
    if (g_relayWhy.size() < 1400) g_relayWhy += one;
    if (g_relay >= layout::kFanSteps - 1) {
        Abandon(("every direction the host laid was blocked from this camera -- " + g_relayWhy).c_str());
        return false;
    }
    ++g_relay;
    char line[200];
    std::snprintf(line, sizeof(line), "remover-drill relay %d cam %.0f %.0f %.0f eid %u at %.0f %.0f %.0f", g_relay, g_aimCam.X,
                  g_aimCam.Y, g_aimCam.Z, static_cast<unsigned>(g_aimBlkEid), g_aimBlkAt.X, g_aimBlkAt.Y, g_aimBlkAt.Z);
    if (!SayToHost(s, line)) { Abandon("the relay line was not sent"); return false; }
    UE_LOGI("[REMOVER-DRILL] client: %s: %s -- asking the host to lay again (relay %d)", which, g_aimWhy, g_relay);
    Next(Step::Relay);
    return false;
}

// Once the layout is at rest: the camera's own trace must meet each target first (a press must be one the game itself could
// make), and the sweep is every ring prop it meets first. True when the run goes on.
bool Choose(void* player, coop::net::Session* s) {
    ue_wrap::trace::Hit probe;
    if (!AimedHit(player, g_single.Get(), &probe)) return Blocked(s, "the single prop");
    if (PhaseOf() != Phase::Granted) return true;
    if (!AimedHit(player, g_mash.Get(), &probe)) return Blocked(s, "the mashed prop");
    if (!AimedHit(player, g_npc.Get(), &probe)) return Blocked(s, "the creature");
    int n = 0;
    for (int i = 0; i < g_ringN; ++i) {
        void* r = g_ring[i].Get();
        if (r && AimedHit(player, r, &probe)) g_sweep[n++].Set(r);
    }
    g_sweepN = n;
    if (n < kSweepFloor) {
        char why[120];
        std::snprintf(why, sizeof(why), "%d of the ring props are in this client's sight, %d are needed", n, kSweepFloor);
        Abandon(why);
        return false;
    }
    return true;
}

bool SpawnToolgun(void* player) {
    FVector me{};
    ue_wrap::asset_load::LoadObjectByPath(kToolgunPath);
    ue_wrap::asset_load::LoadObjectByPath(kToolPath);
    if (!E::TryGetActorLocation(player, me) || !ue_wrap::object_index::ClassByName(kToolClass)) return false;
    void* gun = DW::SpawnToolgun({me.X, me.Y, me.Z + 60.f});
    g_toolgun.Set(gun);
    return gun != nullptr;
}

// A press at `ref`'s actor: the notice count for the leg is taken first.
bool Fire(void* player, const ue_wrap::CachedObjRef& ref, const char* notice) {
    void* target = ref.Get();
    if (!target) { Abandon("the target is gone before its press"); return false; }
    g_noticeBase = notice ? NoticeCount(notice) : 0;
    if (!Press(player, target)) { AbandonAim("the press did not run"); return false; }
    return true;
}

// Fire for the hook steps: a press that cannot be made there is the lane's failure (a hook has pulled the crowbar out of
// this camera's sight), never an abandoned drill.
bool FireOrFail(void* player, const ue_wrap::CachedObjRef& ref, const char* notice) {
    void* target = ref.Get();
    if (!target) { Fail("the crowbar is gone before its press"); return false; }
    g_noticeBase = NoticeCount(notice);
    if (!Press(player, target)) {
        char why[560];
        std::snprintf(why, sizeof(why), "the press on the hooked crowbar did not run: %s", g_aimWhy);
        Fail(why);
        return false;
    }
    return true;
}

// Is the leg's target gone from this copy. Fails the drill on a refusal the leg did not expect.
bool GoneOrFail(const ue_wrap::CachedObjRef& ref, const char* unexpected, const char* failWhy) {
    if (!ref.Alive()) return true;
    if (unexpected && NoticeCount(unexpected) > g_noticeBase) { Fail(failWhy); return false; }
    if (Expired(kLegBoundMs)) Fail("the target was not removed within 30 s of the press");
    return false;
}

void Finish(coop::net::Session* s) {
    // The host judges when it commits the closing line.
    if (!SayToHost(s, EndLine())) {
        Abandon("the closing chat line was not sent");
        return;
    }
    End("client", "PASS", "every leg held on this copy; the closing line is sent for the host to judge", false);
}

void ClientTick(coop::net::Session* s) {
    if (g_done) return;
    const uint64_t now = ::GetTickCount64();
    if (now < g_nextCheckMs) return;
    g_nextCheckMs = now + kCheckEveryMs;
    void* player = coop::players::Registry::Get().Local();
    if (!player) return;
    const bool granted = PhaseOf() == Phase::Granted;
    switch (g_step) {
    case Step::Arm:
        if (!coop::net_pump::HasAnnouncedWorldReady() ||
            coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle)
            return;
        Next(Step::Find);
        return;
    case Step::Find:
        if (!Find(player)) {
            if (Expired(kFindBoundMs)) Abandon("the host's props did not all reach this client within 60 s");
            return;
        }
        if (g_done) return;
        if (!g_toolgun.Alive() && !SpawnToolgun(player)) {
            Abandon("the toolgun or the remover tool could not be spawned");
            return;
        }
        UE_LOGI("[REMOVER-DRILL] client: the host's props are here and the toolgun is up (%s)", PhaseName());
        Next(Step::Settle);
        return;
    case Step::Settle:
        g_restReads = AtRest() ? g_restReads + 1 : 0;
        if (g_restReads < kRestChecks) {
            if (Expired(kFindBoundMs)) Abandon("the host's props did not come to rest within 60 s");
            return;
        }
        if (!Choose(player, s)) return;
        UE_LOGI("[REMOVER-DRILL] client: the host's props are at rest and the camera's trace meets each target first");
        Next(Step::Single);
        return;
    case Step::Relay:
        // The host lays the layout again and says so; or it has no further direction.
        if (NoticeCount(("remover-drill laid n=" + std::to_string(g_relay) + ";").c_str()) > 0) {
            Next(Step::Cleared);
        } else if (NoticeCount("remover-drill relay-failed;") > 0) {
            Abandon(("the host has no direction left that its own trace clears; every one it laid was blocked from this "
                     "camera -- " + g_relayWhy).c_str());
        } else if (Expired(kFindBoundMs)) {
            Abandon("the host did not answer the relay within 60 s");
        }
        return;
    case Step::Cleared: {
        // The old layout's mirrors go with the host's destroys; the roles are found again only once every one has gone, or
        // the find would take an old prop for a new one.
        bool gone = !g_single.Alive() && !g_mash.Alive() && !g_npc.Alive() && !g_hostHooked.Alive() && !g_selfHooked.Alive();
        for (int i = 0; i < g_ringN; ++i) gone = gone && !g_ring[i].Alive();
        if (!gone) {
            if (Expired(kFindBoundMs)) Abandon("the old layout's mirrors did not go within 60 s of the re-lay");
            return;
        }
        g_single.Reset();
        g_mash.Reset();
        g_npc.Reset();
        g_hostHooked.Reset();
        g_selfHooked.Reset();
        for (auto& r : g_ring) r.Reset();
        for (auto& r : g_sweep) r.Reset();
        for (auto& p : g_restAt) p = {};
        g_ringN = 0;
        g_sweepN = 0;
        g_restReads = 0;
        Next(Step::Find);
        return;
    }
    case Step::Single:
        if (Fire(player, g_single, kNoRight)) Next(Step::SingleWait);
        return;
    case Step::SingleWait:
        if (granted) {
            if (GoneOrFail(g_single, kNoRight, "the first prop was refused though the grant stands"))
                LegPass("single", Step::Mash);
            return;
        }
        // withheld: the copy must never go, and the refusal must show.
        if (!g_single.Alive()) { Fail("the refused prop's copy vanished on this client"); return; }
        if (NoticeCount(kNoRight) > g_noticeBase) { LegPass("withheld", Step::Finish); return; }
        if (Expired(kLegBoundMs)) Fail("the host's refusal was not shown within 30 s");
        return;
    case Step::Mash: {
        g_noticeBase = NoticeCount(kNoRight);
        // One aim, then every press in this tick from it, before any destroy can come back: the mash is six presses of one
        // shot, so the host answers the first Removed and the rest Gone.
        void* target = g_mash.Get();
        ue_wrap::trace::Hit hit;
        if (!target) { Abandon("the mashed prop is gone before its presses"); return; }
        if (!AimedHit(player, target, &hit)) { AbandonAim("the mash aim failed"); return; }
        int shots = 0;
        for (int i = 0; i < kMashShots; ++i) {
            void* t = g_mash.Get();
            if (!t) break;   // a press that removed the prop here at once leaves nothing to press
            if (!PressWith(player, t, hit)) { AbandonAim("a mash press did not run"); return; }
            ++shots;
        }
        UE_LOGI("[REMOVER-DRILL] client: %d presses on one prop in one tick", shots);
        Next(Step::MashWait);
        return;
    }
    case Step::MashWait:
        if (GoneOrFail(g_mash, kNoRight, "the mashed prop was refused though the grant stands")) {
            g_sweepAt = 0;
            LegPass("mash", Step::Sweep);
        }
        return;
    case Step::Sweep:
        if (g_sweepAt >= g_sweepN) { LegPass("sweep", Step::Npc); return; }
        if (Fire(player, g_sweep[g_sweepAt], kOutOfSight)) Next(Step::SweepWait);
        return;
    case Step::SweepWait:
        if (GoneOrFail(g_sweep[g_sweepAt], kOutOfSight, "a prop in this client's sight was refused for sight")) {
            ++g_sweepAt;
            Next(Step::Sweep);
        }
        return;
    case Step::Npc:
        if (Fire(player, g_npc, kUnsupported)) Next(Step::NpcWait);
        return;
    case Step::NpcWait:
        if (!g_npc.Alive()) { Fail("the creature was removed on this client"); return; }
        if (NoticeCount(kUnsupported) > g_noticeBase) { LegPass("unsupported", Step::HostHooked); return; }
        if (Expired(kLegBoundMs)) Fail("the remover's refusal of the creature was not shown within 30 s");
        return;
    case Step::HostHooked:
        // The host's own hook must pull the first crowbar before the press; its line says the claim pass reads it.
        if (NoticeCount(HookedLine(kHostHookedSaid, g_relay).c_str()) == 0) {
            if (Expired(kFindBoundMs)) Fail("the host never said its hook pulls the first crowbar within 60 s");
            return;
        }
        if (FireOrFail(player, g_hostHooked, kHeldNotice)) Next(Step::HostHookedWait);
        return;
    case Step::HostHookedWait:
        if (!g_hostHooked.Alive()) { Fail("the crowbar the host's hook pulls was removed on this client"); return; }
        if (NoticeCount(kHeldNotice) > g_noticeBase) { LegPass("host-hooked", Step::SelfHooked); return; }
        if (Expired(kLegBoundMs)) Fail("the host's refusal of the host-hooked crowbar was not shown within 30 s");
        return;
    case Step::SelfHooked: {
        // This client's own hook on the second crowbar first, then the host's line that its claim pass reads that hook.
        if (!g_selfHook.Raw()) {
            void* target = g_selfHooked.Get();
            if (!target) { Fail("the second crowbar is gone before this client's hook"); return; }
            const char* note = "";
            void* hook = coop::dev::hook_plant::Plant(player, target, &note);
            if (!hook) {
                char why[120];
                std::snprintf(why, sizeof(why), "this client's hook could not be planted: %s", note);
                Fail(why);
                return;
            }
            g_selfHook.Set(hook);
            UE_LOGI("[REMOVER-DRILL] client: its own hook is planted on the second crowbar (hook %p)", hook);
            return;
        }
        if (NoticeCount(HookedLine(kSelfHookedSaid, g_relay).c_str()) == 0) {
            if (Expired(kFindBoundMs)) Fail("the host never said this client's hook pulls the second crowbar within 60 s");
            return;
        }
        if (FireOrFail(player, g_selfHooked, kHeldNotice)) Next(Step::SelfHookedWait);
        return;
    }
    case Step::SelfHookedWait:
        // A client may remove what its own hook pulls, as what it drags: the copy goes, and the refusal never shows.
        if (GoneOrFail(g_selfHooked, kHeldNotice, "the crowbar this client's own hook pulls was refused as held")) {
            ReleaseHook(g_selfHook);
            LegPass("self-hooked", Step::Finish);
        }
        return;
    case Step::Finish:
        Finish(s);
        return;
    }
}

}  // namespace

namespace detail {

Phase PhaseOf() {
    static const Phase p = [] {
        const std::string v = coop::config::ResolveEnum(::coop::config_registry::rows::remover_drill);
        return v == "granted" ? Phase::Granted : v == "withheld" ? Phase::Withheld : Phase::Off;
    }();
    return p;
}
const char* PhaseName() { return PhaseOf() == Phase::Granted ? "granted" : "withheld"; }

std::string EndLine() { return std::string("remover-drill end ") + PhaseName(); }

int  g_session = 1;
bool g_done = false;
uint64_t g_stepMs = 0;
uint64_t g_nextCheckMs = 0;

bool Expired(uint64_t bound) { return ::GetTickCount64() - g_stepMs > bound; }

void End(const char* role, const char* word, const char* what, bool warn) {
    if (warn) UE_LOGW("[REMOVER-DRILL] %s %s %s in session %d: %s", role, word, PhaseName(), g_session, what);
    else      UE_LOGI("[REMOVER-DRILL] %s %s %s in session %d: %s", role, word, PhaseName(), g_session, what);
    g_done = true;
}

std::string HookedLine(const char* said, int relays) {
    return std::string(said) + " n=" + std::to_string(relays) + ";";
}

// The item's cancel for a hook this drill planted: the field the plant wrote is cleared whether or not the hook still
// stands, then the actor goes.
void ReleaseHook(ue_wrap::CachedObjRef& hook) {
    if (void* player = coop::players::Registry::Get().Local()) H::WriteActiveHook(player, nullptr);
    if (void* h = hook.Get()) E::DestroyActor(h);
    hook.Reset();
}

}  // namespace detail

void Tick(coop::net::Session* s) {
    if (PhaseOf() == Phase::Off || !s) return;
    if (s->role() == coop::net::Role::Host) HostTick(s);
    else if (s->connected()) ClientTick(s);
}

void OnDisconnect() {
    if (PhaseOf() == Phase::Off) return;
    ++g_session;
    g_done = false;
    HostReset();
    g_step = Step::Arm;
    g_stepMs = g_nextCheckMs = 0;
    layout::Reset();
    if (g_selfHook.Raw()) ReleaseHook(g_selfHook);
    g_toolgun.Reset();
    g_single.Reset();
    g_mash.Reset();
    g_npc.Reset();
    g_hostHooked.Reset();
    g_selfHooked.Reset();
    for (auto& r : g_sweep) r.Reset();
    g_sweepAt = 0;
    g_sweepN = 0;
    g_ringN = 0;
    g_relay = 0;
    g_relayWhy.clear();
    g_restReads = 0;
    for (auto& r : g_ring) r.Reset();
    for (auto& p : g_restAt) p = {};
    g_noticeBase = 0;
}

}  // namespace coop::dev::remover_drill
