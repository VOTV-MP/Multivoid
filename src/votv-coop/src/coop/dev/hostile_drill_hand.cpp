// coop/dev/hostile_drill_hand.cpp -- see coop/dev/hostile_drill_hand.h.

#include "coop/dev/hostile_drill_hand.h"

#include "coop/comms/chat_log.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace coop::dev::hostile_drill::hand {
namespace {

namespace R = ue_wrap::reflection;
using Clock = std::chrono::steady_clock;

// bad_hand_class names the engine's base actor. Every peer has it loaded, it is not a prop, and an
// instance of it is inert (no root component, so a mirror's placement calls do nothing).
constexpr const char* kActorClass = "Actor";
// bad_hand_family names the per-player inventory container in capitals: the engine compares names without
// case, so this spelling names that class (each peer has its own container, so it is loaded on the host),
// and a receipt that compares exactly lets it through. The judge's HeldClassIs compares without case too.
constexpr const char* kFamilyClass = "PROP_INVENTORYCONTAINER_PLAYER_C";

const char* ForgedClass(Arm a) { return a == Arm::BadHandFamily ? kFamilyClass : kActorClass; }

std::wstring Widen(const char* s) {
    std::wstring w;
    for (; *s; ++s) w.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*s)));
    return w;
}

// The fallback hold transform the owner ships before its first measure (hand_item.cpp), so the message
// is shaped as an honest one is in every field but the class.
constexpr float kFallbackRel[6] = {45.f, 10.f, -40.f, 0.f, 0.f, 0.f};
constexpr auto  kFailBound = std::chrono::seconds(180);  // a failure bound, never a wait

// A FIXED sentinel per arm: the host judges it, and a line its record kept from a prior run is excluded
// by the lineSeq baseline the host takes at arm.
std::string Sentinel(Arm a) { return std::string("hostile-drill ") + ArmName(a) + " sentinel"; }

// ---- client (sender) ----
bool g_sent = false;

bool OwnJoinOver() {
    return coop::net_pump::HasAnnouncedWorldReady() &&
           coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle;
}

// HandItem's wire layout (hand_item.cpp BuildPayload): slot, has, then a length-prefixed class, name and
// item, then the six-float hold transform when held. The name and item are empty, so no build writes an
// item name into the actor it spawns.
std::vector<uint8_t> BuildForgedHand(uint8_t slot, const char* cls) {
    std::vector<uint8_t> out;
    const size_t cl = std::strlen(cls);
    out.push_back(slot);
    out.push_back(1);
    out.push_back(static_cast<uint8_t>(cl));
    for (size_t i = 0; i < cl; ++i) out.push_back(static_cast<uint8_t>(cls[i]));
    out.push_back(0);  // name
    out.push_back(0);  // item
    const size_t base = out.size();
    out.resize(base + sizeof(kFallbackRel));
    std::memcpy(out.data() + base, kFallbackRel, sizeof(kFallbackRel));
    return out;
}

bool SendChatLine(coop::net::Session* s, const std::string& text) {
    coop::net::ChatMessagePayload line{};
    const size_t n = text.size() < sizeof(line.text) ? text.size() : sizeof(line.text);
    std::memcpy(line.text, text.data(), n);
    line.len = static_cast<uint8_t>(n);
    return s->SendReliable(coop::net::ReliableKind::ChatMessage, &line, sizeof(line));
}

// ---- host (judge) ----
// Judge waits for the sentinel. For bad_hand_class a stored class then needs the slot's puppet: the
// mirror pass skips a slot whose puppet is not up, and a joiner's first pose can come after its first
// messages. ReadMirror reads the mirror on the call after the one that first sees the puppet live: the
// pump runs the drills, then local_streams (hand_item::TickMirrors), then the puppet drive, so that
// call's mirror pass had the puppet, and the next call follows it.
enum class Step { WaitArm, Judge, ReadMirror, Done };
Step              g_step = Step::WaitArm;
int               g_slot = -1;
uint32_t          g_lineSeqBase = 0;
bool              g_puppetSeen = false;
Clock::time_point g_armAt{};

void Verdict(Arm a, const char* what, const char* detail) {
    UE_LOGI("[HOSTILE-DRILL] %s %s -- %s", ArmName(a), what, detail);
    g_step = Step::Done;
}

uint32_t LastLineSeq() {
    uint32_t last = 0;
    coop::chat_log::ForEach([&](const coop::chat_log::Row& r) { if (r.lineSeq > last) last = r.lineSeq; });
    return last;
}

bool SentinelLanded(Arm a) {
    bool seen = false;
    const std::string want = Sentinel(a);
    coop::chat_log::ForEach([&](const coop::chat_log::Row& r) {
        if (r.lineSeq > g_lineSeqBase && r.slot == static_cast<uint8_t>(g_slot) && r.text == want)
            seen = true;
    });
    return seen;
}

