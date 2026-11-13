// coop/dev/atv_eject_drill.cpp -- see coop/dev/atv_eject_drill.h. A REAL eject through the game's own path, getTire(index)
// -> damageWheel(index, 200, component): durability to zero, ejectWheel, the wheel prop, tires[i] = false and updTires,
// the native transaction rather than a hand-poked array. Each peer acts on an edge the other one said in the lobby chat,
// never on a clock, and ends on its own DONE or FAIL line; the arm's verdict is the judging peer's DONE line (the
// client's in arm host, the host's in arm client), the other peer's line says only that its own act ran.
//   arm host:   the host fires on its idle, host-synced ATV; the client's live copy must lose the wheel, updTires +1.
//   arm client: the client sits, becomes the syncer and fires on its copy; the host must keep the wheel and count the
//               client's presence as skipped. That is the known divergence of atv_condition_sync (the wheel prop a client
//               births does not travel), asserted contained.
// The bounds below only say INVALID and are never waits: every phase leaves on its edge. The verdict of arm client's judge
// is decided on the host's own receipt of the client's eject (kApplyBlocks); a bound there only says INVALID.

#include "coop/dev/atv_eject_drill.h"

#include "atv_seat.h"  // SitTry (co-located private header)

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/director/director.h"
#include "coop/dev/drill_chat.h"
#include "coop/interactables/atv_condition_sync.h"
#include "coop/interactables/atv_sync.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/join_membership_sweep.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"
#include "coop/session/rig_verdict.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/devices/atv.h"
#include "ue_wrap/devices/atv_condition.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

