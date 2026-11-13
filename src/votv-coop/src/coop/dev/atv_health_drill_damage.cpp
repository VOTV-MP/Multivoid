// coop/dev/atv_health_drill_damage.cpp -- the ATV health drill's damage arms, `gate`, `owner` and `trap`: see
// coop/dev/atv_health_drill.h. The host spawns an ATV beside the players (`trap` alone uses the world's own ATV, which the
// gamemode's `car` is) and the client walks to it with the director and sits in it, so its copy owns the tick. gate: the
// host's native blast must be cancelled on the host's copy and run on the client's. owner: the client's kill of its copy is
// one request the host accepts, marking the ATV blown, and the lane sends no second request for it when the host's
// blast lands. trap: the host's story-event trap reaches the seated owner's
// copy, fires there as one request, and the re-arm lets a second kill through. Both peers hold the explosion drill's
// guard from the ready line to the verdict or a reset. Every judge polls from its entering edge until its counters hold
// or its bound passes.

#include "atv_health_drill_detail.h"  // co-located private header (src tree, not include/)
#include "explosion_drill_source.h"   // Guard, SpawnNativeBlast (co-located private header)

#include "coop/dev/drill_chat.h"
#include "coop/interactables/atv_damage_gate.h"
#include "coop/interactables/atv_sync.h"
#include "coop/net/session.h"
#include "coop/world/event_fire_sync.h"
#include "coop/world/explosion_request.h"
#include "coop/world/explosion_sync.h"

