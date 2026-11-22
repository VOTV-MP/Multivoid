// coop/dev/bucket_drill.cpp -- the bucket drill's log, chat, verdict, reads, the player's own verbs and the tick; see
// coop/dev/bucket_drill.h. The host's stage is bucket_drill_stage.cpp (its places _spots), the client's walk to a staged prop _walk; the arms are
// bucket_drill_verbs.cpp (soap, dip, transfer),
// _concrete (mix, trowel), _tick (pin, visuals, mop), _drop, _join and _alc (alc, brew); the tick watch is _watch.
//
// Every verdict is decided by an event: a line a peer says after the effect it waits on, a counter the lane keeps, or a row of the
// host's applied on the client's copy. A FAIL is said where it is decided and nothing after it runs; a bound only says INVALID.

#include "coop/dev/bucket_drill.h"
#include "bucket_drill_detail.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/drill_chat.h"
#include "coop/dev/drill_ground.h"
#include "coop/dev/director/aim_fan.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"
#include "coop/player/roster.h"
#include "coop/props/bucket_sync.h"
#include "coop/props/prop_act_host.h"
#include "coop/props/prop_key_index.h"
#include "coop/props/prop_act_request.h"
#include "coop/props/prop_snapshot.h"
#include "coop/save/join_window_baseline.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"
#include "coop/session/rig_verdict.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/engine/engine_pawn.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <unordered_map>
#include <vector>

