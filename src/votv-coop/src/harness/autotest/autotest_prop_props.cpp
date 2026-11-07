// harness/autotest/autotest_prop_props.cpp -- TEST-ONLY: the toolgun's prop-properties tool across two peers.
//
// One arm per run, VOTVCOOP_PROP_PROPS_ARM on both peers (g1..g6), the drill itself gated by VOTVCOOP_RUN_PROP_PROPS and
// the peers' folder VOTVCOOP_CAN_DIR. The client is walked to the host by the director; the host lays a prop on the floor
// in the client puppet's sight; the client takes a toolgun into its hand (the host reads it, the premise of every
// press) and presses tool_propProps_C on its own copy of the prop through the toolgun's outHit and the tool's init, with
// the tool's fields set as the spawn menu's assignParameters sets them. Each arm is judged on the HOST from the flags
// (static,frozen,sleep) both peers read, and says its own `rig: VERDICT prop-props-<arm>`.

#include "harness/autotest.h"
#include "disc_drills.h"   // co-located private header (src tree, not include/)
#include "harness/autotest/meet.h"
#include "harness/autotest/peer_signal.h"
#include "harness/session_runtime.h"

#include "coop/comms/chat_feed.h"
#include "coop/config/config.h"
#include "coop/dev/drill_hand.h"
#include "coop/dev/drill_props.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/permissions/gameplay_nodes.h"
#include "coop/permissions/permission_host.h"
#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"
#include "coop/player/roster_ledger.h"
#include "coop/session/net_pump.h"
#include "coop/session/rig_verdict.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/trace.h"
#include "ue_wrap/desk/dish_writers.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"
#include "ue_wrap/engine/hit_result.h"