#include "ue_wrap/actors/int_objects.h"
#include "ue_wrap/actors/sleep.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/devices/atv.h"
#include "ue_wrap/world/explosion.h"
#include "ue_wrap/world/world_singleton.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace coop::dev::atv_health_drill::detail {
namespace {

namespace A    = ue_wrap::atv;
namespace DC   = coop::dev::drill_chat;
namespace ER   = coop::explosion_request;
namespace GATE = coop::atv_damage_gate;
namespace R    = ue_wrap::reflection;
namespace XD   = coop::dev::explosion_drill::detail;
using coop::atv_sync::TableView;

constexpr int   kExploded = 1;            // atv_damage_gate's row for ATV_C::exploded
constexpr int   kAtvKind = 3;             // explosion_request's per-kind slot for an ATV's request
constexpr float kBlastLiftCm = 150.f;     // the host's blast above the ATV's root
constexpr float kBlastDamage = 10.f;      // impulse 10000 * 10 / 500000 = 0.2 on either copy: no health moves
constexpr float kBlastForce = 10000.f;
constexpr float kKillPush = 3e7f;         // the game's impulse reads it as 60 times its limit: health 100 -> -20

unsigned long long U(uint64_t v) { return static_cast<unsigned long long>(v); }

std::string Narrow(const wchar_t* w) {
    std::string s;
    for (; w && *w; ++w) s += *w < 128 ? static_cast<char>(*w) : '?';
    return s;
}

// The line the host waits for from the client before it says `phase back`.
const char* AwaitLine(Arm a) { return a == Arm::Gate ? "gate-ran" : a == Arm::Owner ? "blast-seen" : "rekilled"; }

// The gamemode's `car` (the ATV the story event's trap sets), by reflection; null when it does not read.
void* GamemodeCar() {
    static int32_t off = -1;
    void* gm = ue_wrap::world_singleton::Gamemode();
    if (!gm) return nullptr;
    if (off < 0) off = R::FindPropertyOffset(R::ClassOf(gm), L"car");
    return off < 0 ? nullptr : *reinterpret_cast<void**>(static_cast<uint8_t*>(gm) + off);
}

// ---- the host ----

enum class HPh : int { Wait, Seated, Armed, Judge, Await, Spawn, Park };   // Spawn and Park: every arm but trap

struct DHost {
    Run                             run;
    SpawnStep                       sp;        // the ATV the arm acts on (trap: unused, the world's)
    coop::atv_damage_gate::Counters gate0{};   // the gate's counters at the seated edge
    ER::Counters                    req0{};    // and the request lane's
    bool readySeen = false, carChecked = false, sawAwait = false;
    bool sawTrapClear = false;   // latched while the judge polls
};
DHost g_dh;

void DamageHostEnd() { XD::Unguard(); }

void HostFail(Arm a, const std::string& what, const std::string& got, const std::string& want) {
    Fail("host", a, g_dh.run, what, got, want);
    DamageHostEnd();
}

// A bound that ran out with no event to decide on.
void HostInvalid(Arm a, const std::string& what, const std::string& got, const std::string& want) {
    Invalid("host", a, g_dh.run, what, got, want);
    DamageHostEnd();
}

void HostPhase(HPh p, const char* name) { Enter(g_dh.run, static_cast<int>(p), name); }

uint64_t Accepted() {
    return ER::GetCounters().requestsAccepted[kAtvKind] - g_dh.req0.requestsAccepted[kAtvKind];
}

// The blown marks the request lane made since the arm's start: counted, since a trap death at health 0 re-arms the mark on
// the killer's next report, within a frame, before a poll of the table can see it (f7-h-trap).
uint64_t BlownMarked() {
    return ER::GetCounters().atvBlownMarked - g_dh.req0.atvBlownMarked;
}

uint64_t RefusedSinceSeated() {
    const ER::Counters now = ER::GetCounters();
    uint64_t n = 0;
    for (int i = 0; i < static_cast<int>(ER::Refusal::Count); ++i) n += now.refused[i] - g_dh.req0.refused[i];
    return n;
}

void HostWait(Arm a) {
    DHost& h = g_dh;
    if (!h.readySeen && DC::PollDue()) h.readySeen = DC::ChatHasLive(ReadyLine(a));
    if (!h.readySeen) {
        if (Past(h.run, kFailBound)) HostInvalid(a, "the client never said it was ready", "no line", ReadyLine(a));
        return;
    }
    if (const wchar_t* bad = XD::Guard(true))
        return HostFail(a, "the host's guard did not resolve", Narrow(bad), "readable");
    HostPhase(a == Arm::Trap ? HPh::Seated : HPh::Spawn, a == Arm::Trap ? "seated" : "spawn");
}

// The spawned ATV at rest: the arm's wait for the client's seat begins.
void HostPark(Arm a) {
    const Stage st = ParkAtvStep(g_dh.sp, g_dh.run, a);
    if (st == Stage::Next) HostPhase(HPh::Seated, "seated");
    else if (st == Stage::Failed) DamageHostEnd();
}

// The client seated (occupant and syncer): the counters are snapshotted, and the arm's act is the host's one move.
void HostSeated(Arm a, const TableView* t) {
    DHost& h = g_dh;
    if (!t) return HostFail(a, "the ATV is gone before the seat", "no ATV", "the ATV the arm acts on");
    if (a == Arm::Trap && !h.carChecked) {
        h.carChecked = true;
        if (GamemodeCar() != t->actor)
            return HostFail(a, "the gamemode's car is not the world ATV", "another car or none", "the table's ATV");
    }
    if (t->occupantSlot != kSlot || t->syncerSlot != kSlot) {
        if (!Past(h.run, kHostFirstBound)) return;
        char got[64];
        std::snprintf(got, sizeof(got), "occupant %u syncer %u", static_cast<unsigned>(t->occupantSlot),
                      static_cast<unsigned>(t->syncerSlot));
        return HostInvalid(a, "the table never named the client occupant and syncer", got, "occupant 1 syncer 1");
    }
    h.gate0 = GATE::ReadCounters();
    h.req0 = ER::GetCounters();
    if (a == Arm::Owner) return HostPhase(HPh::Judge, "judge");
    if (a == Arm::Trap) {
        if (!coop::event_fire_sync::HostFire(coop::event_fire_sync::FireKind::SpecialEvent, L"atvExplode", L"None"))
            return HostFail(a, "the host's atvExplode fire was refused", "refused", "fired");
        return HostPhase(HPh::Armed, "armed");
    }
    ue_wrap::FVector at{};
    ue_wrap::FRotator rot{};
    if (!A::GetRootTransform(t->actor, at, rot))
        return HostFail(a, "the ATV's root does not read", "no transform", "root transform");
    at.Z += kBlastLiftCm;
    std::string why;
    if (!XD::SpawnNativeBlast(ue_wrap::explosion::Cls::Explosion, at, kBlastDamage, kBlastForce, &why))
        return HostFail(a, "the host's native blast did not spawn", why, "spawned");
    UE_LOGI("[ATV-HEALTH-DRILL] host arm=gate native blast at (%.0f,%.0f,%.0f)", at.X, at.Y, at.Z);
    DC::Say(PhaseLine(SpecOf(a).first));
    HostPhase(HPh::Judge, "judge");
}

// trap: the host's own table reads the trap set (its copy's `trap`, after the story event ran).
void HostArmed(Arm a, const TableView* t) {
    if (!t) return HostFail(a, "the ATV is gone before the trap", "no ATV", "the world's ATV");
    if (t->trap) {
        DC::Say(PhaseLine(SpecOf(a).first));
        return HostPhase(HPh::Judge, "judge");
    }
    if (Past(g_dh.run, kFailBound)) HostInvalid(a, "the host's table never read the trap set", "trap clear", "trap set");
}

// gate: the blast's `exploded` was cancelled once on the host's copy and ran nowhere on it.
void HostJudgeGate(Arm a) {
    const GATE::Counters c = GATE::ReadCounters();
    const uint64_t ran = c.ran[kExploded] - g_dh.gate0.ran[kExploded];
    const uint64_t cancelled = c.cancelled[kExploded] - g_dh.gate0.cancelled[kExploded];
    if (ran != 0)
        return HostFail(a, "the blast ran on the host's copy, which does not own the ATV", "ran " + Plus(ran),
                        "ran +0");
    if (cancelled > 1)
        return HostFail(a, "the blast was cancelled more than once", "cancelled " + Plus(cancelled), "cancelled +1");
    if (cancelled == 1) return HostPhase(HPh::Await, "await");
    if (Past(g_dh.run, kFailBound))
        HostInvalid(a, "the blast did not reach the ATV's copy", "cancelled +0, ran +0", "cancelled +1, ran +0");
}

// owner and trap: one request accepted, the ATV marked blown by the client and, for the trap, the host's own trap
// cleared. The mark is counted where the lane makes it (it can re-arm within a frame); the clear is latched as seen.
void HostJudge(Arm a, const TableView* t) {
    DHost& h = g_dh;
    if (!t) return HostFail(a, "the ATV is gone before the judgement", "no ATV", "the ATV the arm acts on");
    if (a == Arm::Gate) return HostJudgeGate(a);
    if (!t->trap) h.sawTrapClear = true;
    const uint64_t acc = Accepted();
    const uint64_t marked = BlownMarked();
    if (acc > 1) return HostFail(a, "the kill was accepted more than once", "accepted " + Plus(acc), "accepted +1");
    if (acc == 1 && marked >= 1 && (a != Arm::Trap || h.sawTrapClear)) {
        DC::Say(PhaseLine("blown"));
        return HostPhase(HPh::Await, "await");
    }
    // A kill the lane judged and refused is the event (its refused count); a bound with neither says INVALID.
    const uint64_t refused = RefusedSinceSeated();
    const bool judgedRefused = acc == 0 && refused > 0;
    if (!judgedRefused && !Past(h.run, kFailBound)) return;
    char got[128];
    std::snprintf(got, sizeof(got), "accepted +%llu, refused +%llu, blown marked +%llu, trap clear seen %d", U(acc),
                  U(refused), U(marked), h.sawTrapClear ? 1 : 0);
    const char* const want = a == Arm::Trap ? "accepted +1, blown by 1, trap clear" : "accepted +1, blown by 1";
    if (judgedRefused) return HostFail(a, "the client's kill was refused by the host", got, want);
    HostInvalid(a, "the client's kill was not accepted and marked in time to decide", got, want);
}

// The client's last line: the counters must still hold (trap: the re-armed second kill accepted), and the host says
// the run is over.
void HostAwait(Arm a) {
    DHost& h = g_dh;
    if (!h.sawAwait && DC::PollDue()) h.sawAwait = DC::ChatHasLive(ClientLine(AwaitLine(a)));
    if (!h.sawAwait) {
        if (Past(h.run, kFailBound)) HostInvalid(a, "the client never said its edge", "no line", ClientLine(AwaitLine(a)));
        return;
    }
    char nums[64];
    if (a == Arm::Gate) {
        const GATE::Counters c = GATE::ReadCounters();
        const uint64_t ran = c.ran[kExploded] - h.gate0.ran[kExploded];
        const uint64_t cancelled = c.cancelled[kExploded] - h.gate0.cancelled[kExploded];
        if (ran != 0 || cancelled != 1)
            return HostFail(a, "the gate's counters moved after the judgement",
                            "ran " + Plus(ran) + ", cancelled " + Plus(cancelled), "ran +0, cancelled +1");
        std::snprintf(nums, sizeof(nums), "cancelled=%llu", U(cancelled));
    } else {
        const uint64_t want = a == Arm::Owner ? 1 : 2;
        const uint64_t acc = Accepted();
        // The second kill's request may still be on its way: it is judged, accepted or refused, or the bound says INVALID.
        const uint64_t refused = RefusedSinceSeated();
        if (acc < want && refused == 0 && !Past(h.run, kFailBound)) return;
        if (acc != want) {
            const std::string got = "accepted " + Plus(acc) + ", refused " + Plus(refused);
            if (acc > want || refused > 0)
                return HostFail(a, "the requests accepted after the client's last kill", got, "accepted " + Plus(want));
            return HostInvalid(a, "the client's last kill was never judged", got, "accepted " + Plus(want));
        }
        std::snprintf(nums, sizeof(nums), "accepted=%llu", U(acc));
    }
    DC::Say(PhaseLine("back"));
    Pass("host", a, h.run, nums);
    DamageHostEnd();
}

// ---- the client ----

enum class CPh : int { Wait, Walk, Sit, Edge, Judge, Fired, Blown, Kill, Killed, Blast, Back };

struct DClient : SeatedClient {
    bool                                            sawEdge = false, sawBlown = false, sawBack = false;
    GATE::Counters                                  gate0{};   // when the ATV is parked, before any blast can run
    ER::Counters                                    req0{};    // at the kill (owner) or the press (trap)
    uint64_t                                        applied0 = 0;   // the lane's applied blasts at the kill or the press
    uint32_t                                        trapStamps0 = 0;   // trap: the host blocks with the trap bit seen when the ATV is parked
    SpawnedWait                                     spawn;     // the host's ATV (not trap): its key and its `parked`
};
DClient g_dc;

void ClientPhase(CPh p, const char* name) { Enter(g_dc.run, static_cast<int>(p), name); }

void ClientEnd() {
    WalkCancel(g_dc);
    XD::Unguard();
}

void ClientFail(Arm a, const std::string& what, const std::string& got, const std::string& want) {
    Fail("client", a, g_dc.run, what, got, want);
    ClientEnd();
}

// A bound that ran out with no event to decide on.
void ClientInvalid(Arm a, const std::string& what, const std::string& got, const std::string& want) {
    Invalid("client", a, g_dc.run, what, got, want);
    ClientEnd();
}

uint64_t Sent() { return ER::GetCounters().requestsSent[kAtvKind] - g_dc.req0.requestsSent[kAtvKind]; }

// The blasts the explosion lane has applied on this client, all classes.
uint64_t Applied() {
    const coop::explosion_sync::Counters c = coop::explosion_sync::GetCounters();
    return static_cast<uint64_t>(c.applied[0]) + c.applied[1] + c.applied[2];
}

// The ATV's forward-input events are functions on ATV_C itself with one key parameter: called with a zeroed frame.
bool PressInput(void* atv, const wchar_t* name) {
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(atv), name);
    if (!fn) return false;
    const int32_t fs = R::FunctionFrameSize(fn);
    std::vector<uint8_t> frame(fs > 0 ? static_cast<size_t>(fs) : 0, 0u);
    return R::CallFunction(atv, fn, frame.empty() ? nullptr : frame.data());
}

