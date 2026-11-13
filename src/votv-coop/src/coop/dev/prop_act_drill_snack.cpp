// coop/dev/prop_act_drill_snack.cpp -- the act drill's phase `snack`: a client uses a world snack through the host. Each
// arm is one snack on the honest phase's step machine: the host spawns it, writes its copy through its record and
// publishes it, and says it ready with the copy's uses and open; the client acts (action 4 on its mirror, or the hold
// of it) and judges its own copies; the host judges its counters and its copy. See coop/dev/prop_act_drill.h.

#include "coop/dev/prop_act_drill.h"

#include "coop/element/registry.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_act_host.h"
#include "coop/props/prop_act_request.h"
#include "coop/props/prop_hold.h"
#include "coop/props/prop_save_data.h"
#include "coop/props/snack_act.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/prop_use.h"
#include "ue_wrap/actors/snack.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace coop::dev::prop_act_drill::detail {
namespace {

namespace E   = ue_wrap::engine;
namespace R   = ue_wrap::reflection;
namespace EL  = coop::element;
namespace PAH = coop::prop_act_host;
namespace PAR = coop::prop_act_request;
namespace SN  = ue_wrap::snack;
using ue_wrap::FRotator;
using ue_wrap::FVector;

constexpr const wchar_t* kChips = L"/Game/objects/prop_snack_chips1.prop_snack_chips1_C";
constexpr const wchar_t* kBeer  = L"/Game/objects/prop_snack_beer.prop_snack_beer_C";
constexpr const wchar_t* kWine  = L"/Game/objects/prop_snack_wine.prop_snack_wine_C";

constexpr float kWrapperRadiusCm = 300.f;      // a snack's wrapper is born at its place
constexpr uint64_t kSpillBoundMs = 60000;      // HOST: the tilted beer to a spilled use
constexpr float kTiltPitch = 100.f;            // past upside-down by 10 deg, where the spill's weight is sin(10 deg) a frame
constexpr uint16_t kReserveId = 0xFFF0;        // the host's own reservation for the client's slot (the cancel arm)
constexpr uint8_t kEatAction = 4;

uint32_t Since(uint32_t now, uint32_t then) { return now - then; }

// What this peer read of its snack before the act: the wrapper's name (the snack's `trash`) and its place.
R::FName g_trash{0, 0};
FVector g_pos{};
// The host's copy as it was said ready: the judge reckons from it, and the client reads it off the ready line.
int32_t g_readyUses = 0;
bool g_readyOpen = false;
int32_t g_spillStart = 0;

bool Capture(void* snack) {
    static R::InstanceOffset sTrash{L"trash"};
    const int32_t o = snack ? sTrash.Of(snack) : -1;
    if (o < 0 || !E::TryGetActorLocation(snack, g_pos)) return false;
    std::memcpy(&g_trash, static_cast<const uint8_t*>(snack) + o, sizeof(g_trash));
    return true;
}

// ---- the wrapper census ---------------------------------------------------------------------------------------------

struct ClassCollector {
    void* base;
    std::vector<void*> classes;
};

void CollectClass(void* ctx, void* cls, void*) {
    auto* c = static_cast<ClassCollector*>(ctx);
    if (cls == c->base || R::IsDescendantOfAny(cls, &c->base, 1)) c->classes.push_back(cls);
}

void CollectInstance(void* ctx, void* obj, int32_t) {
    static R::InstanceOffset sName{L"name"};
    const int32_t o = sName.Of(obj);
    if (o < 0) return;
    R::FName n{};
    std::memcpy(&n, static_cast<const uint8_t*>(obj) + o, sizeof(n));
    if (n.ComparisonIndex == g_trash.ComparisonIndex && n.Number == g_trash.Number)
        static_cast<std::vector<void*>*>(ctx)->push_back(obj);
}

// The live prop_C descendants named by the snack's trash within the radius of where it stood: `total` of them, and
// `tracked` of those that have an element (the host's own, and a client's mirror of it; a client's refused newborn
// has none). The instances are gathered first and read after, since a read is an engine call.
void WrapperCensus(uint32_t* total, uint32_t* tracked) {
    *total = *tracked = 0;
    if (g_trash.ComparisonIndex == 0) return;
    void* const propClass = ue_wrap::object_index::ClassByName(L"prop_C");
    if (!propClass) return;
    ClassCollector classes{propClass, {}};
    ue_wrap::object_index::ForEachClass(&CollectClass, &classes);
    std::vector<void*> found;
    for (void* cls : classes.classes) ue_wrap::object_index::ForEachInstance(cls, &CollectInstance, &found);
    for (void* obj : found) {
        FVector at{};
        if (!R::IsLive(obj) || !E::TryGetActorLocation(obj, at)) continue;
        const float dx = at.X - g_pos.X, dy = at.Y - g_pos.Y, dz = at.Z - g_pos.Z;
        if (std::sqrt(dx * dx + dy * dy + dz * dz) > kWrapperRadiusCm) continue;
        ++*total;
        if (EL::Registry::Get().EidForActor(obj) != EL::kInvalidId) ++*tracked;
    }
}

// ---- the arms -------------------------------------------------------------------------------------------------------

//                  hostUses hostOpen clientUses full  spends opens cancelHost spill
const SnackArm kArms[] = {
    /* 0 sip        */ {-1, -1, -1, false, true,  false, false, false},
    /* 1 full eater */ {-1, -1, -1, true,  false, false, false, false},
    /* 2 beer open  */ { 8,  0, -1, false, false, true,  false, false},
    /* 3 beer sip   */ { 8,  1, -1, false, true,  false, false, false},
    /* 4 cancel     */ {-1, -1, -1, false, true,  false, true,  false},
    /* 5 respawn    */ { 3, -1,  1, false, true,  false, false, false},
    /* 6 last chips */ { 1, -1, -1, false, true,  false, false, false},
    /* 7 last beer  */ { 1,  1, -1, false, true,  false, false, false},
    /* 8 last wine  */ { 1,  1, -1, false, true,  false, false, false},
    /* 9 spill hold */ { 8,  1, -1, false, false, false, false, true},
};

template <class T>
bool WriteField(R::InstanceOffset& field, void* obj, T value) {
    const int32_t o = obj ? field.Of(obj) : -1;
    if (o < 0) return false;
    std::memcpy(static_cast<uint8_t*>(obj) + o, &value, sizeof(value));
    return true;
}

// CLIENT, before the act: the snack's wrapper name and place are kept, and the arm's own write to the mirror's uses
// (the drill's instrument: the mirror runs ahead of the host's copy, as a beer's local spill leaves it).
template <size_t I>
const char* SetupArm(void*, void* mirror) {
    static R::InstanceOffset sUses{L"uses"};
    if (!Capture(mirror)) return "the mirror's wrapper name or place could not be read";
    if (kArms[I].clientUses >= 0 && !WriteField<int32_t>(sUses, mirror, kArms[I].clientUses))
        return "the mirror's uses could not be written";
    return nullptr;
}

// The cancel arm: the host holds a reservation for the client's slot and acts on its own copy. The watch refuses the
// host's call, so the copy keeps its uses and the cancel counts.
const char* CancelHostUse(void* actor) {
    int32_t uses = 0;
    bool open = false;
    void* const player = coop::players::Registry::Get().Local();
    FVector at{};
    if (!player || !SN::ReadSnackState(actor, uses, open) || !E::TryGetActorLocation(actor, at))
        return "the host's copy or player could not be read";
    void* const root = RootOf(actor);
    const EL::ElementId eid = EL::Registry::Get().EidForActor(actor);
    if (!coop::prop_hold::Reserve(kClientSlot, kReserveId, static_cast<uint32_t>(eid), actor))
        return "the host's reservation for the client's slot was refused";
    const uint32_t cancelsBefore = PAR::dev::HostCancels();
    ue_wrap::prop_use::CallActionOptionIndex(actor, player, root, at, kEatAction, root);
    coop::prop_hold::Release(kClientSlot);
    int32_t after = 0;
    bool openAfter = false;
    if (!SN::ReadSnackState(actor, after, openAfter) || after != uses || openAfter != open)
        return "the host's own action 4 changed its copy while a client held the reservation";
    if (PAR::dev::HostCancels() != cancelsBefore + 1)
        return "the host's own action 4 was not cancelled for the reservation";
    UE_LOGI("%s host snack: the host's own action 4 was cancelled for the client's reservation, uses %d kept", kTag, uses);
    return nullptr;
}

// The spill arm: the beer is tilted to where its own tick spills a use, and put back exactly as it was once its uses have
// fallen. The beer's tick (prop_snack_beer's ReceiveTick, its spill branch)
// returns unless `open`, then spends a use with the chance RandomBoolWithWeight(up . (0,0,-1)):
// a pitch of 100 degrees with no roll points its up vector 10 degrees past horizontal, a weight of about 0.17 a tick,
// whatever roll the bottle settled at. The tilt is made with physics OFF and the place pinned: a bottle lying on the floor
// turned about its origin by a teleport with physics on has a corner in the floor and the solver pushes it through the
// world (measured: z 6317 down to -11509). The spill's own mark publishes it. The pose before the tilt is recorded, and
// RestoreTilt puts back exactly that rotation and physics: it runs on the spill, on every INVALID or FAIL return after the
// pin, and when the host's phase ends (SnackRestoreTilt).
struct Tilt {
    ue_wrap::CachedObjRef actor;
    FRotator rot{};
    FVector at{};
    bool on = false;
    int samples = 0;   // host polls still to log the beer's state after the restore
};
Tilt g_tilt;

// AActor::GetActorEnableCollision of the beer, 1 or 0; -1 when it could not be read.
int ReadCollision(void* beer) {
    void* const fn = R::FindDispatchFunctionCached(R::ClassOf(beer), L"GetActorEnableCollision");
    if (!fn) return -1;
    ue_wrap::ParamFrame f(fn);
    return f.valid() && ue_wrap::Call(beer, f) ? (f.Get<bool>(L"ReturnValue") ? 1 : 0) : -1;
}

// What the fall after a restore is read from (the cause was never measured): the beer's height against the recorded one, the
// actor's collision flag, whether its root simulates and its speed.
void SampleBeer(const char* when) {
    void* const beer = g_tilt.actor.Get();
    if (!beer) return;
    FVector at{}, lin{}, ang{};
    E::TryGetActorLocation(beer, at);
    E::GetActorRootPhysicsVelocity(beer, lin, ang);
    UE_LOGI("%s host snack: beer %s: z=%.1f (recorded %.1f) collision=%d simulating=%d speed=%.0f cm/s", kTag, when, at.Z,
            g_tilt.at.Z, ReadCollision(beer), E::IsActorRootSimulatingPhysics(beer) ? 1 : 0,
            std::sqrt(lin.X * lin.X + lin.Y * lin.Y + lin.Z * lin.Z));
}

// Puts the beer back as it was: the recorded pose first (rotation and place, physics still off), the actor's collision on
// (UPrimitiveComponent::SetSimulatePhysics changes the simulation only and the pin never touched collision, so this
// is made certain and not assumed), then physics, then its velocity zeroed so nothing the body carried out of the pin
// moves it. Its state is logged before and after, and on the polls that follow (SnackHostMarker).
void RestoreTilt() {
    if (!g_tilt.on) return;
    g_tilt.on = false;
    void* const beer = g_tilt.actor.Get();
    if (!beer) return;
    SampleBeer("pinned, before the restore");
    const bool posed = E::SetActorRotation(beer, g_tilt.rot) && E::SetActorLocation(beer, g_tilt.at);
    E::SetActorEnableCollision(beer, true);
    const bool simulating = E::SetActorSimulatePhysics(beer, true);
    E::SetActorRootPhysicsVelocity(beer, FVector{}, FVector{});
    if (!posed || !simulating) UE_LOGW("%s host snack: the tilted beer could not be put back as it was", kTag);
    SampleBeer("right after the restore");
    g_tilt.samples = 5;
}

const char* TiltFailed(const char* why) {
    RestoreTilt();
    return why;
}

const char* TiltUntilSpilled(void* actor, uint64_t sinceMs, bool* done) {
    int32_t uses = 0;
    bool open = false;
    if (sinceMs == 0) {
        FRotator rot{};
        if (!SN::ReadSnackState(actor, uses, open) || !E::TryGetActorRotation(actor, rot)) return "the beer's state could not be read";
        g_tilt.actor.Set(actor);
        g_tilt.rot = rot;
        E::TryGetActorLocation(actor, g_tilt.at);
        g_tilt.on = true;
        FRotator tilt{kTiltPitch, rot.Yaw, 0.f};
        if (!E::SetActorSimulatePhysics(actor, false)) return TiltFailed("the beer's physics could not be stopped for the tilt");
        return E::SetActorRotation(actor, tilt) ? nullptr : TiltFailed("the beer could not be tilted");
    }
    if (!SN::ReadSnackState(actor, uses, open)) return TiltFailed("the beer's state could not be read");
    if (uses < g_spillStart) {
        RestoreTilt();
        UE_LOGI("%s host snack: the tilted beer spilled %d use(s) by its own tick, put back as it was", kTag,
                g_spillStart - uses);
        *done = true;
        return nullptr;
    }
    return sinceMs > kSpillBoundMs ? TiltFailed("INVALID: the tilted beer spilled no use in the bound") : nullptr;
}

// HOST, after the target is announced: the copy's capture and writes on the first call, then the arm's own work.
template <size_t I>
const char* PrepArm(void* actor, uint64_t sinceMs, bool* done) {
    const SnackArm& a = kArms[I];
    if (sinceMs == 0) {
        int32_t uses = 0;
        bool open = false;
        if (!Capture(actor) || !SN::ReadSnackState(actor, uses, open)) return "the host copy's state could not be read";
        const int32_t wantUses = a.hostUses >= 0 ? a.hostUses : uses;
        const bool wantOpen = a.hostOpen >= 0 ? a.hostOpen != 0 : open;
        if (wantUses != uses || wantOpen != open) {
            if (!SN::PatchSnackRecord(actor, wantUses, wantOpen)) return "the host copy's record could not be patched";
            coop::snack_act::dev::MarkForPublication(actor);
        }
        g_spillStart = wantUses;
        if (a.cancelHost) {
            if (const char* why = CancelHostUse(actor)) return why;
        }
    }
    if (!a.spill) {
        *done = true;
        return nullptr;
    }
    return TiltUntilSpilled(actor, sinceMs, done);
}

// What the host's copy holds once the arm's act has landed, from what it held when it said the step ready.
void Expect(const SnackArm& a, int32_t readyUses, bool readyOpen, int32_t* uses, bool* open) {
    *uses = readyUses - ((a.spends && !a.full) ? 1 : 0);
    *open = readyOpen || a.opens;
}

bool ParseTail(const std::string& rest, int32_t* uses, bool* open) {
    const size_t u = rest.find(" uses=");
    const size_t o = rest.find(" open=");
    if (u == std::string::npos || o == std::string::npos) return false;
    *uses = static_cast<int32_t>(std::strtol(rest.c_str() + u + 6, nullptr, 10));
    *open = std::strtol(rest.c_str() + o + 6, nullptr, 10) != 0;
    return true;
}

template <size_t I>
constexpr Row ArmRow(const char* name, const wchar_t* path, const wchar_t* leaf, Verb verb, Ends ends) {
    return Row{name, path, leaf, verb, ends, &SetupArm<I>, Target::Host, true, false, nullptr, nullptr, false,
               PAH::Tally::kCount, &PrepArm<I>, &kArms[I]};
}

const Row kRows[] = {
    ArmRow<0>("snack sip",    kChips, L"prop_snack_chips1_C", Verb::Use,  Ends::Applied),
    ArmRow<1>("snack full",   kChips, L"prop_snack_chips1_C", Verb::Use,  Ends::Applied),
    ArmRow<2>("beer open",    kBeer,  L"prop_snack_beer_C",   Verb::Use,  Ends::Applied),
    ArmRow<3>("beer sip",     kBeer,  L"prop_snack_beer_C",   Verb::Use,  Ends::Applied),
    ArmRow<4>("host cancel",  kChips, L"prop_snack_chips1_C", Verb::Use,  Ends::Applied),
    ArmRow<5>("snack respawn", kChips, L"prop_snack_chips1_C", Verb::Use, Ends::Respawn),
    ArmRow<6>("last chips",   kChips, L"prop_snack_chips1_C", Verb::Use,  Ends::Ended),
    ArmRow<7>("last beer",    kBeer,  L"prop_snack_beer_C",   Verb::Use,  Ends::Ended),
    ArmRow<8>("last wine",    kWine,  L"prop_snack_wine_C",   Verb::Use,  Ends::Ended),
    ArmRow<9>("spilled hold", kBeer,  L"prop_snack_beer_C",   Verb::Hold, Ends::Hand),
};

// The host's verdict: the counters the arm must have moved, the host's own copy, and for a last use the host's one
// wrapper. The client judged its own copies and said ok before this runs.
Judged JudgeSnack(const Row& row, EL::ElementId eid, const Baseline& base, std::string* why) {
    const SnackArm& a = *row.snack;
    void* const host = EL::LivePropActor(eid);
    if (row.verb == Verb::Hold) {
        const uint32_t destroys = Since(PAH::CommitsOf(kClientSlot, PAH::internal::CommitStep::Destroy), base.destroys);
        if (destroys != 1 || host) {
            *why = std::to_string(destroys) + " destroy commits for the hold, host copy " + (host ? "live" : "gone");
            return destroys == 0 ? Judged::Wait : Judged::Fail;
        }
        *why = "the hold of the spilled beer was one destroy commit and the host's copy is gone";
        return Judged::Pass;
    }
    const uint32_t applied = Since(PAH::CommitsOf(kClientSlot, PAH::internal::CommitStep::SnackApplied), base.snackApplied);
    const uint32_t ended = Since(PAH::CommitsOf(kClientSlot, PAH::internal::CommitStep::SnackEnded), base.snackEnded);
    const bool wantEnded = row.ends != Ends::Applied;
    if ((wantEnded ? ended : applied) == 0) {
        *why = std::string("no snack commit of the kind ") + (wantEnded ? "ended" : "applied") + " yet";
        return Judged::Wait;
    }
    if ((wantEnded ? ended : applied) != 1 || (wantEnded ? applied : ended) != 0) {
        *why = std::to_string(applied) + " applied and " + std::to_string(ended) + " ended snack commits";
        return Judged::Fail;
    }
    int32_t wantUses = 0;
    bool wantOpen = false;
    Expect(a, g_readyUses, g_readyOpen, &wantUses, &wantOpen);
    if (row.ends == Ends::Ended) {
        if (host) {
            *why = "the host's copy is still live after its last use";
            return Judged::Fail;   // the end commit is counted and the host ends its copy in the same call: final
        }
        uint32_t total = 0, tracked = 0;
        WrapperCensus(&total, &tracked);
        if (total != 1 || tracked != 1) {
            *why = std::to_string(total) + " wrappers on the host (" + std::to_string(tracked) + " tracked), 1 expected";
            // The wrapper is born inside the commit, so none or two is final; one not yet tracked waits for the expression tick.
            return total == 1 ? Judged::Wait : Judged::Fail;
        }
        *why = "one snack commit (ended), the host's copy gone and one wrapper on the host";
        return Judged::Pass;
    }
    int32_t uses = 0;
    bool open = false;
    if (!host || !SN::ReadSnackState(host, uses, open)) {
        *why = "the host's copy is gone or unread after a use that leaves it";
        return Judged::Fail;
    }
    if (uses != wantUses || open != wantOpen) {
        *why = "the host's copy holds uses " + std::to_string(uses) + " open " + (open ? "1" : "0") + ", expected uses " +
               std::to_string(wantUses) + " open " + (wantOpen ? "1" : "0");
        return Judged::Fail;
    }
    *why = "one snack commit, the host's copy at uses " + std::to_string(uses) + " open " + (open ? "1" : "0");
    return Judged::Pass;
}

}  // namespace