namespace coop::dev::atv_eject_drill {
namespace {

namespace AC = ue_wrap::atv_condition;
namespace CS = coop::atv_condition_sync;
namespace DC = coop::dev::drill_chat;
namespace R  = ue_wrap::reflection;
using coop::atv_sync::TableView;
using Clock = std::chrono::steady_clock;

enum class Arm : uint8_t { Off, Host, Client };

constexpr const wchar_t* kKey = L"ATV";   // the world's own ATV, under its game key
constexpr uint8_t        kSlot = 1;       // the one slot the drill acts on
constexpr uint8_t        kNoSeat = 0xFF;
constexpr int32_t        kWheel = 2;      // backWheel_R in the ATV's getTire switch
constexpr uint8_t        kWheelBit = 1u << kWheel;
constexpr float          kDamage = 200.f; // above any durability: FMax(x,0) = 0 -> ejectWheel
constexpr auto           kFailBound = std::chrono::seconds(180);   // a bound: INVALID when it passes, never a wait
constexpr auto           kSeatBound = std::chrono::seconds(360);   // the client's walk and its sit, never a wait
constexpr float          kSitReachCm = 400.f;   // the director's walk ends this close to the ATV's root
constexpr int            kWalkDeadlineS = 300;
// Counted not clocked: the seated client syncer streams a condition block with every state block, one per 50 ms
// (kDriveSendMs), and the client says its fired line once its own mask reads clear, so its eject is in the first block
// sent within a frame of that line. 20 blocks applied on the host after the line (1 s of stream) have carried it, so
// the host's skip count is known there: with no skip, the host's apply did not take the eject, a FAIL.
constexpr uint64_t       kApplyBlocks = 20;

constexpr const char* kFiredLine = "ATVEJ client fired";

Arm ArmOf() {
    static const Arm a = [] {
        const std::string v = coop::config::ResolveEnum(coop::config_registry::rows::atv_eject_drill);
        if (v == "host") return Arm::Host;
        if (v == "client") return Arm::Client;
        return Arm::Off;
    }();
    return a;
}

const char* NameOf(Arm a) { return a == Arm::Host ? "host" : a == Arm::Client ? "client" : "off"; }

// Lines no save can hold, as a prop class or a place could.
std::string ReadyLine(Arm a) { return std::string("ATVEJ client ready ") + NameOf(a); }

// One role's phase: its name for the lines, when it began and when its bounded stretch did. `done` once it printed its
// verdict.
struct Run {
    int               phase = 0;
    const char*       name = "wait";
    Clock::time_point at{}, from{};
    bool              done = false;
};

void Enter(Run& r, const char* role, Arm a, int phase, const char* name) {
    r.phase = phase;
    r.name = name;
    r.at = Clock::now();
    UE_LOGI("[ATV-EJECT-DRILL] %s arm=%s phase=%s", role, NameOf(a), name);
}

bool Past(Clock::time_point since, std::chrono::seconds bound) { return Clock::now() - since > bound; }

long long Ms(Clock::time_point from, Clock::time_point to) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(to - from).count();
}

void Fail(const char* role, Arm a, Run& r, const std::string& what, const std::string& got, const std::string& want) {
    UE_LOGW("[ATV-EJECT-DRILL] FAIL %s arm=%s phase=%s: %s, got %s, want %s", role, NameOf(a), r.name, what.c_str(),
            got.c_str(), want.c_str());
    r.done = true;
}

// A run that could not decide: no event came and a bound ran out. Neither green nor red; the rig ends on the line.
void Invalid(const char* role, Arm a, Run& r, const std::string& what, const std::string& got, const std::string& want) {
    UE_LOGW("[ATV-EJECT-DRILL] INVALID %s arm=%s phase=%s: %s, got %s, want %s", role, NameOf(a), r.name, what.c_str(),
            got.c_str(), want.c_str());
    coop::rig_verdict::Say("atv-eject-drill", coop::rig_verdict::Outcome::Invalid, "%s arm=%s phase=%s: %s", role,
                           NameOf(a), r.name, what.c_str());
    r.done = true;
}

void Pass(const char* role, Arm a, Run& r, const std::string& numbers) {
    UE_LOGI("[ATV-EJECT-DRILL] %s DONE PASS arm=%s %s", role, NameOf(a), numbers.c_str());
    r.done = true;
}

std::string Plus(uint64_t n) { return "+" + std::to_string(n); }

std::string Hex(uint8_t m) {
    char b[8];
    std::snprintf(b, sizeof(b), "%02X", static_cast<unsigned>(m));
    return b;
}

bool ReadMask(void* actor, uint8_t* mask) {
    AC::Snapshot snap;
    if (!AC::Read(actor, snap)) return false;
    *mask = snap.mask;
    return true;
}

// The client acts once its own streams run, as a player in a settled world does.
bool StreamsRun() {
    return coop::net_pump::HasAnnouncedWorldReady() &&
           coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle &&
           coop::net_pump::IsWorldSettled() && coop::join_membership_sweep::HasLoadTailQuiesced();
}

// The game's own eject on `actor`: getTire(kWheel), then damageWheel(kWheel, kDamage, that component). False with the
// reason when a step does not run.
bool Fire(void* actor, std::string* why) {
    void* cls = R::FindClass(L"ATV_C");
    void* fnGetTire = cls ? R::FindFunction(cls, L"getTire") : nullptr;
    void* fnDamage = cls ? R::FindFunction(cls, L"damageWheel") : nullptr;
    if (!fnGetTire || !fnDamage) {
        *why = "getTire or damageWheel did not resolve";
        return false;
    }
    ue_wrap::ParamFrame tireFrame(fnGetTire);
    tireFrame.Set(L"index", kWheel);
    if (!ue_wrap::Call(actor, tireFrame)) {
        *why = "the getTire call failed";
        return false;
    }
    void* component = tireFrame.Get<void*>(L"ReturnValue");
    if (!component) {
        *why = "getTire returned no wheel";
        return false;
    }
    ue_wrap::ParamFrame dmgFrame(fnDamage);
    dmgFrame.Set(L"index", kWheel);
    dmgFrame.Set(L"damage", kDamage);
    dmgFrame.Set(L"component", component);
    if (!ue_wrap::Call(actor, dmgFrame)) {
        *why = "the damageWheel call failed";
        return false;
    }
    return true;
}

// ---- the host ----

enum class HPh : int { Wait, Own, Fire, Judge, Seated };

struct HostState {
    Run                 run;
    bool                readySeen = false, firedSeen = false;
    CS::Counters        base{};    // the lane's counters at the client's ready line
    uint8_t             before = 0;
    Clock::time_point   firedAt{};
    uint64_t            appliedAtFired = 0;   // the lane's applied-block count when the client's fired line was seen
} g_h;

void HostPhase(Arm a, HPh p, const char* name) { Enter(g_h.run, "host", a, static_cast<int>(p), name); }

void HostFail(Arm a, const std::string& what, const std::string& got, const std::string& want) {
    Fail("host", a, g_h.run, what, got, want);
}

// A bound that ran out with no event to decide on.
void HostInvalid(Arm a, const std::string& what, const std::string& got, const std::string& want) {
    Invalid("host", a, g_h.run, what, got, want);
}

// The client's ready line, then the arm's start: arm host waits for the idle ATV, arm client takes the counters.
void HostWait(Arm a) {
    HostState& h = g_h;
    if (!h.readySeen && DC::PollDue()) h.readySeen = DC::ChatHasLive(ReadyLine(a));
    if (!h.readySeen) {
        if (Past(h.run.at, kFailBound)) HostInvalid(a, "the client never said it was ready", "no line", ReadyLine(a));
        return;
    }
    if (a == Arm::Host) return HostPhase(a, HPh::Own, "own");
    TableView t;
    if (!coop::atv_sync::ReadTable(kKey, t)) return HostFail(a, "the ATV is gone", "no ATV", "the world's ATV");
    if (!ReadMask(t.actor, &h.before) || !(h.before & kWheelBit))
        return HostFail(a, "the ATV has no rear wheel to knock off", "mask " + Hex(h.before), "bit 2 set");
    h.base = CS::ReadCounters();
    h.run.from = Clock::now();
    HostPhase(a, HPh::Seated, "seated");
}

// arm host: the host owns the idle ATV (it syncs it and nobody sits in it).
void HostOwn(Arm a, const TableView& t) {
    if (t.syncerSlot == 0 && t.occupantSlot == kNoSeat) return HostPhase(a, HPh::Fire, "fire");
    if (!Past(g_h.run.at, kFailBound)) return;
    char got[64];
    std::snprintf(got, sizeof(got), "occupant %u syncer %u", static_cast<unsigned>(t.occupantSlot),
                  static_cast<unsigned>(t.syncerSlot));
    HostInvalid(a, "the host never owned the idle ATV", got, "occupant 255 syncer 0");
}

void HostFire(Arm a, const TableView& t) {
    HostState& h = g_h;
    if (!ReadMask(t.actor, &h.before) || !(h.before & kWheelBit))
        return HostFail(a, "the ATV has no rear wheel to knock off", "mask " + Hex(h.before), "bit 2 set");
    std::string why;
    if (!Fire(t.actor, &why)) return HostFail(a, "the host's eject did not run", why, "damageWheel called");
    HostPhase(a, HPh::Judge, "judge");
}

// arm host: the host's own wheel is gone, which the game's eject does in the same call.
void HostJudgeHost(Arm a, const TableView& t) {
    uint8_t now = 0;
    if (ReadMask(t.actor, &now) && !(now & kWheelBit)) {
        char nums[64];
        std::snprintf(nums, sizeof(nums), "mask %02X -> %02X in %lld ms", static_cast<unsigned>(g_h.before),
                      static_cast<unsigned>(now), Ms(g_h.run.at, Clock::now()));
        Pass("host", a, g_h.run, nums);
        return;
    }
    if (Past(g_h.run.at, kFailBound))
        HostInvalid(a, "the host's damageWheel did not eject the wheel", "mask " + Hex(now), "bit 2 clear");
}

// arm client: the client sits and syncs the ATV.
void HostSeated(Arm a, const TableView& t) {
    if (t.occupantSlot == kSlot && t.syncerSlot == kSlot) return HostPhase(a, HPh::Judge, "judge");
    if (!Past(g_h.run.from, kSeatBound)) return;
    char got[64];
    std::snprintf(got, sizeof(got), "occupant %u syncer %u", static_cast<unsigned>(t.occupantSlot),
                  static_cast<unsigned>(t.syncerSlot));
    HostInvalid(a, "the table never named the client occupant and syncer", got, "occupant 1 syncer 1");
}

// arm client: the host keeps its wheel and calls no updTires, and once the client has fired, its presence is counted
// skipped. A wheel gone or an updTires call fails at once: those are the host consuming a client's claim.
void HostJudgeClient(Arm a, const TableView& t) {
    HostState& h = g_h;
    uint8_t now = 0;
    if (ReadMask(t.actor, &now) && !(now & kWheelBit))
        return HostFail(a, "the host consumed a client syncer's wheel presence", "mask " + Hex(now), "bit 2 kept");
    const CS::Counters c = CS::ReadCounters();
    const uint64_t tires = c.updTiresCalled - h.base.updTiresCalled;
    if (tires != 0)
        return HostFail(a, "the host called updTires on a client syncer's block", "updTires " + Plus(tires),
                        "updTires +0");
    if (!h.firedSeen && DC::PollDue()) {
        h.firedSeen = DC::ChatHasLive(kFiredLine);
        if (h.firedSeen) {
            h.firedAt = Clock::now();
            h.appliedAtFired = c.applied;
        }
    }
    if (!h.firedSeen) {
        if (Past(h.run.from, kSeatBound))
            Invalid("host", a, h.run, "the client never said it fired", "no line", kFiredLine);
        return;
    }
    const uint64_t skipped = c.presenceSkippedDiffering - h.base.presenceSkippedDiffering;
    if (skipped >= 1) {
        char nums[64];
        std::snprintf(nums, sizeof(nums), "skipped %s, mask %02X kept", Plus(skipped).c_str(),
                      static_cast<unsigned>(now));
        Pass("host", a, h.run, nums);
        return;
    }
    const uint64_t blocks = c.applied - h.appliedAtFired;
    if (blocks >= kApplyBlocks)
        return HostFail(a, "the client's eject reached the host's apply and was not skipped",
                        "skipped +0 after " + std::to_string(blocks) + " applied blocks", "skipped >= +1");
    if (Past(h.firedAt, kFailBound))
        Invalid("host", a, h.run, "no block of the client's stream reached the host's apply after its fired line",
                "applied " + Plus(blocks), "applied >= +" + std::to_string(kApplyBlocks));
}

void HostTick(coop::net::Session* s, Arm a) {
    HostState& h = g_h;
    if (h.run.done) return;
    if (h.run.at == Clock::time_point{}) {   // the bounds run from the edge of slot 1 being in the world
        if (!s->IsSlotWorldReady(kSlot)) return;
        HostPhase(a, HPh::Wait, "wait");
    }
    const HPh p = static_cast<HPh>(h.run.phase);
    if (p == HPh::Wait) return HostWait(a);
    TableView t;
    if (!coop::atv_sync::ReadTable(kKey, t)) {   // a miss is a wait: the index may be rebuilding
        if (Past(h.run.at, kFailBound)) HostInvalid(a, "the ATV left the table", "no ATV", "the world's ATV");
        return;
    }
    switch (p) {
    case HPh::Own:    return HostOwn(a, t);
    case HPh::Fire:   return HostFire(a, t);
    case HPh::Judge:  return a == Arm::Host ? HostJudgeHost(a, t) : HostJudgeClient(a, t);
    case HPh::Seated: return HostSeated(a, t);
    case HPh::Wait:   return;
    }
}

// ---- the client ----

enum class CPh : int { Ready, Walk, Sit, Owned, Judge };

struct ClientState {
    Run                                             run;
    bool                                            idleSaid = false;
    CS::Counters                                    base{};    // the lane's counters at the ready line
    uint8_t                                         before = 0;
    Clock::time_point                               readyAt{};
    std::shared_ptr<coop::director::BackgroundWalk> walk;
    atv_seat::SitTry                                sit;
} g_c;

void ClientPhase(Arm a, CPh p, const char* name) { Enter(g_c.run, "client", a, static_cast<int>(p), name); }

void WalkCancel() {
    if (g_c.walk && g_c.walk->state.load() == 0) coop::director::CancelBackgroundWalk(*g_c.walk);
    g_c.walk.reset();
}

void ClientFail(Arm a, const std::string& what, const std::string& got, const std::string& want) {
    Fail("client", a, g_c.run, what, got, want);
    WalkCancel();
}

// A bound that ran out with no event to decide on.
void ClientInvalid(Arm a, const std::string& what, const std::string& got, const std::string& want) {
    Invalid("client", a, g_c.run, what, got, want);
    WalkCancel();
}

// The client's streams run and the table holds an ATV whose copy still has its rear wheel: the base counters are taken,
// the ready line said, and the arm starts (arm host judges, arm client walks to the ATV).
void ClientReady(Arm a) {
    ClientState& c = g_c;
    TableView t;
    if (StreamsRun() && coop::atv_sync::ReadTable(kKey, t)) {
        if (!ReadMask(t.actor, &c.before) || !(c.before & kWheelBit))
            return ClientFail(a, "the ATV has no rear wheel to knock off", "mask " + Hex(c.before), "bit 2 set");
        ue_wrap::FVector at{};
        ue_wrap::FRotator rot{};
        if (a == Arm::Client && !ue_wrap::atv::GetRootTransform(t.actor, at, rot))
            return ClientFail(a, "the ATV's root does not read", "no transform", "root transform");
        c.base = CS::ReadCounters();
        c.readyAt = Clock::now();
        c.run.from = c.readyAt;
        DC::Say(ReadyLine(a));
        if (a == Arm::Host) return ClientPhase(a, CPh::Judge, "judge");
        c.walk = coop::director::StartBackgroundWalk(at, kSitReachCm, kWalkDeadlineS);
        UE_LOGI("[ATV-EJECT-DRILL] client arm=client the ATV is at (%.0f,%.0f,%.0f), walking to it", at.X, at.Y, at.Z);
        return ClientPhase(a, CPh::Walk, "walk");
    }
    if (Past(c.run.at, kFailBound))
        ClientInvalid(a, "the client's streams or the ATV's table never held", "not ready", "streams running, ATV indexed");
}

// arm host: the client's live copy loses the wheel once, through the verb the lane runs on a mirror: with the bit clear,
// updTires +1 passes and more than one call is the 20 Hz refire a mis-gated presence rule causes.
void ClientJudgeHost(Arm a, const TableView& t) {
    ClientState& c = g_c;
    const CS::Counters now = CS::ReadCounters();
    const uint64_t skipped = now.presenceSkippedDiffering - c.base.presenceSkippedDiffering;
    if (skipped != 0)
        return ClientFail(a, "the host's wheel presence was skipped on the client", "skipped " + Plus(skipped),
                          "skipped +0");
    const uint64_t tires = now.updTiresCalled - c.base.updTiresCalled;
    uint8_t mask = 0;
    const bool read = ReadMask(t.actor, &mask);
    if (read && !(mask & kWheelBit) && tires > 1)
        return ClientFail(a, "the client's copy ran updTires more than once for one eject", "updTires " + Plus(tires),
                          "updTires +1");
    if (read && !(mask & kWheelBit) && tires == 1) {
        char nums[64];
        std::snprintf(nums, sizeof(nums), "updTires %s, mask %02X, %lld ms after ready", Plus(tires).c_str(),
                      static_cast<unsigned>(mask), Ms(c.readyAt, Clock::now()));
        Pass("client", a, c.run, nums);
        return;
    }
    if (Past(c.readyAt, kFailBound))
        ClientInvalid(a, "the host's eject never reached the client's copy",
                      "updTires " + Plus(tires) + ", mask " + Hex(mask), "updTires +1, bit 2 clear");
}

void ClientWalk(Arm a) {
    const int state = g_c.walk->state.load();
    if (state == 2) return ClientFail(a, "the director's walk to the ATV did not arrive", "walk failed", "arrived");
    if (state == 1) ClientPhase(a, CPh::Sit, "sit");
}

// The seat through the game's own verb, then the host's grant.
void ClientSit(Arm a, const TableView& t, void* player, uint8_t self) {
    ClientState& c = g_c;
    if (!atv_seat::TrySit(player, t.actor, c.sit)) {
        if (Past(c.run.from, kSeatBound)) ClientInvalid(a, "the sit never finished", "no grant", "seated");
        return;
    }
    if (coop::atv_sync::ActorSeatedBy(self) != t.actor)
        return ClientFail(a, "the sit did not take (see the [ATVP] sit lines)", "not seated", "seated");
    ClientPhase(a, CPh::Owned, "owned");
}

// The host's table names this client the syncer: the copy owns the tick, and the eject is fired on it.
void ClientOwned(Arm a, const TableView& t, uint8_t self) {
    ClientState& c = g_c;
    if (t.syncerSlot != self) {
        if (!Past(c.run.from, kSeatBound)) return;
        char got[32];
        std::snprintf(got, sizeof(got), "syncer %u", static_cast<unsigned>(t.syncerSlot));
        return ClientInvalid(a, "the table never named this client the syncer", got, "syncer = this client");
    }
    if (!ReadMask(t.actor, &c.before) || !(c.before & kWheelBit))
        return ClientFail(a, "the ATV has no rear wheel to knock off", "mask " + Hex(c.before), "bit 2 set");
    std::string why;
    if (!Fire(t.actor, &why)) return ClientFail(a, "the client's eject did not run", why, "damageWheel called");
    ClientPhase(a, CPh::Judge, "fired");
}

// arm client: the client's own wheel is gone; the line goes out, and the client stays seated until the host's verdict.
void ClientJudgeClient(Arm a, const TableView& t) {
    ClientState& c = g_c;
    uint8_t now = 0;
    if (ReadMask(t.actor, &now) && !(now & kWheelBit)) {
        DC::Say(kFiredLine);
        char nums[64];
        std::snprintf(nums, sizeof(nums), "mask %02X -> %02X", static_cast<unsigned>(c.before),
                      static_cast<unsigned>(now));
        Pass("client", a, c.run, nums);
        return;
    }
    if (Past(c.run.at, kFailBound))
        ClientInvalid(a, "the client's damageWheel did not eject the wheel", "mask " + Hex(now), "bit 2 clear");
}

void ClientTick(Arm a) {
    ClientState& c = g_c;
    if (c.run.done) return;
    const uint8_t self = coop::players::Registry::Get().LocalPeerId();
    void* player = coop::players::Registry::Get().Local();
    if (!coop::net_pump::HasAnnouncedWorldReady() || !player || self == 0 || self >= coop::players::kMaxPeers) return;
    if (self != kSlot) {
        if (!c.idleSaid) {
            c.idleSaid = true;
            UE_LOGI("[ATV-EJECT-DRILL] client idles: this client is slot %u, the drill acts on slot %u",
                    static_cast<unsigned>(self), static_cast<unsigned>(kSlot));
        }
        return;
    }
    if (c.run.at == Clock::time_point{}) ClientPhase(a, CPh::Ready, "ready");   // the bounds run from the world-ready edge
    const CPh p = static_cast<CPh>(c.run.phase);
    if (p == CPh::Ready) return ClientReady(a);
    TableView t;
    if (!coop::atv_sync::ReadTable(kKey, t)) {   // a miss is a wait: the index may be rebuilding
        if (Past(c.run.at, kFailBound)) ClientInvalid(a, "the ATV left the table", "no ATV", "the world's ATV");
        return;
    }
    switch (p) {
    case CPh::Walk:   return ClientWalk(a);
    case CPh::Sit:    return ClientSit(a, t, player, self);
    case CPh::Owned:  return ClientOwned(a, t, self);
    case CPh::Judge:  return a == Arm::Host ? ClientJudgeHost(a, t) : ClientJudgeClient(a, t);
    case CPh::Ready:  return;
    }
}

}  // namespace

void Tick(coop::net::Session* s) {
    const Arm a = ArmOf();
    if (a == Arm::Off || !s) return;
    if (s->role() == coop::net::Role::Host) HostTick(s, a);
    else if (s->connected()) ClientTick(a);
}

void OnDisconnect() {
    WalkCancel();
    g_h = HostState{};
    g_c = ClientState{};
}

}  // namespace coop::dev::atv_eject_drill