// What the forward input's gate reads on the copy (`battery <= 0 || empty || brokenn || underwater || zapped` returns
// before the trap is read), resolved on the first failure and read only then; -1 is unresolved.
std::string GateFlags(void* atv) {
    struct Bit { int32_t off = -1; uint8_t mask = 0; };
    static bool    resolved = false;
    static int32_t batteryOff = -1;
    static Bit     empty, broken, underwater, zapped;
    if (!resolved) {
        resolved = true;
        void* cls = R::ClassOf(atv);
        batteryOff = R::FindPropertyOffset(cls, L"battery");
        R::FindBoolProperty(cls, L"empty", empty.off, empty.mask);
        R::FindBoolProperty(cls, L"brokenn", broken.off, broken.mask);
        R::FindBoolProperty(cls, L"underwater", underwater.off, underwater.mask);
        R::FindBoolProperty(cls, L"zapped", zapped.off, zapped.mask);
    }
    const auto bit = [atv](const Bit& b) {
        return b.off < 0 || !b.mask ? -1 : (*(static_cast<uint8_t*>(atv) + b.off) & b.mask) != 0 ? 1 : 0;
    };
    const float battery = batteryOff < 0 ? NAN : *reinterpret_cast<float*>(static_cast<uint8_t*>(atv) + batteryOff);
    char buf[128];
    std::snprintf(buf, sizeof(buf), "battery %.1f empty %d brokenn %d underwater %d zapped %d", battery, bit(empty),
                  bit(broken), bit(underwater), bit(zapped));
    return buf;
}