const Phase& Snack() {
    static const Phase p{"snack", kRows, sizeof(kRows) / sizeof(kRows[0]), &JudgeSnack};
    return p;
}

std::string SnackCopyNow(void* mirror) {
    int32_t uses = 0;
    bool open = false;
    if (!SN::ReadSnackState(mirror, uses, open)) return "unread";
    return "uses " + std::to_string(uses) + " open " + (open ? "1" : "0");
}

std::string SnackSettledHead(const Row& row) { return std::string(kTag) + " host " + row.name + " settled"; }

namespace {
bool g_settledSaid = false;
}

void SnackStepReset() {
    g_settledSaid = false;
    RestoreTilt();
    g_tilt.samples = 0;
    g_tilt.actor.Reset();
}

void SnackRestoreTilt() { RestoreTilt(); }

// HOST, each poll while it waits for the client: once the host has processed the act (its commit counted, its copy
// applied, published or ended, its wrapper expressed) it says `settled`, once. The line rides the lobby chat, which is
// the Default lane the host's PropSpawn, record and PropDestroy ride, FIFO behind them
// (session_lanes.h LaneForKind, send_backlog.cpp SendLocked_), so a client that reads it has already taken every
// birth, record and destroy the host sent for the act: what its copies show then cannot change, and it decides on it.
void SnackHostMarker(const Row& row, EL::ElementId eid, const Baseline& base) {
    if (g_tilt.samples > 0) {
        --g_tilt.samples;
        SampleBeer("after the restore");
    }
    if (g_settledSaid || coop::snack_act::dev::MarksPending()) return;
    // The host's record for the copy has gone out, not parked or refused and waiting to go again.
    if (void* const host = EL::LivePropActor(eid)) {
        if (coop::prop_save_data::dev::Unsettled(ue_wrap::prop::GetInteractableKeyString(host))) return;
    }
    std::string why;
    if (JudgeSnack(row, eid, base, &why) == Judged::Wait) return;
    g_settledSaid = true;
    Say(SnackSettledHead(row));
    UE_LOGI("%s host %s: settled (%s)", kTag, row.name, why.c_str());
}