void ArmHost(coop::net::Session* s, Arm a) {
    for (int slot = 1; slot < static_cast<int>(coop::net::kMaxPeers) && g_slot < 0; ++slot)
        if (s->IsSlotWorldReady(slot)) g_slot = slot;
    if (g_slot < 0) return;
    g_lineSeqBase = LastLineSeq();
    g_armAt = Clock::now();
    g_step = Step::Judge;
    UE_LOGI("[HOSTILE-DRILL] %s armed on slot %d (chat lineSeq baseline %u)", ArmName(a), g_slot,
            g_lineSeqBase);
}

void Judge(Arm a) {
    if (!SentinelLanded(a)) {
        if (Clock::now() - g_armAt > kFailBound) Verdict(a, "INCONCLUSIVE", "the sentinel never arrived");
        return;
    }
    const std::wstring forged = Widen(ForgedClass(a));
    if (!coop::hand_item::HeldClassIs(static_cast<uint8_t>(g_slot), forged.c_str())) {
        Verdict(a, "PASS", "the forged hand class was not stored for the slot (and the host relays only what "
                           "it stores); the line sent behind it arrived");
        return;
    }
    if (a == Arm::BadHandFamily) {
        // The family's own instance is per-player state; a mirror of it is not waited for.
        UE_LOGW("[HOSTILE-DRILL] %s FAIL: stored", ArmName(a));
        g_step = Step::Done;
        return;
    }
    UE_LOGI("[HOSTILE-DRILL] %s the host STORED the forged class for slot %d; reading its mirror after a "
            "mirror pass with the slot's puppet up", ArmName(a), g_slot);
    g_step = Step::ReadMirror;
}

void ReadMirror(Arm a) {
    if (!g_puppetSeen) {
        coop::RemotePlayer* pup = coop::players::Registry::Get().Puppet(static_cast<uint8_t>(g_slot));
        if (pup && pup->valid()) g_puppetSeen = true;
        else if (Clock::now() - g_armAt > kFailBound) Verdict(a, "INCONCLUSIVE", "the slot's puppet never came up");
        return;
    }
    void* m = coop::hand_item::MirrorActorForSlot(static_cast<uint8_t>(g_slot));
    if (m && !ue_wrap::prop::IsDescendantOfProp(m)) {
        const std::wstring cls = R::ClassNameOf(m);
        UE_LOGW("[HOSTILE-DRILL] %s the slot's mirror is a '%ls', not a prop", ArmName(a), cls.c_str());
        UE_LOGW("[HOSTILE-DRILL] %s FAIL: spawned", ArmName(a));
    } else {
        UE_LOGW("[HOSTILE-DRILL] %s the slot's mirror: %s", ArmName(a), m ? "a prop" : "none");
        UE_LOGW("[HOSTILE-DRILL] %s FAIL: stored", ArmName(a));
    }
    g_step = Step::Done;
}

}  // namespace

const char* ArmName(Arm a) {
    switch (a) {
    case Arm::BadHandClass:  return "bad_hand_class";
    case Arm::BadHandFamily: return "bad_hand_family";
    default:                 return "off";
    }
}

void TickClient(coop::net::Session* s, Arm a) {
    if (g_sent || !OwnJoinOver()) return;
    const uint8_t self = coop::players::Registry::Get().LocalPeerId();
    if (self == 0 || self >= coop::players::kMaxPeers) return;  // no slot yet; wait
    g_sent = true;
    const std::vector<uint8_t> p = BuildForgedHand(self, ForgedClass(a));
    if (!s->SendReliableToSlot(0, coop::net::ReliableKind::HandItem, p.data(), static_cast<int>(p.size()))) {
        UE_LOGW("[HOSTILE-DRILL] %s INCONCLUSIVE: the forged hand state was not sent", ArmName(a));
        return;  // no sentinel behind it, so the host cannot judge
    }
    if (!SendChatLine(s, Sentinel(a))) {
        UE_LOGW("[HOSTILE-DRILL] %s INCONCLUSIVE: the sentinel was not sent", ArmName(a));
        return;
    }
    UE_LOGI("[HOSTILE-DRILL] %s sent a hand state naming class '%s' for slot %u + sentinel", ArmName(a),
            ForgedClass(a), static_cast<unsigned>(self));
}

void TickHostJudge(coop::net::Session* s, Arm a) {
    if (g_step == Step::WaitArm) { ArmHost(s, a); return; }
    if (g_step == Step::Done) return;
    if (!s->HasPeerConn(g_slot)) { Verdict(a, "INCONCLUSIVE", "the judged slot left before the verdict"); return; }
    if (g_step == Step::Judge)           Judge(a);
    else if (g_step == Step::ReadMirror) ReadMirror(a);
}

void OnDisconnect() {
    g_sent = false;
    g_step = Step::WaitArm;
    g_slot = -1;
    g_lineSeqBase = 0;
    g_puppetSeen = false;
    g_armAt = {};
}

}  // namespace coop::dev::hostile_drill::hand