// The kill: the copy's own addDamage with a push along its nose that the game's impulse reads as far past its limit.
// The copy's health at 0 or above and the time dilation at 1 or below are read first (the game explodes only from 0 or
// above and ignores the impulse under dilation above 1). `snapshot` takes the request counters. False once failed.
bool KillFixture(Arm a, void* atv, bool snapshot) {
    const float health = A::GetHealth(atv);
    if (!(health >= 0.f)) {
        char got[48];
        std::snprintf(got, sizeof(got), "health %.1f", health);
        ClientFail(a, "the copy's health is below 0: the game explodes only from 0 or above", got, "health >= 0");
        return false;
    }
    const float dilation = ue_wrap::sleep::GetGlobalTimeDilation();
    if (!(dilation > 0.f && dilation <= 1.f)) {
        char got[48];
        std::snprintf(got, sizeof(got), "time dilation %.2f", dilation);
        ClientFail(a, "the game ignores the impulse unless the time dilation is at most 1", got, "0 < dilation <= 1");
        return false;
    }
    ue_wrap::FVector root{};
    ue_wrap::FRotator rot{};
    if (!A::GetRootTransform(atv, root, rot)) {
        ClientFail(a, "the copy's root does not read", "no transform", "root transform");
        return false;
    }
    if (snapshot) {   // before the call: the request, the host's blast and its apply can all land within one frame
        g_dc.req0 = ER::GetCounters();
        g_dc.applied0 = Applied();
    }
    const float rad = 3.14159265f / 180.f;
    const ue_wrap::FVector push{std::cos(rot.Pitch * rad) * std::cos(rot.Yaw * rad) * kKillPush,
                                std::cos(rot.Pitch * rad) * std::sin(rot.Yaw * rad) * kKillPush,
                                std::sin(rot.Pitch * rad) * kKillPush};
    if (!ue_wrap::int_objects::InvokeAddDamage(atv, nullptr, 0.f, root, push)) {
        ClientFail(a, "the copy's addDamage did not run", "not called", "called");
        return false;
    }
    return true;
}

