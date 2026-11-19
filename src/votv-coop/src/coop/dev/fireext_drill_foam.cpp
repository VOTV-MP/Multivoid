// coop/dev/fireext_drill_foam.cpp -- the spray and spray_join arms of the fire extinguisher drill; see coop/dev/fireext_drill.h.
//
// spray, leg 1: the host spawns a paper (C1) before the client's puppet, each peer lights its own copy, and the client
// sprays the extinguisher in its hand at it (its charge written to three seconds of spray, the press through mainPlayer's
// input stub). The client says when its own spray ended, with its own C1 read; the host decides at that line, on its mirror
// of the client's hand and its own C1, and then on the client's hand charge against the host's count. Leg 2: the same with
// the host spraying at a paper (C2) before its own pawn; the client reads its copy of C2 at the marker of the host's spray end
// reaching it. spray_join: before any client is launched (the rig's --client-after) the host sprays and sets an extinguisher
// (E) running away; once a joiner's world is up the host says "snapshot sent", and the joiner, whose lane delivers that line
// after the snapshot's messages, says whether it was told both and then whether it shows both (told and not shown is a FAIL
// at that read), and the end of the spray.
// A step waits on a state it reads, a chat line or a marker of fireext_sync; a time bound only says INVALID. The host
// prints every verdict, the client reports its reads in chat lines (its lines leave one per drill_chat_pace.h's pace, so the
// host decides a client's read at the arrival of the client's line). Players are never moved: a peer faces its target by
// writing its controller's rotation.

#include "fireext_drill_detail.h"

#include "coop/dev/director/director.h"
#include "coop/dev/director/input_stub.h"
#include "coop/dev/drill_chat.h"
#include "coop/dev/drill_ground.h"
#include "coop/dev/drill_hand.h"
#include "coop/element/registry.h"
#include "coop/net/net_clock.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"
#include "coop/props/fireext_sync.h"
#include "coop/props/prop_snapshot.h"
#include "coop/save/join_window_baseline.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"
#include "coop/session/rig_verdict.h"