#include <windows.h>

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace harness::autotest {
namespace {

namespace DP = coop::dev::drill_props;
namespace DW = ue_wrap::dish_writers;
namespace E  = ue_wrap::engine;
namespace PS = peer_signal;
namespace R  = ue_wrap::reflection;
namespace RV = coop::rig_verdict;
namespace GN = coop::permissions::gameplay_nodes;
using disc_drill::OnGameThread;
using ue_wrap::FVector;

constexpr const wchar_t* kSackPath = L"/Game/objects/prop_pointSack.prop_pointSack_C";
constexpr const wchar_t* kRugPath = L"/Game/objects/prop_rug.prop_rug_C";
constexpr const wchar_t* kToolPath = L"/Game/objects/tool_propProps.tool_propProps_C";
constexpr const wchar_t* kToolgunPath = L"/Game/objects/prop_toolgun.prop_toolgun_C";
constexpr const wchar_t* kToolgunItem = L"toolgun";
constexpr const wchar_t* kToolgunClass = L"prop_toolgun_C";
constexpr const char* kNoRight = "The host does not allow the properties tool";
constexpr const char* kOutOfSight = "You can't see that from there";
constexpr float kFloorDownCm = 400.f;
constexpr float kMinColumnCm = 5.f;
constexpr float kRiseCm = 40.f;
constexpr float kMidUpCm = 30.f;        // the height of a prop's middle above the floor, for the clear-segment test
constexpr float kSightSlackCm = 25.f;
constexpr float kForgedOriginCm = 2000.f;
constexpr int kJoinSec = 480;
constexpr int kSignalSec = 240;
constexpr int kEidSec = 30;
constexpr int kHandSec = 30;
constexpr int kApplySec = 30;           // the press, the host's judge, the publication and the apply
constexpr int kNoticeSec = 40;          // a refusal reaching the client's screen
constexpr int kLaySec = 60;

// The arms, one per run:
//   g1  a client's freeze lands on the host's copy and the client's own.
//   g2  a client's make-static lands on both. With VOTVCOOP_PROP_PROPS_REJOIN=1 the client's first life then leaves and
//       the rejoined life (VOTVCOOP_PROP_PROPS_REJOINED=1, --rejoin-env) reads the prop static too:
//       `rig: VERDICT prop-props-g2-rejoin`.
//   g3  the host withholds the tool's node from the client (a permission fixture for the client's id, which the host
//       checks first): nothing changes on either copy and the client shows the refusal. Red: prop_props_judge_off.
//   g4  a forged press whose origin lies 2000 uu from the client's body is refused for sight: nothing changes and the
//       client shows the refusal.
//   g5  a prop of a class that overrides setPropProps (a rug) is frozen by a client and both copies agree.
//   g6  the HOST presses the tool on a prop laid near the client (its own press, published from the tool's post row); the
//       client presses nothing and reports the flags its copy reads.
// Red arm of g1: prop_props_gate_off, whose press runs on the client's copy alone, so the host's copy keeps its flags.

struct Arm {
    const char* name;
    const wchar_t* path;      // the class the host lays
    uint8_t press[4];         // static, frozen, active, sleeping
    const char* expect;       // the flags both copies hold after a press that is applied
    bool refused;             // the press must be refused: nothing changes, the client shows `notice`
    const char* notice;
    bool forged;              // the press is a forged request over the wire
    bool hostPress;           // the host presses; the client only reads
};

constexpr Arm kArms[] = {
    {"g1", kSackPath, {0, 1, 0, 0}, "0,1,0", false, nullptr, false, false},
    {"g2", kSackPath, {1, 0, 0, 0}, "1,0,0", false, nullptr, false, false},
    {"g3", kSackPath, {0, 1, 0, 0}, "0,0,0", true, kNoRight, false, false},
    {"g4", kSackPath, {0, 1, 0, 0}, "0,0,0", true, kOutOfSight, true, false},
    {"g5", kRugPath, {0, 1, 0, 0}, "0,1,0", false, nullptr, false, false},
    {"g6", kSackPath, {0, 1, 0, 0}, "0,1,0", false, nullptr, false, true},
};

const Arm* ArmOf(const std::string& name) {
    for (const Arm& a : kArms)
        if (name == a.name) return &a;
    return nullptr;
}

std::string Tag(const Arm& a) { return std::string("prop-props-") + a.name; }

void Say(const std::string& tag, RV::Outcome o, const char* fmt, ...) {
    char reason[400];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(reason, sizeof(reason), fmt, args);
    va_end(args);
    RV::Say(tag.c_str(), o, "%s", reason);
}

// The flags of a prop copy as one word, "static,frozen,sleep"; "unread" for none.
std::string FlagsOf(void* prop) {
    if (!prop) return "unread";
    char b[16];
    std::snprintf(b, sizeof(b), "%d,%d,%d", ue_wrap::prop::IsStatic(prop) ? 1 : 0, ue_wrap::prop::IsFrozen(prop) ? 1 : 0,
                  ue_wrap::prop::IsSleeping(prop) ? 1 : 0);
    return b;
}

void* CopyOf(uint32_t eid) {
    const coop::element::Element* el = eid < coop::element::kMaxElements ? coop::element::Registry::Get().Get(eid) : nullptr;
    return el ? el->LiveActor() : nullptr;
}

std::string CopyFlags(uint32_t eid) {
    std::string out;
    OnGameThread([&] { out = FlagsOf(CopyOf(eid)); });
    return out;
}

bool CopyLive(uint32_t eid) {
    bool live = false;
    OnGameThread([&] { live = CopyOf(eid) != nullptr; });
    return live;
}

// How many lines of this peer's feed carry `text`.
int NoticeCount(const char* text) {
    int n = 0;
    coop::chat_feed::ForEachRow([&](const coop::chat_feed::RowView& r) {
        if (r.line.find(text) != std::string_view::npos) ++n;
    });
    return n;
}

bool WriteToolBool(void* tool, const wchar_t* field, bool value) {
    const int32_t off = R::FindPropertyOffset(R::ClassOf(tool), field);
    if (off < 0) return false;
    const bool v = value;
    std::memcpy(static_cast<uint8_t*>(tool) + off, &v, sizeof(v));
    return true;
}

// ---- the host ---------------------------------------------------------------------------------------------------

bool Clear(void* puppet, const FVector& eye, const FVector& mid, void* ignoreActor) {
    const uint8_t types[4] = {0, 1, 2, 3};
    void* ignore[coop::players::kMaxPeers + 1];
    int n = coop::players::Registry::Get().LiveBodies(ignore, coop::players::kMaxPeers);
    ignore[n++] = ignoreActor;
    ue_wrap::trace::Hit hit;
    if (!ue_wrap::trace::LineHitForObjects(puppet, eye, mid, types, 4, ignore, n, &hit)) return false;
    auto dist = [](const FVector& a, const FVector& b) {
        return std::sqrt((a.X - b.X) * (a.X - b.X) + (a.Y - b.Y) * (a.Y - b.Y) + (a.Z - b.Z) * (a.Z - b.Z));
    };
    return !hit.blocked || dist(eye, hit.point) >= dist(eye, mid) - kSightSlackCm;
}

// A prop of `path` laid on the floor in sight of slot 1's puppet: a fan of directions and two distances, each tested with
// the segment from the puppet's eye to the prop's middle; a prop in a spot that fails is taken away. Game thread.
void* LayInSight(const wchar_t* path) {
    void* cls = ue_wrap::asset_load::LoadObjectByPath(path);
    coop::RemotePlayer& rp = coop::puppet_drive::Puppet(1);
    void* puppet = rp.valid() && rp.HasPose() ? rp.GetActor() : nullptr;
    FVector eye{}, at{};
    if (!cls || !puppet || !rp.TryGetEyePosition(eye) || !E::TryGetActorLocation(puppet, at)) return nullptr;
    const FVector fwd = E::GetActorForwardVector(puppet);
    const float len = std::sqrt(fwd.X * fwd.X + fwd.Y * fwd.Y);
    if (len < 0.1f) return nullptr;
    const float degs[] = {0.f, 40.f, -40.f, 80.f, -80.f, 140.f, -140.f, 180.f};
    const float aheads[] = {150.f, 220.f};
    for (const float deg : degs) {
        const float rad = deg * (3.14159265f / 180.f);
        const float dx = (fwd.X * std::cos(rad) - fwd.Y * std::sin(rad)) / len;
        const float dy = (fwd.X * std::sin(rad) + fwd.Y * std::cos(rad)) / len;
        for (const float ahead : aheads) {
            void* prop = nullptr;
            float floorZ = 0.f;
            const DP::Drop d = DP::DropOnFloor(puppet, cls, nullptr, at.X + dx * ahead, at.Y + dy * ahead, at.Z,
                                               kFloorDownCm, kMinColumnCm, kRiseCm, &prop, &floorZ);
            if (d == DP::Drop::NoFloor) continue;
            if (d == DP::Drop::SpawnFailed) return nullptr;
            const FVector mid{at.X + dx * ahead, at.Y + dy * ahead, floorZ + kMidUpCm};
            if (Clear(puppet, eye, mid, prop)) return prop;
            E::DestroyActor(prop);
        }
    }
    return nullptr;
}

// The withheld node of the g3 arm: slot 1's proved id is refused it by the host's permissions. Game thread.
bool NodeWithheldForSlot1() {
    auto& s = harness::session_runtime::Session();
    const std::string guid = s.ProvedGuidForSlotWithToken(1, coop::roster_ledger::Get(1).bornGeneration);
    return !guid.empty() &&
           !coop::permissions::host::Allows(guid, GN::kToolgunPropProps.node, GN::kToolgunPropProps.defaultGranted, false);
}

bool Press(const Arm& arm, void* target, const char** why);   // defined with the client's side

// The host's own press: the toolgun in the host's hand, then the tool's init on its copy of the prop, the click's path.
bool HostPresses(const Arm& arm, uint32_t eid, const char** why) {
    OnGameThread([] { coop::dev::drill_hand::TakeIntoHandNow(kToolgunItem, kToolgunClass); });
    if (!PS::WaitFor("the toolgun in the host's hand", kHandSec, [] {
            bool in = false;
            OnGameThread([&] {
                void* h = coop::hand_item::LocalHandActor();
                in = h && R::ClassNameOf(h) == kToolgunClass;
            });
            return in;
        })) {
        *why = "the toolgun did not reach the host's hand";
        return false;
    }
    bool pressed = false;
    OnGameThread([&] {
        void* copy = CopyOf(eid);
        pressed = copy && Press(arm, copy, why);
    });
    return pressed;
}

// The join arm, host side: once the client's first life has left slot 1, the rejoined life must read the prop as the
// host's copy does.
void HostRejoin(const Arm& arm, uint32_t eid, const std::string& hostFlags) {
    const std::string tag = Tag(arm) + "-rejoin";
    auto& session = harness::session_runtime::Session();
    const uint32_t life1Gen = session.peerGenerationForSlot(1);
    PS::Mark("host_ack");
    if (!PS::WaitFor("the client's life 1 gone from slot 1", 300, [&session, life1Gen] {
            return !session.IsSlotConnected(1) || session.peerGenerationForSlot(1) != life1Gen;
        }))
        return;
    if (!PS::Await("rejoin_ready", 400)) return;
    PS::Mark("rejoin_target", std::to_string(eid) + " " + hostFlags);
    std::string word;
    if (!PS::AwaitText("rejoin_flags", word, 400)) return;
    UE_LOGI("prop-props: the rejoined life read %s, the host's copy %s", word.c_str(), hostFlags.c_str());
    Say(tag, word == hostFlags ? RV::Outcome::Pass : RV::Outcome::Fail,
        "the rejoined client reads the prop as '%s'; the host's copy holds '%s'", word.c_str(), hostFlags.c_str());
}

void RunHost(const Arm& arm) {
    const std::string tag = Tag(arm);
    if (!disc_drill::HostWorldUp()) return;
    PS::Mark("host_ready");
    if (!PS::Await("client_ready", kJoinSec) || !PS::Await("client_arrived", kJoinSec)) return;
    OnGameThread([] {
        ue_wrap::asset_load::LoadObjectByPath(kToolgunPath);
        ue_wrap::asset_load::LoadObjectByPath(kToolPath);
    });
    if (std::strcmp(arm.name, "g3") == 0) {
        bool withheld = false;
        OnGameThread([&] { withheld = NodeWithheldForSlot1(); });
        if (!withheld) {
            Say(tag, RV::Outcome::Invalid, "the host's permissions do not withhold %s from slot 1: the fixture is not installed",
                GN::kToolgunPropProps.node);
            return;
        }
    }
    void* prop = nullptr;
    PS::WaitFor("a prop laid on the floor in the client puppet's sight", kLaySec, [&] {
        OnGameThread([&] { prop = LayInSight(arm.path); });
        return prop != nullptr;
    });
    if (!prop) return;
    uint32_t eid = 0;
    PS::WaitFor("the prop's element id", kEidSec, [&] {
        OnGameThread([&] { eid = static_cast<uint32_t>(coop::element::Registry::Get().EidForActor(prop)); });
        return eid != 0 && eid != static_cast<uint32_t>(coop::element::kInvalidId);
    });
    if (!eid) return;
    PS::WaitFor("the prop at rest", kEidSec, [&] {
        bool rest = false;
        OnGameThread([&] { rest = E::IsActorRootBodyAtRest(prop); });
        return rest;
    }, false);
    const std::string before = CopyFlags(eid);
    // An applied press must change what the prop holds, and a refused one must find it at the flags it is to keep.
    if (arm.refused ? before != arm.expect : before == arm.expect) {
        Say(tag, RV::Outcome::Invalid, "the prop holds '%s' before the press, which proves nothing about flags '%s'", before.c_str(),
            arm.expect);
        return;
    }
    PS::Mark("target", std::to_string(eid));
    if (arm.hostPress) {
        // A state for an element the client has no copy of is dropped: the press waits for the client's copy.
        if (!PS::Await("copy_ready", kSignalSec)) return;
        const char* why = "";
        if (!HostPresses(arm, eid, &why)) {
            Say(tag, RV::Outcome::Invalid, "the press did not run on the host: %s", why);
            return;
        }
        UE_LOGI("prop-props: HOST pressed on element %u (the tool's init)", eid);
        PS::WaitFor("the host's copy at the press's flags", kApplySec, [&] { return CopyFlags(eid) == arm.expect; }, false);
    } else {
        if (!PS::Await("toolgun_in_hand", kSignalSec)) return;
        int held = 0;
        PS::WaitFor("the client's toolgun in hand here", kHandSec, [&] {
            OnGameThread([&] { held = coop::hand_item::HeldClassIs(1, kToolgunClass) ? 1 : 0; });
            return held == 1;
        }, false);
        UE_LOGI("prop-props: HeldClassIs(slot 1, prop_toolgun_C)=%d; the prop's element %u holds %s", held, eid, before.c_str());
        if (!held) {
            Say(tag, RV::Outcome::Invalid, "the host does not see a toolgun in the client's hand");
            return;
        }
    }
    PS::Mark("go", before);

    std::string clientWord;
    if (!PS::AwaitText("client_flags", clientWord, kSignalSec)) return;
    if (!arm.refused) {
        // The host's copy is waited on for the press's flags (the publication is the host's own apply); the client's word
        // was taken after its copy reached the host's reading.
        PS::WaitFor("the host's copy at the press's flags", kApplySec, [&] { return CopyFlags(eid) == arm.expect; }, false);
    }
    const std::string hostFlags = CopyFlags(eid);
    UE_LOGI("prop-props: %s host copy %s, client reports '%s'", arm.name, hostFlags.c_str(), clientWord.c_str());
    const std::string wantClient = std::string("flags=") + arm.expect + (arm.refused ? " notice=1" : "");
    // The client's word is "flags=<s,f,sl> notice=<0|1>"; a press that is applied has no notice to wait for.
    const bool clientOk = arm.refused ? clientWord == wantClient : clientWord.rfind(std::string("flags=") + arm.expect, 0) == 0;
    const bool hostOk = hostFlags == arm.expect;
    if (hostOk && clientOk) {
        Say(tag, RV::Outcome::Pass, "the host's copy holds '%s' and the client reports '%s'", hostFlags.c_str(), clientWord.c_str());
    } else if (arm.refused && !hostOk) {
        Say(tag, RV::Outcome::Fail, "the host applied a press that should have been refused: its copy holds '%s' (before '%s'); the client reports '%s'",
            hostFlags.c_str(), before.c_str(), clientWord.c_str());
    } else {
        Say(tag, RV::Outcome::Fail, "the host's copy holds '%s', the client reports '%s', wanted flags '%s'%s", hostFlags.c_str(),
            clientWord.c_str(), arm.expect, arm.refused ? " with the refusal shown" : "");
    }
    if (hostOk && clientOk && std::strcmp(arm.name, "g2") == 0 && coop::config::ReadEnv("VOTVCOOP_PROP_PROPS_REJOIN") == "1")
        HostRejoin(arm, eid, hostFlags);
    UE_LOGI("prop-props: DONE on host");
}

// ---- the client -------------------------------------------------------------------------------------------------

// One press on this client's copy of the prop: the toolgun in the hand holds the hit, a fresh tool holds the arm's values,
// and init(toolgun) runs as the toolgun's click runs it. The tool destroys itself; one left standing is destroyed here.
bool Press(const Arm& arm, void* target, const char** why) {
    void* toolCls = ue_wrap::object_index::ClassByName(L"tool_propProps_C");
    void* gun = coop::hand_item::LocalHandActor();
    void* me = coop::players::Registry::Get().Local();
    FVector at{}, tgt{};
    if (!toolCls || !gun || !me || R::ClassNameOf(gun) != kToolgunClass || !E::TryGetActorLocation(me, at) ||
        !E::TryGetActorLocation(target, tgt)) {
        *why = "the tool's class, the toolgun in the hand or a location is missing";
        return false;
    }
    void* tool = DW::SpawnTool(toolCls, {at.X, at.Y, at.Z + 60.f});
    if (!tool) {
        *why = "the tool did not spawn";
        return false;
    }
    const bool wrote = WriteToolBool(tool, L"isStatic", arm.press[0] != 0) && WriteToolBool(tool, L"isFrozen", arm.press[1] != 0) &&
                       WriteToolBool(tool, L"isActive", arm.press[2] != 0) && WriteToolBool(tool, L"isSleeping", arm.press[3] != 0);
    const bool ran = wrote && ue_wrap::hit_result::WriteField(gun, L"outHit", target, ue_wrap::prop::GetStaticMesh(target), tgt) &&
                     DW::CallToolInit(tool, gun);
    if (!ran) *why = wrote ? "outHit could not be written or init did not run" : "the tool's fields did not resolve";
    if (R::IsLive(tool)) E::DestroyActor(tool);
    return ran;
}

// A press no player's click made: the request a modified client would send, with its origin far from its body.
bool SendForged(const Arm& arm, uint32_t eid, void* target, const char** why) {
    void* me = coop::players::Registry::Get().Local();
    FVector cam{}, tgt{};
    if (!me || !E::ReadMainPlayerCameraLocation(me, cam) || !E::TryGetActorLocation(target, tgt)) {
        *why = "the camera or the prop's location did not read";
        return false;
    }
    coop::net::PropPropsIntentPayload p{};
    p.eid = eid;
    p.origin[0] = cam.X + kForgedOriginCm;
    p.origin[1] = cam.Y;
    p.origin[2] = cam.Z;
    p.hit[0] = tgt.X;
    p.hit[1] = tgt.Y;
    p.hit[2] = tgt.Z;
    p.isStatic = arm.press[0];
    p.isFrozen = arm.press[1];
    p.isActive = arm.press[2];
    p.isSleeping = arm.press[3];
    if (!harness::session_runtime::Session().SendReliableToSlot(0, coop::net::ReliableKind::PropPropsIntent, &p, sizeof(p))) {
        *why = "the forged request was not sent";
        return false;
    }
    return true;
}

void LeaveForRejoin() {
    PS::Await("host_ack", 60);
    OnGameThread([] {
        auto& s = harness::session_runtime::Session();
        if (s.running()) coop::net_pump::FleeAfterNativeMenuTravel(s, "prop-props: life 1 leaves for the rejoin");
    });
    PS::WaitFor("this life's session stopped", 15, [] { return !harness::session_runtime::Session().running(); }, false);
}

void RunClient(const Arm& arm) {
    const std::string tag = Tag(arm);
    if (!disc_drill::HostPuppetUp()) return;
    PS::Mark("client_ready");
    if (!meet::WalkToHost(tag.c_str())) return;
    PS::Mark("client_arrived");
    std::string eidText;
    if (!PS::AwaitText("target", eidText, kSignalSec)) return;
    const uint32_t eid = static_cast<uint32_t>(std::strtoul(eidText.c_str(), nullptr, 10));
    if (!PS::WaitFor("this client's copy of the prop", kEidSec, [&] { return CopyLive(eid); })) return;
    std::string before;
    if (arm.hostPress) {
        PS::Mark("copy_ready");
        if (!PS::AwaitText("go", before, kSignalSec)) return;
    } else {
        OnGameThread([] { ue_wrap::asset_load::LoadObjectByPath(kToolgunPath); });
        OnGameThread([] { coop::dev::drill_hand::TakeIntoHandNow(kToolgunItem, kToolgunClass); });
        if (!PS::WaitFor("the toolgun in this hand", kHandSec, [] {
                bool in = false;
                OnGameThread([&] {
                    void* h = coop::hand_item::LocalHandActor();
                    in = h && R::ClassNameOf(h) == kToolgunClass;
                });
                return in;
            }))
            return;
        PS::Mark("toolgun_in_hand");
        if (!PS::AwaitText("go", before, kSignalSec)) return;

        const char* why = "";
        bool pressed = false;
        OnGameThread([&] {
            void* copy = CopyOf(eid);
            pressed = copy && (arm.forged ? SendForged(arm, eid, copy, &why) : Press(arm, copy, &why));
        });
        if (!pressed) {
            Say(tag, RV::Outcome::Invalid, "the press did not run on the client: %s", why);
            return;
        }
        UE_LOGI("prop-props: CLIENT pressed on element %u (%s)", eid, arm.forged ? "forged" : "the tool's init");
    }

    // A refused press ends in the notice; an applied one ends when this copy reads the flags the press asked for (on a
    // red arm that never happens, and the word says so).
    if (arm.refused) {
        PS::WaitFor("the refusal on this screen", kNoticeSec, [&] { return NoticeCount(arm.notice) > 0; }, false);
    } else {
        PS::WaitFor("this copy at the press's flags", kApplySec, [&] { return CopyFlags(eid) == arm.expect; }, false);
    }
    const std::string flags = CopyFlags(eid);
    const bool notice = arm.notice && NoticeCount(arm.notice) > 0;
    const std::string word = "flags=" + flags + (arm.refused ? (notice ? " notice=1" : " notice=0") : "");
    UE_LOGI("prop-props: CLIENT copy: %s", word.c_str());
    PS::Mark("client_flags", word);

    if (std::strcmp(arm.name, "g2") == 0 && coop::config::ReadEnv("VOTVCOOP_PROP_PROPS_REJOIN") == "1") LeaveForRejoin();
    UE_LOGI("prop-props: DONE on client");
}

// The rejoined life: the host names the prop and its flags; this life reads its own copy once the connect set has made it.
void RunRejoined() {
    if (!disc_drill::HostPuppetUp()) return;
    PS::Mark("rejoin_ready");
    std::string word;
    if (!PS::AwaitText("rejoin_target", word, kSignalSec)) return;
    const uint32_t eid = static_cast<uint32_t>(std::strtoul(word.c_str(), nullptr, 10));
    const std::string host = word.substr(word.find(' ') + 1);
    PS::WaitFor("the rejoined copy at the host's flags", kApplySec, [&] { return CopyFlags(eid) == host; }, false);
    const std::string here = CopyFlags(eid);
    UE_LOGI("prop-props: rejoined: here %s, there %s", here.c_str(), host.c_str());
    PS::Mark("rejoin_flags", here);
    UE_LOGI("prop-props: REJOIN DONE");
}

}  // namespace

DWORD WINAPI PropPropsThread(LPVOID /*arg*/) {
    const std::string armName = coop::config::ReadEnv("VOTVCOOP_PROP_PROPS_ARM");
    const Arm* arm = ArmOf(armName);
    if (!arm) {
        RV::Say("prop-props", RV::Outcome::Invalid, "VOTVCOOP_PROP_PROPS_ARM '%s' is not g1, g2, g3, g4, g5 or g6", armName.c_str());
        return 0;
    }
    if (!PS::Begin(Tag(*arm).c_str())) return 0;
    if (!IsClientRole()) RunHost(*arm);
    else if (coop::config::ReadEnv("VOTVCOOP_PROP_PROPS_REJOINED") == "1") RunRejoined();
    else RunClient(*arm);
    return 0;
}

}  // namespace harness::autotest