// The gate's counters are snapshotted when the ATV is parked: the host blasts only after the client sat.
void ClientWait(Arm a, bool due) {
    AtvAt t;
    if (a == Arm::Trap ? AtvReady(kWorldAtvKey, &t) : SpawnedAtvReady(g_dc.spawn, a, due, &t)) {
        if (const wchar_t* bad = XD::Guard(false))
            return ClientFail(a, "the client's guard did not resolve", Narrow(bad), "readable");
        g_dc.gate0 = GATE::ReadCounters();
        g_dc.trapStamps0 = coop::atv_sync::TrapStampsSeen();
        WalkStart(g_dc, a, t, kSitReachCm);
        return ClientPhase(CPh::Walk, "walk");
    }
    if (WaitBound(g_dc, a, a == Arm::Trap ? nullptr : &g_dc.spawn) == Stage::Failed) ClientEnd();
}

void ClientWalk(Arm a) {
    const Stage s = WalkStep(g_dc, a);
    if (s == Stage::Failed) return ClientEnd();
    if (s == Stage::Next) ClientPhase(CPh::Sit, "sit");
}

// The seat through the game's own verb, then the host's grant: the copy then owns the tick.
void ClientSit(Arm a, void* player, void* atv, uint8_t self) {
    const Stage s = SitStep(g_dc, a, player, atv, self);
    if (s == Stage::Failed) return ClientEnd();
    if (s == Stage::Wait) return;
    if (a == Arm::Owner) ClientPhase(CPh::Kill, "kill");
    else ClientPhase(CPh::Edge, "edge");
}