std::string SnackReadyTail(void* hostActor) {
    bool open = false;
    if (!SN::ReadSnackState(hostActor, g_readyUses, open)) g_readyUses = -1;
    g_readyOpen = open;
    return " uses=" + std::to_string(g_readyUses) + " open=" + (open ? "1" : "0");
}

Judged SnackClientCopy(const Row& row, const ue_wrap::CachedObjRef& was, EL::ElementId eid, const std::string& tail,
                       bool settled, std::string* why) {
    int32_t readyUses = 0;
    bool readyOpen = false;
    if (!ParseTail(tail, &readyUses, &readyOpen)) {
        *why = "the host's ready line carried no uses and open";
        return Judged::Fail;
    }
    int32_t wantUses = 0;
    bool wantOpen = false;
    Expect(*row.snack, readyUses, readyOpen, &wantUses, &wantOpen);
    void* const now = EL::LivePropActor(eid);
    // The host said `settled`, and this client's record for the copy is not parked or held by the apply budget: its state is final.
    if (now && coop::prop_save_data::dev::Unsettled(ue_wrap::prop::GetInteractableKeyString(now))) settled = false;
    if (row.ends == Ends::Ended) {
        if (was.Alive() || now) {
            *why = "this client's copy is still live after the last use";
            return settled ? Judged::Fail : Judged::Wait;
        }
        uint32_t total = 0, tracked = 0;
        WrapperCensus(&total, &tracked);
        if (tracked == 0) {
            *why = "the host's wrapper has not reached this client (" + std::to_string(total) + " untracked)";
            return settled ? Judged::Fail : Judged::Wait;
        }
        if (total != 1) {
            *why = std::to_string(total) + " wrappers on this client, 1 expected (a ghost beside the host's)";
            return Judged::Fail;
        }
        *why = "the snack is gone here and one wrapper, the host's, stands";
        return Judged::Pass;
    }
    if (!now) {
        *why = "the mirror is not on this client";
        return settled ? Judged::Fail : Judged::Wait;
    }
    if (row.ends == Ends::Respawn ? was.Is(now) : !was.Is(now)) {
        *why = row.ends == Ends::Respawn ? "the old copy still stands: the host's birth has not come"
                                         : "the mirror was replaced on a use that leaves it";
        return row.ends == Ends::Respawn && !settled ? Judged::Wait : Judged::Fail;
    }
    int32_t uses = 0;
    bool open = false;
    if (!SN::ReadSnackState(now, uses, open) || uses != wantUses || open != wantOpen) {
        *why = "this client's copy holds uses " + std::to_string(uses) + " open " + (open ? "1" : "0") +
               ", expected uses " + std::to_string(wantUses) + " open " + (wantOpen ? "1" : "0");
        return settled ? Judged::Fail : Judged::Wait;
    }
    *why = "this client's copy holds uses " + std::to_string(uses) + " open " + (open ? "1" : "0");
    return Judged::Pass;
}

bool SnackMirrorHoldsReady(void* mirror, const std::string& tail) {
    int32_t readyUses = 0, uses = 0;
    bool readyOpen = false, open = false;
    return ParseTail(tail, &readyUses, &readyOpen) && mirror && SN::ReadSnackState(mirror, uses, open) &&
           uses == readyUses && open == readyOpen;
}

const char* SnackHeldVerdict(void* held, const std::string& tail) {
    int32_t readyUses = 0, uses = 0;
    bool readyOpen = false, open = false;
    if (!ParseTail(tail, &readyUses, &readyOpen)) return "the host's ready line carried no uses and open";
    if (!SN::ReadSnackState(held, uses, open)) return "the held item's uses could not be read";
    return uses == readyUses && open == readyOpen
               ? nullptr
               : "the held beer's uses or open differ from the host's copy (the spilled use did not reach the mirror)";
}

}  // namespace coop::dev::prop_act_drill::detail