#include "ue_wrap/actors/fire_extinguisher.h"
#include "ue_wrap/actors/int_objects.h"
#include "ue_wrap/actors/prop_fire.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_pawn.h"

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace coop::dev::fireext_drill::detail {
namespace {

namespace R   = ue_wrap::reflection;
namespace E   = ue_wrap::engine;
namespace FX  = ue_wrap::fire_extinguisher;
namespace PR  = ue_wrap::prop;
namespace DC  = coop::dev::drill_chat;
namespace DG  = coop::dev::drill_ground;
namespace DR  = coop::director;
namespace HB  = coop::hand_item;
namespace EL  = coop::element;
namespace FS  = coop::fireext_sync;
namespace RV  = coop::rig_verdict;

constexpr const wchar_t* kExtCls    = L"prop_fireExt_C";
constexpr const wchar_t* kExtPath   = L"/Game/objects/prop_fireExt.prop_fireExt_C";
constexpr const wchar_t* kPaperCls  = L"prop_paper_C";
constexpr const wchar_t* kPaperPath = L"/Game/objects/prop_paper.prop_paper_C";

constexpr float kFuel       = 60.f;    // the game's own cap: ignite clamps the fuel to 60 (comp_flammable)
constexpr float kPaperHealth = 100000.f;  // a burning paper's fire damage (4 to 5 a hit) never reaches the destroy in a minute
constexpr float kDamage     = 10.f;    // above the 5 that addDamage must exceed
constexpr float kSprayLife  = 0.05f;   // three seconds of spray (1/60 of the charge a second)
constexpr float kTargetCm   = 250.f;   // a paper's distance before a player: beyond the use key's reach
constexpr float kEntityCm   = 160.f;   // the join arm's extinguisher before the host
constexpr float kChargeTol  = 0.02f;   // the client hand's charge against the host's count
constexpr int   kQuietTicks = 30;      // a spawned target is at rest after this many quiet ticks in a row
constexpr uint64_t kLocalBoundMs = 120000;   // a step that waits on this peer's own state, INVALID only
constexpr uint64_t kPeerBoundMs  = 480000;   // a step that waits on the other peer or a join, INVALID only

const std::string kS1 = "fxs c1 ", kSEnd1 = "fxs end1 ", kSCharge = "fxs ch ", kS2 = "fxs c2 ", kSLit2 = "fxs c2lit",
                  kSOut2 = "fxs c2out ";
const std::string kJSnap = "fxj snapshot sent ", kJSaw = "fxj saw", kJMiss = "fxj miss ", kJNoShow = "fxj noshow ",
                  kJEnded = "fxj ended ";

bool g_join = false, g_over = false, g_chatArmed = false;
uint64_t g_stepMs = 0;
int g_quiet = 0;

bool g_host = false;   // this peer hosts: read off the session, which a host has before any client connects
char Who() { return g_host ? 'H' : 'C'; }
const char* Tag() { return g_join ? "fireext-spray-join" : "fireext-spray"; }
void Fact(const char* fmt, ...) {
    char buf[400];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    UE_LOGI("[FIREEXT-DRILL] [%c] %s", Who(), buf);
}

// The run's dead marker and the INVALID verdict: this arm could not measure.
void Invalid(const char* why) {
    if (g_over) return;
    g_over = true;
    UE_LOGW("[FIREEXT-DRILL] [%c] INVALID %s", Who(), why);
    RV::Say(Tag(), RV::Outcome::Invalid, "%s", why);
}

// The host's verdicts: a FAIL is said at the event that decides it, and nothing runs after.
void Verdict(RV::Outcome o, const char* fmt, ...) {
    if (g_over) return;
    g_over = true;
    char buf[300];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    RV::Say(Tag(), o, "%s", buf);
}

// A step that waits for its event: true when its bound ran out (the arm has said INVALID).
bool Late(uint64_t boundMs, const char* why) {
    if (coop::net::NowMs() - g_stepMs <= boundMs) return false;
    Invalid(why);
    return true;
}
void Stamp() {
    g_stepMs = coop::net::NowMs();
    g_quiet = 0;
}

// ---- reads of the extinguisher's and the paper's members -----------------------------------------------------------------

bool IsExt(void* o) { return o && R::IsLive(o) && FX::IsExtinguisher(o) && !FX::IsMount(o); }
int Firing(void* ext) {
    bool v = false;
    return FX::ReadSpraying(ext, v) ? (v ? 1 : 0) : -1;
}
int Active(void* ext) {
    bool v = false;
    return FX::ReadThrusting(ext, v) ? (v ? 1 : 0) : -1;
}
float Life(void* ext) {
    float v = -1.f;
    FX::ReadCharge(ext, v);
    return v;
}

// A prop's fire: its flameBase must hold a live comp_flammable that says it burns. 0 for a prop with no flame, -1 when the
// class has no flameBase.
int Burning(void* prop) {
    const int32_t off = prop ? R::FindPropertyOffset(R::ClassOf(prop), L"flameBase") : -1;
    if (off < 0) return -1;
    void* const flame = *reinterpret_cast<void* const*>(static_cast<const char*>(prop) + off);
    if (!flame || !R::IsLive(flame)) return 0;
    int32_t boolOff = -1;
    uint8_t mask = 0;
    if (!R::FindBoolProperty(R::ClassOf(flame), L"isBurning", boolOff, mask)) return -1;
    return (static_cast<const uint8_t*>(flame)[boolOff] & mask) ? 1 : 0;
}

uint32_t EidOf(void* actor) {
    const EL::ElementId eid = EL::Registry::Get().EidForActor(actor);
    return eid == EL::kInvalidId ? 0u : static_cast<uint32_t>(eid);
}

void* LiveProp(uint32_t eid) {
    return eid ? EL::LiveActorOfType(static_cast<EL::ElementId>(eid), EL::ElementType::Prop) : nullptr;
}

void* Local() {
    void* const me = coop::players::Registry::Get().Local();
    return me && R::IsLive(me) ? me : nullptr;
}

// A target `fwdCm` before `me`, dropped from its height.
void* SpawnTarget(void* me, const wchar_t* cls, const wchar_t* path, float fwdCm) {
    void* const c = ue_wrap::object_index::ClassByName(cls) ? ue_wrap::object_index::ClassByName(cls)
                                                             : ue_wrap::asset_load::LoadObjectByPath(path);
    ue_wrap::FVector at{};
    if (!c || !me || !E::TryGetActorLocation(me, at)) return nullptr;
    const ue_wrap::FVector f = E::GetActorForwardVector(me);
    return E::SpawnActor(c, {at.X + f.X * fwdCm, at.Y + f.Y * fwdCm, at.Z});
}

// This player faces `target`: its controller's rotation is written, as a human's mouse would, and nobody is moved.
void Face(void* me, void* target) {
    ue_wrap::FVector at{};
    void* const ctrl = me ? E::GetController(me) : nullptr;
    if (ctrl && target && E::TryGetActorLocation(target, at)) E::SetControlRotation(ctrl, DR::LookAt(E::GetCameraLocation(), at));
}

bool ClientMayStart() {
    return coop::net_pump::HasAnnouncedWorldReady() &&
           coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle;
}
bool HostMayStart(coop::net::Session& s) {
    return s.IsSlotWorldReady(1) && coop::prop_snapshot::IsBracketClosed(1) && !coop::join_window_baseline::IsLateWindowOpen(1);
}

// The text after `prefix` of the last line seen, as a number; false when there is none or it is not one.
bool NumberAfter(const std::string& prefix, double& out) {
    std::string rest;
    if (!DC::SeenRestOf(prefix, &rest)) return false;
    char* end = nullptr;
    out = std::strtod(rest.c_str(), &end);
    return end != rest.c_str();
}

// ---- the papers ----------------------------------------------------------------------------------------------------------

// A paper's fire: lit once, then read burning.
struct Fire {
    bool lit = false;
    uint64_t litMs = 0;
};
float BurnedS(const Fire& f) { return static_cast<float>(coop::net::NowMs() - f.litMs) / 1000.f; }

// The actor in `ref` has an element id and has stood quiet for kQuietTicks ticks. On the host `eid` is read off the actor; on
// a client it is the id the host named, and the copy is found by it. False while it waits.
bool AtRest(ue_wrap::CachedObjRef& ref, uint32_t& eid, bool host) {
    if (!host) ref.Set(LiveProp(eid));
    void* const c = ref.Get();
    if (host && !c) { Invalid("a spawned actor died before it came to rest"); return false; }
    if (host) eid = EidOf(c);
    if (c && eid && DG::Quiet(c)) {
        if (++g_quiet >= kQuietTicks) return true;
        return false;
    }
    g_quiet = 0;
    Late(kLocalBoundMs, "an actor never got its element id here and came to rest");
    return false;
}

// This peer's own copy of the paper lit and read burning. False while it waits.
bool PaperBurns(ue_wrap::CachedObjRef& ref, Fire& fire, uint32_t eid) {
    void* const c = ref.Get();
    if (!c) { Invalid("a paper died before it read burning"); return false; }
    if (!fire.lit) {
        // The game's fire damage ends a paper in about 2 s (its physicsImpact component takes the damage from its health and
        // destroys the prop), before the spray could be judged: the health is raised first, on each peer's own copy.
        float was = -1.f;
        PR::ReadImpactHealth(c, was);
        if (!PR::WriteImpactHealth(c, kPaperHealth)) { Invalid("a paper's physicsImpact health could not be written"); return false; }
        Fact("the paper's health %.1f raised to %.0f before its ignite", was, kPaperHealth);
        if (!PR::CallIgnite(c, kFuel)) { Invalid("a paper's ignite did not run"); return false; }
        fire.lit = true;
        fire.litMs = coop::net::NowMs();
    }
    if (Burning(c) != 1) {
        Late(kLocalBoundMs, "a paper never read burning after its ignite");
        return false;
    }
    Fact("the paper (eid %u) burns here", eid);
    return true;
}

// ---- the press -----------------------------------------------------------------------------------------------------------

DR::InputStub g_firePress{L"InpActEvt_fire_K2Node_InputActionEvent_58"};
DR::InputStub g_fireRelease{L"InpActEvt_fire_K2Node_InputActionEvent_59"};
bool g_sawFiring = false;   // this peer's own hand read firing after the press

// This peer's hand holds an extinguisher the hand lane announced: its charge written (a take leaves it at 0), `target` faced
// and the stub pressed. False while the hand is not ready or when the press did not run.
bool PressedAt(void* me, void* target, float charge) {
    void* const hand = HB::LocalHandActor();
    if (!IsExt(hand) || !HB::OwnAnnounced(kExtCls)) {
        Late(kLocalBoundMs, "this peer's hand never held an extinguisher the hand lane announced");
        return false;
    }
    FX::WriteCharge(hand, charge);
    Face(me, target);
    if (!DR::PressInputEvent(me, g_firePress)) { Invalid("the press through the input stub did not run"); return false; }
    Fact("pressed InpActEvt_fire _58; the hand's charge written %.2f", charge);
    g_sawFiring = false;
    return true;
}

// This peer's own spray is read until it has been seen firing and then is not (the game ends it when the charge is spent);
// `target` stays faced and the stub is released at the end. True at the end.
bool SprayEnded(void* me, void* target) {
    void* const hand = HB::LocalHandActor();
    if (!IsExt(hand)) { Invalid("this peer's hand lost its extinguisher during its spray"); return false; }
    Face(me, target);
    const int firing = Firing(hand);
    if (!g_sawFiring) {
        if (firing == 1) g_sawFiring = true;
        else Late(kLocalBoundMs, "the press never started this peer's spray");
        return false;
    }
    if (firing != 0) return false;
    DR::PressInputEvent(me, g_fireRelease);
    Fact("this peer's spray ended: its hand charge %.4f", Life(hand));
    return true;
}

// ===================================================================================================================
// spray
// ===================================================================================================================

enum class SH { Start, C1Settle, C1Lit, WaitEnd1, WaitCharge1, C2Settle, C2Lit, WaitC2Lit, Take, Spraying, WaitC2Out };
enum class SC { WaitC1, C1Settle, C1Lit, Take, Spraying, WaitMarker1, WaitC2, C2Settle, C2Lit, WaitMarker2, Done };
SH g_sh = SH::Start;
SC g_sc = SC::WaitC1;
ue_wrap::CachedObjRef g_c1, g_c2;
Fire g_fire1, g_fire2;
uint32_t g_eid1 = 0, g_eid2 = 0;
bool g_mirrorFired = false;     // the host: its mirror of the client's hand read firing
bool g_sprayTold = false;       // the host: the client's spray has been told on (SprayToldOn), so rays may exist
bool g_c1OutSaid = false;
bool g_took = false;
uint32_t g_armSeq = 0;          // the marker count when a wait for a marker began

void SHGo(SH s) {
    g_sh = s;
    Stamp();
}
void SCGo(SC s) {
    g_sc = s;
    Stamp();
}

void SprayHost(coop::net::Session& s) {
    void* const me = Local();
    if (!me) return;
    switch (g_sh) {
    case SH::Start: {
        if (!HostMayStart(s)) return;
        void* const puppet = coop::puppet_drive::Puppet(1).GetActor();
        if (!puppet || !R::IsLive(puppet)) return;
        g_c1.Set(SpawnTarget(puppet, kPaperCls, kPaperPath, kTargetCm));
        if (!g_c1.Get()) return Invalid("the world refused the paper's spawn before the client's puppet");
        Fact("spawned paper C1 %.0f cm before the client's puppet", kTargetCm);
        SHGo(SH::C1Settle);
        return;
    }
    case SH::C1Settle:
        if (AtRest(g_c1, g_eid1, true)) SHGo(SH::C1Lit);
        return;
    case SH::C1Lit:
        if (!PaperBurns(g_c1, g_fire1, g_eid1)) return;
        DC::Say(kS1 + std::to_string(g_eid1));
        SHGo(SH::WaitEnd1);
        return;
    case SH::WaitEnd1: {
        void* const c = g_c1.Get();
        void* const mirror = HB::MirrorActorForSlot(1);
        if (!g_mirrorFired && IsExt(mirror) && Firing(mirror) == 1) {
            g_mirrorFired = true;
            Fact("the mirror of the client's hand reads firing");
        }
        // No ray of the client's spray exists on this peer until the host has been told it, so a paper out before that went
        // out by itself and the arm has nothing to judge.
        if (FS::SprayToldOn(1)) g_sprayTold = true;
        if (c && !g_sprayTold && Burning(c) == 0) return Invalid("C1 went out before the host was told the client's spray: no ray put it out");
        if (c && !g_c1OutSaid && Burning(c) == 0) {
            g_c1OutSaid = true;
            Fact("C1 reads out %.1f s after it was lit", BurnedS(g_fire1));
        }
        std::string rest;
        if (!DC::SeenRestOf(kSEnd1, &rest)) {
            Late(kPeerBoundMs, "the client's spray-ended line never came");
            return;
        }
        if (!c) return Invalid("C1 died before the client's spray-ended line");
        if (!g_mirrorFired) {
            Verdict(RV::Outcome::Fail, "the host's mirror of the client's hand never read firing during the client's spray");
        } else if (Burning(c) != 0) {
            Verdict(RV::Outcome::Fail, "the host's C1 still burned when the client's spray ended (%.1f s since it was lit): "
                    "no ray reached it from the client's mirror", BurnedS(g_fire1));
        } else if (rest != "0") {
            Verdict(RV::Outcome::Fail, "the client's own C1 still burned when its own spray ended");
        } else {
            Fact("leg 1: the host's C1 is out and the client's own is out");
            SHGo(SH::WaitCharge1);
        }
        return;
    }
    case SH::WaitCharge1: {
        double said = 0.0;
        if (!NumberAfter(kSCharge, said)) {
            Late(kPeerBoundMs, "the client's charge line never came");
            return;
        }
        const float count = FS::HostSprayCount(1);
        if (count < 0.f || std::fabs(static_cast<float>(said) - count) > kChargeTol) {
            Verdict(RV::Outcome::Fail, "the client's hand charge %.4f and the host's count %.4f differ by over %.2f", said,
                    count, kChargeTol);
            return;
        }
        Fact("leg 1: the client's hand charge %.4f and the host's count %.4f agree", said, count);
        g_c2.Set(SpawnTarget(me, kPaperCls, kPaperPath, kTargetCm));
        if (!g_c2.Get()) return Invalid("the world refused the second paper's spawn before the host");
        SHGo(SH::C2Settle);
        return;
    }
    case SH::C2Settle:
        if (AtRest(g_c2, g_eid2, true)) SHGo(SH::C2Lit);
        return;
    case SH::C2Lit:
        if (!PaperBurns(g_c2, g_fire2, g_eid2)) return;
        DC::Say(kS2 + std::to_string(g_eid2));
        SHGo(SH::WaitC2Lit);
        return;
    case SH::WaitC2Lit:
        if (!DC::SeenHas(kSLit2)) {
            Late(kPeerBoundMs, "the client never said its copy of C2 burns");
            return;
        }
        if (!coop::dev::drill_hand::TakeIntoHandNow(L"fireExt", kExtCls)) return Invalid("the host's hand could not take an extinguisher");
        SHGo(SH::Take);
        return;
    case SH::Take:
        if (PressedAt(me, g_c2.Get(), kSprayLife)) SHGo(SH::Spraying);
        return;
    case SH::Spraying:
        if (SprayEnded(me, g_c2.Get())) SHGo(SH::WaitC2Out);
        return;
    case SH::WaitC2Out: {
        std::string rest;
        if (!DC::SeenRestOf(kSOut2, &rest)) {
            Late(kPeerBoundMs, "the client's C2 line never came");
            return;
        }
        if (rest == "0")
            Verdict(RV::Outcome::Pass, "a client's spray put out the host's copy of its paper and the host's put out the "
                    "client's, and the client's hand charge followed the host's count");
        else
            Verdict(RV::Outcome::Fail, "the client's copy of C2 still burned when the host's spray ended: no ray reached it "
                    "from the client's mirror of the host's hand");
        return;
    }
    }
}

void SprayClient() {
    void* const me = Local();
    if (!me || !ClientMayStart()) return;
    const uint8_t own = coop::players::Registry::Get().LocalPeerId();
    double eid = 0.0;
    switch (g_sc) {
    case SC::WaitC1:
        if (!NumberAfter(kS1, eid)) {
            Late(kPeerBoundMs, "the host's C1 line never came");
            return;
        }
        g_eid1 = static_cast<uint32_t>(eid);
        SCGo(SC::C1Settle);
        return;
    case SC::C1Settle:
        if (AtRest(g_c1, g_eid1, false)) SCGo(SC::C1Lit);
        return;
    case SC::C1Lit:
        if (PaperBurns(g_c1, g_fire1, g_eid1)) SCGo(SC::Take);
        return;
    case SC::Take:
        if (!g_took) {
            if (!coop::dev::drill_hand::TakeIntoHandNow(L"fireExt", kExtCls)) return Invalid("this client's hand could not take an extinguisher");
            g_took = true;
        }
        g_armSeq = FS::LastSprayApplied(own).seq;
        if (PressedAt(me, g_c1.Get(), kSprayLife)) SCGo(SC::Spraying);
        return;
    case SC::Spraying: {
        if (!SprayEnded(me, g_c1.Get())) return;
        const int burning = g_c1.Get() ? Burning(g_c1.Get()) : -1;
        Fact("this client's C1 reads burning=%d at the end of its spray", burning);
        DC::Say(kSEnd1 + (burning == 0 ? "0" : "1"));
        SCGo(SC::WaitMarker1);
        return;
    }
    case SC::WaitMarker1: {
        const FS::Applied a = FS::LastSprayApplied(own);
        if (a.seq == g_armSeq || a.on) {
            Late(kPeerBoundMs, "the host's end of this client's spray never reached its hand");
            return;
        }
        char line[48];
        std::snprintf(line, sizeof(line), "%.4f", a.charge);
        DC::Say(kSCharge + line);
        Fact("the host's spray end reached this hand: charge %.4f", a.charge);
        SCGo(SC::WaitC2);
        return;
    }
    case SC::WaitC2:
        if (!NumberAfter(kS2, eid)) {
            Late(kPeerBoundMs, "the host's C2 line never came");
            return;
        }
        g_eid2 = static_cast<uint32_t>(eid);
        SCGo(SC::C2Settle);
        return;
    case SC::C2Settle:
        if (AtRest(g_c2, g_eid2, false)) SCGo(SC::C2Lit);
        return;
    case SC::C2Lit:
        if (!PaperBurns(g_c2, g_fire2, g_eid2)) return;
        g_armSeq = FS::LastSprayApplied(0).seq;
        DC::Say(kSLit2);
        SCGo(SC::WaitMarker2);
        return;
    case SC::WaitMarker2: {
        const FS::Applied a = FS::LastSprayApplied(0);
        if (a.seq == g_armSeq || a.on) {
            Late(kPeerBoundMs, "the host's spray end never reached this client's mirror of its hand");
            return;
        }
        const int burning = g_c2.Get() ? Burning(g_c2.Get()) : -1;
        Fact("the host's spray end reached this client: its C2 reads burning=%d. SPRAY DONE", burning);
        DC::Say(kSOut2 + (burning == 0 ? "0" : "1"));
        SCGo(SC::Done);
        return;
    }
    case SC::Done:
        return;
    }
}

// ===================================================================================================================
// spray_join
// ===================================================================================================================

enum class JH { Start, WaitHand, WaitE, Damage, Press, Hold, Snapshot, WaitSaw, WaitEnded };
enum class JC { WaitLine, Decide, WaitShown, WaitEnd, Done };
JH g_jh = JH::Start;
JC g_jc = JC::WaitLine;
ue_wrap::CachedObjRef g_e;
bool g_prevReady[coop::net::kMaxPeers] = {};
bool g_heldSaid = false, g_damaged = false, g_pressed = false;
uint32_t g_sawSeq = 0;

void JHGo(JH s) {
    g_jh = s;
    Stamp();
}
void JCGo(JC s) {
    g_jc = s;
    Stamp();
}

// The host's hand and E stay full while the joiner loads, so a slow join does not drain what it is to be shown.
void KeepFull() {
    FX::WriteCharge(HB::LocalHandActor(), 1.f);
    if (void* const e = g_e.Get()) FX::WriteCharge(e, 1.f);
}

void JoinHost(coop::net::Session& s) {
    void* const me = Local();
    if (!me) return;
    void* const hand = HB::LocalHandActor();
    switch (g_jh) {
    case JH::Start:
        if (!coop::dev::drill_hand::TakeIntoHandNow(L"fireExt", kExtCls)) return Invalid("the host's hand could not take an extinguisher");
        JHGo(JH::WaitHand);
        return;
    case JH::WaitHand:
        if (!IsExt(hand)) {
            Late(kLocalBoundMs, "the host's hand never held an extinguisher");
            return;
        }
        g_e.Set(SpawnTarget(me, kExtCls, kExtPath, kEntityCm));
        if (!g_e.Get()) return Invalid("the world refused the extinguisher E's spawn");
        JHGo(JH::WaitE);
        return;
    case JH::WaitE: {
        uint32_t eid = 0;
        if (!g_e.Get()) return Invalid("E died before it was damaged");
        if (AtRest(g_e, eid, true)) JHGo(JH::Damage);
        return;
    }
    case JH::Damage: {
        void* const e = g_e.Get();
        ue_wrap::FVector at{};
        if (!e || !E::TryGetActorLocation(e, at)) return Invalid("E's place could not be read before its damage");
        if (!g_damaged) {
            g_damaged = true;
            if (!ue_wrap::int_objects::InvokeAddDamage(e, nullptr, kDamage, at, ue_wrap::FVector{})) return Invalid("E's addDamage did not run");
        }
        if (Active(e) != 1) {
            Late(kLocalBoundMs, "the damage never started E's runaway");
            return;
        }
        KeepFull();
        Fact("E (eid %u) runs away; the host's hand charge and E's are held at 1", EidOf(e));
        JHGo(JH::Press);
        return;
    }
    case JH::Press:
        KeepFull();
        if (!g_pressed) {
            g_pressed = true;
            if (!DR::PressInputEvent(me, g_firePress)) return Invalid("the press through the input stub did not run");
        }
        if (Firing(hand) != 1) {
            Late(kLocalBoundMs, "the press never started the host's spray");
            return;
        }
        for (int i = 1; i < static_cast<int>(coop::net::kMaxPeers); ++i) g_prevReady[i] = s.IsSlotWorldReady(i);
        JHGo(JH::Hold);
        return;
    case JH::Hold: {
        KeepFull();
        if (!g_heldSaid) {
            g_heldSaid = true;
            Fact("SPRAY-JOIN HOLDING: the host sprays and E thrusts; the client may be launched");
        }
        if (Firing(hand) != 1) return Invalid("the host's spray ended before a joiner's world came up");
        for (int i = 1; i < static_cast<int>(coop::net::kMaxPeers); ++i) {
            const bool ready = s.IsSlotWorldReady(i);
            const bool edge = ready && !g_prevReady[i];
            g_prevReady[i] = ready;
            if (!edge) continue;
            Fact("slot %d's world is up: the join's replay has run", i);
            JHGo(JH::Snapshot);
            return;
        }
        return;
    }
    case JH::Snapshot:   // one tick after the edge: the replay, the snapshot included, runs at the edge
        KeepFull();
        DC::Say(kJSnap + std::to_string(EidOf(g_e.Get())));
        JHGo(JH::WaitSaw);
        return;
    case JH::WaitSaw: {
        KeepFull();
        std::string rest;
        if (DC::SeenHas(kJSaw)) {
            DR::PressInputEvent(me, g_fireRelease);
            Fact("the joiner shows both: the host's stub is released");
            JHGo(JH::WaitEnded);
            return;
        }
        if (DC::SeenRestOf(kJNoShow, &rest)) {
            if (rest.size() >= 1 && rest[0] == '1') Verdict(RV::Outcome::Fail, "told the host's spray but its mirror did not show it");
            else Verdict(RV::Outcome::Fail, "told E's thrust but its copy did not show it");
            return;
        }
        if (!DC::SeenRestOf(kJMiss, &rest)) {
            Late(kPeerBoundMs, "the joiner's line never came");
            return;
        }
        void* const e = g_e.Get();
        const bool sprayMissed = rest.size() >= 1 && rest[0] == '1';
        const bool thrustMissed = rest.size() >= 3 && rest[2] == '1';
        const bool handOn = IsExt(hand) && Firing(hand) == 1;
        const bool eOn = e && Active(e) == 1 && Life(e) > 0.f;
        if (sprayMissed && handOn) Verdict(RV::Outcome::Fail, "the join snapshot did not carry the host's spray: the joiner was told none");
        else if (thrustMissed && eOn) Verdict(RV::Outcome::Fail, "the join snapshot did not carry E's thrust: the joiner was told none");
        else Invalid("what the joiner missed had already ended on the host");
        return;
    }
    case JH::WaitEnded: {
        std::string rest;
        if (!DC::SeenRestOf(kJEnded, &rest)) {
            Late(kPeerBoundMs, "the joiner's spray-ended line never came");
            return;
        }
        if (rest == "0")
            Verdict(RV::Outcome::Pass, "a joiner was told the host's spray and a runaway extinguisher that began before it "
                    "joined, showed both, and showed the spray end");
        else
            Verdict(RV::Outcome::Fail, "the joiner's mirror of the host's hand still sprayed after the host's release");
        return;
    }
    }
}

void JoinClient() {
    if (!Local() || !ClientMayStart()) return;
    switch (g_jc) {
    case JC::WaitLine: {
        double eid = 0.0;
        if (!NumberAfter(kJSnap, eid)) {
            Late(kPeerBoundMs, "the host's snapshot-sent line never came");
            return;
        }
        g_eid1 = static_cast<uint32_t>(eid);
        Fact("the host says its snapshot is sent (E is eid %u)", g_eid1);
        JCGo(JC::Decide);
        return;
    }
    case JC::Decide: {
        // The snapshot's messages reached this peer before the line (one lane): each state was told or it never will be.
        const bool sprayTold = FS::SprayToldOn(0) || FS::LastSprayApplied(0).seq != 0;
        const bool thrustTold = FS::ThrustWaits(g_eid1) || FS::LastThrustApplied(g_eid1).seq != 0;
        if (!sprayTold || !thrustTold) {
            Fact("the snapshot told this joiner: spray=%d thrust=%d", sprayTold ? 1 : 0, thrustTold ? 1 : 0);
            DC::Say(kJMiss + (sprayTold ? "0 " : "1 ") + (thrustTold ? "0" : "1"));
            JCGo(JC::Done);
            return;
        }
        JCGo(JC::WaitShown);
        return;
    }
    case JC::WaitShown: {
        void* const mirror = HB::MirrorActorForSlot(0);
        const FS::Applied spray = FS::LastSprayApplied(0);
        const FS::Applied thrust = FS::LastThrustApplied(g_eid1);
        void* const copy = LiveProp(g_eid1);
        // A marker is written after its actor: a marker on over an actor that reads off is a display that failed, decided now.
        const bool sprayNotShown = spray.on && IsExt(mirror) && Firing(mirror) == 0;
        const bool thrustNotShown = thrust.on && IsExt(copy) && Active(copy) == 0;
        if (sprayNotShown || thrustNotShown) {
            Fact("told but not shown: spray=%d thrust=%d", sprayNotShown ? 1 : 0, thrustNotShown ? 1 : 0);
            DC::Say(kJNoShow + (sprayNotShown ? "1 " : "0 ") + (thrustNotShown ? "1" : "0"));
            JCGo(JC::Done);
            return;
        }
        if (!(spray.on && IsExt(mirror) && Firing(mirror) == 1 && thrust.on && IsExt(copy) && Active(copy) == 1)) {
            Late(kPeerBoundMs, "the spray or the thrust the snapshot told never reached its actor");
            return;
        }
        g_sawSeq = spray.seq;
        Fact("this joiner shows the host's spray (mirror firing=1) and E's thrust (active=1)");
        DC::Say(kJSaw);
        JCGo(JC::WaitEnd);
        return;
    }
    case JC::WaitEnd: {
        const FS::Applied a = FS::LastSprayApplied(0);
        if (a.seq == g_sawSeq || a.on) {
            Late(kPeerBoundMs, "the host's spray end never reached this joiner");
            return;
        }
        void* const mirror = HB::MirrorActorForSlot(0);
        const int firing = IsExt(mirror) ? Firing(mirror) : -1;
        Fact("the host's spray end reached this joiner: its mirror reads firing=%d. SPRAY-JOIN DONE", firing);
        DC::Say(kJEnded + (firing == 0 ? "0" : "1"));
        JCGo(JC::Done);
        return;
    }
    case JC::Done:
        return;
    }
}

}  // namespace

void FoamTick(coop::net::Session& s, bool join) {
    if (g_over) return;
    g_join = join;
    if (!g_chatArmed) {
        DC::SeenHas(join ? "fxj " : "fxs ");   // the first Seen* read starts the chat history
        g_chatArmed = true;
        Stamp();
    }
    g_host = s.role() == coop::net::Role::Host;
    if (g_host) {
        if (join) JoinHost(s);
        else SprayHost(s);
    } else {
        if (join) JoinClient();
        else SprayClient();
    }
}

void FoamReset() {
    g_over = g_chatArmed = g_host = false;
    g_stepMs = 0;
    g_quiet = 0;
    g_sh = SH::Start;
    g_sc = SC::WaitC1;
    g_jh = JH::Start;
    g_jc = JC::WaitLine;
    g_c1.Reset();
    g_c2.Reset();
    g_e.Reset();
    g_fire1 = Fire{};
    g_fire2 = Fire{};
    g_eid1 = g_eid2 = 0;
    g_mirrorFired = g_sprayTold = g_c1OutSaid = g_sawFiring = g_took = g_heldSaid = g_damaged = g_pressed = false;
    g_armSeq = g_sawSeq = 0;
    for (bool& r : g_prevReady) r = false;
}

}  // namespace coop::dev::fireext_drill::detail