// gate: after the host's blast, the client's own copy ran `exploded` once.
void ClientJudgeGate(Arm a) {
    const GATE::Counters c = GATE::ReadCounters();
    const uint64_t ran = c.ran[kExploded] - g_dc.gate0.ran[kExploded];
    const uint64_t cancelled = c.cancelled[kExploded] - g_dc.gate0.cancelled[kExploded];
    if (ran >= 1) {
        DC::Say(ClientLine("gate-ran"));
        return ClientPhase(CPh::Back, "back");
    }
    // The gate's own count of a blast it cancelled on the owner's copy is the event; a bound with none says INVALID.
    if (cancelled >= 1)
        return ClientFail(a, "the blast was cancelled on the owner's copy", "ran +0, cancelled " + Plus(cancelled),
                          "ran +1");
    if (Past(g_dc.run, kFailBound))
        ClientInvalid(a, "the blast did not reach the owner's copy", "ran +0, cancelled +0", "ran +1");
}

// trap: on the host's `armed` edge the copy's trap and the table's read set; then the forward key is pressed and
// released on the copy, the game's own half-second delay being the game's.
void ClientPrime(Arm a, void* atv) {
    bool trap = false;
    TableView t;
    const bool copy = A::ReadTrap(atv, trap) && trap;
    const bool table = coop::atv_sync::ReadTable(kWorldAtvKey, t) && t.trap;
    if (copy && table) {
        g_dc.req0 = ER::GetCounters();
        g_dc.applied0 = Applied();
        if (!PressInput(atv, L"InpActEvt_atv_forward_K2Node_InputActionEvent_4"))
            return ClientFail(a, "the forward press did not run", "not called", "called");
        if (!PressInput(atv, L"InpActEvt_atv_forward_K2Node_InputActionEvent_5"))
            return ClientFail(a, "the forward release did not run", "not called", "called");
        return ClientPhase(CPh::Fired, "fired");
    }
    // The event: a block of the host's carrying the trap bit has been received since the ATV was parked, and the copy or the
    // table still reads clear, so this peer lost the bit (the `trapbit` red masks it after the count).
    const uint32_t stamps = coop::atv_sync::TrapStampsSeen() - g_dc.trapStamps0;
    if (stamps >= 1)
        return ClientFail(a, "a block carrying the host's trap bit was received and the owner's copy did not take it",
                          "trap blocks +" + Plus(stamps) + ", copy trap " + (copy ? "set" : "clear") + ", table trap " +
                              (table ? "set" : "clear"),
                          "both set");
    if (Past(g_dc.run, kFailBound))
        ClientInvalid(a, "the host's trap did not reach the owner's copy",
                      std::string("copy trap ") + (copy ? "set" : "clear") + ", table trap " + (table ? "set" : "clear"),
                      "both set");
}