namespace coop::dev::bucket_drill::detail {

namespace R   = ue_wrap::reflection;
namespace E   = ue_wrap::engine;
namespace OI  = ue_wrap::object_index;
namespace DC  = coop::dev::drill_chat;
namespace DG  = coop::dev::drill_ground;
namespace EL  = coop::element;
namespace HB  = coop::hand_item;
namespace PAR = coop::prop_act_request;
namespace RV  = coop::rig_verdict;

namespace {

bool g_ended = false, g_armed = false;
int g_stepTicks = 0;

char Who() { return coop::roster::LocalIsHost() ? 'H' : 'C'; }

std::string Format(const char* fmt, va_list ap) {
    char buf[700];
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    return buf;
}

const char* g_verdictName = nullptr;   // set by a drill that runs the stage under its own name

std::string Tag() { return std::string("bucket_") + (g_verdictName ? g_verdictName : ArmName(ArmOf())); }

}  // namespace

Arm ArmOf() {
    static const Arm a = [] {
        const std::string v = coop::config::ResolveEnum(::coop::config_registry::rows::bucket_drill);
        if (v == "soap") return Arm::Soap;
        if (v == "dip") return Arm::Dip;
        if (v == "mix") return Arm::Mix;
        if (v == "trowel") return Arm::Trowel;
        if (v == "transfer") return Arm::Transfer;
        if (v == "pin") return Arm::Pin;
        if (v == "visuals") return Arm::Visuals;
        if (v == "ctick") return Arm::Ctick;
        if (v == "drop") return Arm::Drop;
        if (v == "join") return Arm::Join;
        if (v == "mop") return Arm::Mop;
        if (v == "alc") return Arm::Alc;
        if (v == "brew") return Arm::Brew;
        return Arm::Off;
    }();
    return a;
}

const char* ArmName(Arm a) {
    switch (a) {
    case Arm::Soap: return "soap";
    case Arm::Dip: return "dip";
    case Arm::Mix: return "mix";
    case Arm::Trowel: return "trowel";
    case Arm::Transfer: return "transfer";
    case Arm::Pin: return "pin";
    case Arm::Visuals: return "visuals";
    case Arm::Ctick: return "ctick";
    case Arm::Drop: return "drop";
    case Arm::Join: return "join";
    case Arm::Mop: return "mop";
    case Arm::Alc: return "alc";
    case Arm::Brew: return "brew";
    default: return "off";
    }
}

// ---- the log, the chat, the verdict and the bound -------------------------------------------------------------------------

void Fact(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const std::string text = Format(fmt, ap);
    va_end(ap);
    UE_LOGI("[BUCKET] [%c] %s", Who(), text.c_str());
}

void Say(const std::string& line) { DC::Say(std::string(kPrefix) + line); }
bool Seen(const std::string& line) { return DC::SeenHas(std::string(kPrefix) + line); }
bool Rest(const std::string& prefix, std::string* rest) { return DC::SeenRestOf(std::string(kPrefix) + prefix, rest); }

bool ParseInts(const std::string& text, int n, int* out) {
    const char* p = text.c_str();
    for (int i = 0; i < n; ++i) {
        char* end = nullptr;
        out[i] = static_cast<int>(std::strtol(p, &end, 10));
        if (end == p) return false;
        p = end;
    }
    return true;
}

std::string Ints(std::initializer_list<int> v) {
    std::string out;
    for (const int x : v) out += (out.empty() ? "" : " ") + std::to_string(x);
    return out;
}

bool Late(const char* why, int boundTicks) {
    if (++g_stepTicks <= boundTicks) return false;
    Invalid(why);
    return true;
}

// The peer's pin cost, said once as the drill ends here, before the verdict line: the rig ends a run by killing the peers, so the
// session's teardown line never prints.
namespace {
void SayPinCost() {
    if (!g_ended) coop::bucket_sync::dev::PinCostLine();
}
}  // namespace

void Invalid(const char* why) {
    SayPinCost();
    if (coop::roster::LocalIsHost()) RV::Say(Tag().c_str(), RV::Outcome::Invalid, "%s", why);
    else UE_LOGW("[BUCKET] [C] INVALID %s", why);
    g_ended = true;
}

void Fail(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const std::string text = Format(fmt, ap);
    va_end(ap);
    SayPinCost();
    RV::Say(Tag().c_str(), RV::Outcome::Fail, "%s", text.c_str());
    g_ended = true;
}

void Pass(const char* why) {
    SayPinCost();
    RV::Say(Tag().c_str(), RV::Outcome::Pass, "%s", why);
    g_ended = true;
}

void Finish() {
    SayPinCost();
    g_ended = true;
}
bool Ended() { return g_ended; }
void Go() { g_stepTicks = 0; }
void NameVerdict(const char* arm) { g_verdictName = arm; }

// ---- reads ---------------------------------------------------------------------------------------------------------------

int Centi(float v) { return static_cast<int>(std::lround(v * 100.f)); }

int EidOf(void* actor) {
    const EL::ElementId eid = EL::Registry::Get().EidForActor(actor);
    return eid == EL::kInvalidId ? 0 : static_cast<int>(eid);
}

void* LiveProp(int eid) {
    return eid > 0 ? EL::LiveActorOfType(static_cast<EL::ElementId>(eid), EL::ElementType::Prop) : nullptr;
}

std::wstring KeyOf(void* actor) { return actor ? ue_wrap::prop::GetInteractableKeyString(actor) : std::wstring(); }

void* ByKey(const std::wstring& key) {
    void* const a = key.empty() ? nullptr : coop::prop_key_index::Find(key);
    return a && R::IsLive(a) ? a : nullptr;
}

void* Local() { return coop::players::Registry::Get().Local(); }

void* Puppet() {
    coop::RemotePlayer& rp = coop::puppet_drive::Puppet(kSlot);
    return rp.valid() ? rp.actor() : nullptr;
}

bool Quiet(void* actor) {
    if (!actor) return false;
    const ue_wrap::prop::VelocityState v = ue_wrap::prop::GetPhysicsVelocity(actor, /*linearOnly=*/true);
    if (v.ok) return DG::Quiet(actor);
    // A copy with no readable body: its place has not moved a centimetre since the last read of it.
    static std::unordered_map<void*, ue_wrap::FVector> s_last;
    ue_wrap::FVector at{};
    if (!E::TryGetActorLocation(actor, at)) return false;
    const auto it = s_last.find(actor);
    const bool still = it != s_last.end() && std::fabs(it->second.X - at.X) < 1.f && std::fabs(it->second.Y - at.Y) < 1.f &&
                       std::fabs(it->second.Z - at.Z) < 1.f;
    if (s_last.size() > 256) s_last.clear();
    s_last[actor] = at;
    return still;
}

bool Calm(void* actor, int& quiet) {
    quiet = Quiet(actor) ? quiet + 1 : 0;
    return quiet >= kQuietTicks;
}

bool HandEmpty() { return HB::LocalHandActor() == nullptr; }

std::vector<void*> LiveOf(const wchar_t* cls) {
    std::vector<void*> out;
    if (void* const c = OI::ClassByName(cls))
        OI::ForEachInstance(c, [](void* ctx, void* obj, int32_t) {
            if (obj && R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__"))
                static_cast<std::vector<void*>*>(ctx)->push_back(obj);
        }, &out);
    return out;
}

float Dist(void* a, void* b) {
    ue_wrap::FVector p{}, q{};
    if (!a || !b || !E::TryGetActorLocation(a, p) || !E::TryGetActorLocation(b, q)) return -1.f;
    return DG::Dist(p, q);
}

void* Nearest(const wchar_t* cls, void* to, float maxCm) {
    void* best = nullptr;
    float bd = maxCm;
    for (void* a : LiveOf(cls)) {
        const float d = Dist(a, to);
        if (d >= 0.f && d <= bd) {
            bd = d;
            best = a;
        }
    }
    return best;
}

bool HostMayStart(coop::net::Session& s) {
    return s.IsSlotWorldReady(kSlot) && coop::prop_snapshot::IsBracketClosed(kSlot) &&
           !coop::join_window_baseline::IsLateWindowOpen(kSlot);
}

bool ClientMayStart() {
    return coop::net_pump::HasAnnouncedWorldReady() &&
           coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle;
}

// ---- the player's own verbs ------------------------------------------------------------------------------------------------

// The hand key's `Hold Object` with `manual` set. A client's request to the host is open until its answer.
bool Hold(void* player, void* item) {
    bool collected = false;
    const bool ran = player && item && E::CallMainPlayerHoldObject(player, item, collected);
    Fact("Hold Object on %ls ran=%d collected=%d; a request to the host open=%u", item ? R::ClassNameOf(item).c_str() : L"(none)",
         ran ? 1 : 0, collected ? 1 : 0, static_cast<unsigned>(PAR::dev::PendingId()));
    return ran;
}

// The hand holds an actor, and on a client the lane has announced it and no request is open.
bool HandReady() {
    if (PAR::dev::PendingId() != 0) return false;
    void* const hand = HB::LocalHandActor();
    if (!hand) return false;
    return coop::roster::LocalIsHost() || HB::OwnAnnounced(R::ClassNameOf(hand).c_str());
}

// The target's playerUsedOn with the hand actor and the player's holding_name: the call the use key makes through the interface.
bool UseOn(void* player, void* target) {
    void* const hand = HB::LocalHandActor();
    ue_wrap::ParamFrame f(target ? R::FindDispatchFunctionCached(R::ClassOf(target), L"playerUsedOn") : nullptr);
    const int32_t off = player ? R::FindPropertyOffset(R::ClassOf(player), L"holding_name") : -1;
    if (!f.valid() || off < 0 || !hand) return false;
    R::FName held;
    std::memcpy(&held, static_cast<const char*>(player) + off, sizeof(held));
    return f.Set<void*>(L"player", player) && f.Set<void*>(L"holdObject", hand) &&
           f.SetRaw(L"holdPropName", &held, sizeof(held)) && ue_wrap::Call(target, f);
}

// The item's left or right click, the player's hit written on `target` first (a held item's use breaks the player's own trace).
bool HandUse(void* player, void* item, void* target, bool rmb) {
    ue_wrap::FVector at{};
    if (!player || !item || !target || !E::TryGetActorLocation(target, at) ||
        !E::WriteMainPlayerHitResult(player, target, ue_wrap::prop::GetStaticMesh(target), at))
        return false;
    void* const fn = R::FindDispatchFunctionCached(R::ClassOf(item), rmb ? L"playerHandUse_RMB" : L"playerHandUse_LMB");
    ue_wrap::ParamFrame f(fn);
    return fn && f.valid() && f.Set<void*>(L"player", player) && ue_wrap::Call(item, f);
}

bool DropHeld(void* player) {
    void* const fn = player ? R::FindDispatchFunctionCached(R::ClassOf(player), L"simulateDrop") : nullptr;
    ue_wrap::ParamFrame f(fn);
    return fn && f.valid() && f.Set<bool>(L"dontCollect", true) && ue_wrap::Call(player, f);
}

namespace {
coop::director::AimFan g_fan;
constexpr float kRimOfHalfHeight = 0.8f;   // the aim point stands this share of the bucket's half height above its bounds centre
constexpr float kRimFromActorCm = 100.f;   // colliding bounds whose centre is farther than this from the bucket are not its own
}

// The camera turned round a point near the target's rim until the player's own look-at trace names the target: the rim stands clear of
// the low world geometry beside a bucket's base, which a trace aimed at the origin meets first.
int Aim(void* player, void* target, std::string& why) {
    if (!player || !target) return 0;
    ue_wrap::FVector origin{}, extent{};
    if (!E::GetActorBounds(target, true, origin, extent)) {
        Invalid("the bounds of the staged bucket could not be read, so its rim is not known to aim at");
        return 0;
    }
    ue_wrap::FVector loc{};
    if (!E::TryGetActorLocation(target, loc) || std::hypot(origin.X - loc.X, origin.Y - loc.Y) > kRimFromActorCm) {
        Invalid("the staged bucket's colliding bounds do not stand at the bucket, so its rim is not known to aim at");
        return 0;
    }
    const ue_wrap::FVector at{origin.X, origin.Y, origin.Z + kRimOfHalfHeight * extent.Z};
    const bool aimed = E::ReadMainPlayerHitActor(player) == target;
    const auto st = g_fan.Tick(player, at, aimed);
    if (st == coop::director::AimFan::State::Failed) {
        const ue_wrap::FVector cam = E::GetCameraLocation();
        void* const hit = E::ReadMainPlayerHitActor(player);
        const float reach = std::sqrt((cam.X - at.X) * (cam.X - at.X) + (cam.Y - at.Y) * (cam.Y - at.Y) + (cam.Z - at.Z) * (cam.Z - at.Z));
        char text[400];
        std::snprintf(text, sizeof(text),
                      "the camera was turned round the target's rim and the player's trace never named it: this camera is at (%.0f, %.0f, %.0f), %.0f cm from "
                      "the rim point at (%.0f, %.0f, %.0f); the trace names %ls",
                      static_cast<double>(cam.X), static_cast<double>(cam.Y), static_cast<double>(cam.Z), static_cast<double>(reach),
                      static_cast<double>(at.X), static_cast<double>(at.Y), static_cast<double>(at.Z),
                      hit ? R::ClassNameOf(hit).c_str() : L"(nothing)");
        why = text;
        return -1;
    }
    return st == coop::director::AimFan::State::Aimed ? 1 : 0;
}

void AimReset() { g_fan = coop::director::AimFan(); }

// ---- the counters of the client's commits ----

// The commits the host took of the client's, whatever their ending, and the ones that applied.
uint32_t CommitsTaken() {
    using Step = coop::prop_act_host::internal::CommitStep;
    uint32_t n = 0;
    for (const Step s : {Step::FamilyApply, Step::Abort, Step::BadCommit, Step::TargetGone})
        n += coop::prop_act_host::CommitsOf(kSlot, s);
    return n;
}

uint32_t Applies() { return coop::prop_act_host::CommitsOf(kSlot, coop::prop_act_host::internal::CommitStep::FamilyApply); }

uint32_t Refusals() {
    return coop::prop_act_host::AnsweredTotal(kSlot) -
           coop::prop_act_host::AnsweredFor(kSlot, coop::net::PropActReason::Granted);
}

Round RoundBegin() { return Round{CommitsTaken(), Applies(), Refusals()}; }

int RoundStep(const Round& r, const char* what) {
    if (Refusals() > r.refusals) {
        Fail("the host refused the client's %s", what);
        return -1;
    }
    if (CommitsTaken() <= r.taken) {
        Late("the host never took the client's commit");
        return 0;
    }
    if (Applies() <= r.applies) {
        Fail("the client's %s commit did not apply on the host: it was aborted or the target had changed", what);
        return -1;
    }
    return 1;
}

bool PendingIdOpen() { return PAR::dev::PendingId() != 0; }

bool RecordClosed() {
    if (PAR::dev::PendingId() != 0) {
        Late("the host never answered the client's ask");
        return false;
    }
    return true;
}

bool RowPast(const char* prefix, int n, int* out, bool bounded) {
    std::string rest;
    if (!Rest(prefix, &rest)) {
        if (bounded) Late("the host's row line never came");
        return false;
    }
    if (!ParseInts(rest, n, out)) {
        Invalid("the host's row line could not be read");
        return false;
    }
    if (coop::bucket_sync::dev::AppliedSeq(static_cast<uint32_t>(out[0])) < static_cast<uint32_t>(out[1])) {
        if (bounded) Late("the host's row past the commit never reached this client's copy");
        return false;
    }
    return true;
}

// ---- the tick --------------------------------------------------------------------------------------------------------------

namespace {

void HostTickArm(coop::net::Session& s) {
    switch (ArmOf()) {
    case Arm::Soap:     return SoapHost(s);
    case Arm::Dip:      return DipHost(s);
    case Arm::Mix:      return MixHost(s);
    case Arm::Trowel:
    case Arm::Ctick:    return TrowelHost(s);
    case Arm::Transfer: return TransferHost(s);
    case Arm::Pin:      return PinHost(s);
    case Arm::Visuals:  return VisualsHost(s);
    case Arm::Mop:      return MopHost(s);
    case Arm::Drop:     return DropHost(s);
    case Arm::Join:     return JoinHost(s);
    case Arm::Alc:
    case Arm::Brew:     return AlcHost(s);
    case Arm::Off:      return;
    }
}

void ClientTickArm() {
    switch (ArmOf()) {
    case Arm::Soap:     return SoapClient();
    case Arm::Dip:      return DipClient();
    case Arm::Mix:      return MixClient();
    case Arm::Trowel:
    case Arm::Ctick:    return TrowelClient();
    case Arm::Transfer: return TransferClient();
    case Arm::Pin:      return PinClient();
    case Arm::Visuals:  return VisualsClient();
    case Arm::Mop:      return MopClient();
    case Arm::Drop:     return DropClient();
    case Arm::Join:     return JoinClient();
    case Arm::Alc:
    case Arm::Brew:     return AlcClient();
    case Arm::Off:      return;
    }
}

}  // namespace

void TickImpl(coop::net::Session& session) {
    if (ArmOf() == Arm::Off || g_ended) return;
    if (!g_armed) {
        g_armed = true;
        DC::SeenHas(kPrefix);   // the first read starts the chat history
    }
    if (coop::roster::LocalIsHost()) HostTickArm(session);
    else ClientTickArm();
}

void ResetArms() {
    ResetVerbs();
    ResetConcrete();
    ResetTickArms();
    ResetDrop();
    ResetJoin();
    ResetAlc();
}

void Reset() {
    g_ended = g_armed = false;
    g_stepTicks = 0;
    AimReset();
    ApproachReset();
    StageReset();
    ClientReset();
    WatchReset();
    ResetArms();
}

}  // namespace coop::dev::bucket_drill::detail

namespace coop::dev::bucket_drill {

void Tick(coop::net::Session* session) {
    if (session) detail::TickImpl(*session);
}

void OnDisconnect() { detail::Reset(); }

}  // namespace coop::dev::bucket_drill