// trap: the fire is one request and the copy's trap is cleared by the game.
void ClientFired(Arm a, void* atv) {
    bool trap = true;
    A::ReadTrap(atv, trap);
    const uint64_t sent = Sent();
    if (sent >= 1 && !trap) {
        if (sent > 1) return ClientFail(a, "the trap sent more than one request", "sent " + Plus(sent), "sent +1");
        DC::Say(ClientLine("trap-fired"));
        return ClientPhase(CPh::Blown, "blown");
    }
    if (Past(g_dc.run, kFailBound))
        ClientInvalid(a, "the trap did not fire on the owner's copy",
                      "sent " + Plus(sent) + ", trap " + (trap ? "set" : "clear") + ", " + GateFlags(atv),
                      "sent +1, trap clear");
}

// True once the explosion lane has applied a blast on this client since `applied0` (the kill or the press), the host's
// blast for it; at the bound the FAIL is printed and the run ends.
bool BlastLanded(Arm a) {
    if (Applied() > g_dc.applied0) return true;
    if (Past(g_dc.run, kFailBound))
        ClientInvalid(a, "the host's blast was never applied on the client", "applied +0", "applied +1");
    return false;
}

// trap: on the host's `blown` edge and once its blast has landed here, the copy's health is printed and the kill
// fixture runs again (the re-arm).
void ClientBlown(Arm a, void* atv) {
    if (!g_dc.sawBlown) {
        if (Past(g_dc.run, kFailBound))
            ClientInvalid(a, "the host never said it had the kill", "no line", PhaseLine("blown"));
        return;
    }
    if (!BlastLanded(a)) return;
    UE_LOGI("[ATV-HEALTH-DRILL] client arm=trap the copy's health after the host's blast is %.1f", A::GetHealth(atv));
    if (KillFixture(a, atv, false)) ClientPhase(CPh::Killed, "killed");
}

// owner: the kill is one request (trap: the second one of two since the press).
void ClientKilled(Arm a) {
    const uint64_t want = a == Arm::Owner ? 1 : 2;
    const uint64_t sent = Sent();
    if (sent > want) return ClientFail(a, "the kills sent too many requests", "sent " + Plus(sent), "sent " + Plus(want));
    if (sent == want) {
        DC::Say(ClientLine(a == Arm::Owner ? "killed" : "rekilled"));
        if (a == Arm::Trap) return ClientPhase(CPh::Back, "back");
        return ClientPhase(CPh::Blast, "blast");
    }
    if (Past(g_dc.run, kFailBound))
        ClientInvalid(a, "the kill sent no request", "sent " + Plus(sent), "sent " + Plus(want));
}

// owner: the host's blast for the kill is applied here.
void ClientBlast(Arm a) {
    if (!BlastLanded(a)) return;
    DC::Say(ClientLine("blast-seen"));
    ClientPhase(CPh::Back, "back");
}

void ClientBack(Arm a, void* atv) {
    if (!g_dc.sawBack) {
        if (Past(g_dc.run, kFailBound))
            ClientInvalid(a, "the host never said it had finished", "no line", PhaseLine("back"));
        return;
    }
    // Guards the lane: one kill is one request, and the host's blast on this copy sends no second one. The game itself
    // cannot explode again from health -1 (the DONE line's health shows it), so this proves no game behaviour.
    if (a == Arm::Owner && Sent() != 1)
        return ClientFail(a, "the lane sent a second request for the one ATV", "sent " + Plus(Sent()), "sent +1");
    char nums[64];
    std::snprintf(nums, sizeof(nums), "walk_ms=%lld health=%.1f", Ms(g_dc.walkAt, g_dc.arrivedAt),
                  atv ? A::GetHealth(atv) : 0.f);
    Pass("client", a, g_dc.run, nums);
    ClientEnd();
}

}  // namespace

void DamageHostTick(coop::net::Session* s, Arm a) {
    DHost& h = g_dh;
    if (h.run.done) return;
    if (h.run.at == Clock::time_point{}) {   // the bounds run from the edge of slot 1 being in the world
        if (!s->IsSlotWorldReady(kSlot)) return;
        HostPhase(HPh::Wait, "wait");
    }
    if (static_cast<HPh>(h.run.phase) == HPh::Wait) return HostWait(a);
    if (static_cast<HPh>(h.run.phase) == HPh::Await) return HostAwait(a);
    TableView t;
    static const std::wstring world = kWorldAtvKey;
    const std::wstring& key = a == Arm::Trap ? world : h.sp.key;
    const bool read = !key.empty() && coop::atv_sync::ReadTable(key, t);
    const TableView* view = read ? &t : nullptr;
    switch (static_cast<HPh>(h.run.phase)) {
    case HPh::Spawn: {
        const Stage st = SpawnAtvStep(h.sp, h.run, a);
        if (st == Stage::Next) HostPhase(HPh::Park, "park");
        else if (st == Stage::Failed) DamageHostEnd();   // the FAIL line is printed: the guard is let go
        return;
    }
    case HPh::Park:   return HostPark(a);
    case HPh::Seated: return HostSeated(a, view);
    case HPh::Armed:  return HostArmed(a, view);
    case HPh::Judge:  return HostJudge(a, view);
    case HPh::Wait:
    case HPh::Await:  return;
    }
}

void DamageClientTick(coop::net::Session*, Arm a) {
    DClient& c = g_dc;
    void* player = nullptr;
    uint8_t self = 0;
    if (!SeatedActs(c, &player, &self)) return;
    if (c.run.at == Clock::time_point{}) ClientPhase(CPh::Wait, "wait");   // the bounds run from the world-ready edge
    // The host's edge lines, latched on sight: a live chat row retires after seconds.
    const bool due = DC::PollDue();
    if (due) {
        if (!c.sawEdge) c.sawEdge = DC::ChatHasLive(PhaseLine(SpecOf(a).first));
        if (!c.sawBlown) c.sawBlown = DC::ChatHasLive(PhaseLine("blown"));
        if (!c.sawBack) c.sawBack = DC::ChatHasLive(PhaseLine("back"));
    }
    void* atv = c.atv.Get();
    const CPh phase = static_cast<CPh>(c.run.phase);
    if (phase == CPh::Wait) return ClientWait(a, due);
    if (phase == CPh::Walk) return ClientWalk(a);
    if (!atv) return ClientFail(a, "the ATV is gone before the verdict", "no ATV", "the ATV the arm acts on");
    switch (phase) {
    case CPh::Sit:    return ClientSit(a, player, atv, self);
    case CPh::Edge:
        if (c.sawEdge) return ClientPhase(CPh::Judge, "judge");
        if (Past(c.run, kFailBound))
            ClientInvalid(a, "the host never said its edge", "no line", PhaseLine(SpecOf(a).first));
        return;
    case CPh::Judge:  return a == Arm::Gate ? ClientJudgeGate(a) : ClientPrime(a, atv);
    case CPh::Fired:  return ClientFired(a, atv);
    case CPh::Blown:  return ClientBlown(a, atv);
    case CPh::Kill:   if (KillFixture(a, atv, true)) ClientPhase(CPh::Killed, "killed"); return;
    case CPh::Killed: return ClientKilled(a);
    case CPh::Blast:  return ClientBlast(a);
    case CPh::Back:   return ClientBack(a, atv);
    case CPh::Wait:
    case CPh::Walk:   return;
    }
}

void DamageReset() {
    DamageHostEnd();
    g_dh = DHost{};
    ClientEnd();
    g_dc = DClient{};
}

}  // namespace coop::dev::atv_health_drill::detail
