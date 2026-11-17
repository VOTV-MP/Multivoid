// coop/dev/hand_record_drill.cpp -- see coop/dev/hand_record_drill.h.

#include "coop/dev/hand_record_drill.h"

#include "hand_record_drill_detail.h"  // co-located private header: a can's state, the forged record

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/drill_chat.h"
#include "coop/dev/drill_hand.h"          // TakeIntoHandNow: the item into the hand, as a pick does
#include "coop/net/net_clock.h"
#include "coop/net/session.h"
#include "coop/player/hand_item.h"
#include "coop/player/hand_record_sync.h"
#include "coop/player/players_registry.h"
#include "coop/session/host_world_settle.h"  // Settled: the host's world has finished starting up
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"           // HasAnnouncedWorldReady
#include "coop/session/rig_verdict.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>

namespace coop::dev::hand_record_drill {
namespace {

namespace D  = coop::dev::hand_record_drill::detail;
namespace DC = coop::dev::drill_chat;
namespace E  = ue_wrap::engine;
namespace HS = coop::hand_record_sync;
namespace R  = ue_wrap::reflection;
namespace RV = coop::rig_verdict;

// The one verdict, said by the HOST alone, from the marks the two peers print in the lobby chat and its own reads. A
// mark is a state read where its effect has settled (a function of the game's own, run synchronously; a copy at rest; a
// record counted as written or landed): the host compares when both marks of a step are in, and the compare is the
// verdict's event. A bound below only ever says INVALID.
constexpr const char* kVerdict = "hand-record";

constexpr uint64_t kCheckEveryMs = 100;
constexpr uint64_t kStepBoundMs  = 240000;   // a step that waits on the other peer or on a record
constexpr float    kRestCm       = 0.5f;     // a copy that moved less than this since the last check is at rest

enum class Mode : uint8_t { Off, Can, Hand, Joiner, Forge, Hostile, Keyless, Oversize, Bounds, NonFinite, Box };
Mode TheMode() {
    static const Mode m = [] {
        const std::string v = coop::config::ResolveEnum(::coop::config_registry::rows::hand_record_drill);
        constexpr std::pair<const char*, Mode> kNames[] = {
            {"can", Mode::Can}, {"hand", Mode::Hand}, {"joiner", Mode::Joiner}, {"forge", Mode::Forge},
            {"hostile", Mode::Hostile}, {"keyless", Mode::Keyless}, {"oversize", Mode::Oversize},
            {"bounds", Mode::Bounds}, {"nonfinite", Mode::NonFinite}, {"box", Mode::Box}};
        for (const auto& n : kNames) if (v == n.first) return n.second;
        return Mode::Off;
    }();
    return m;
}

// The forgery each forge-family mode sends, the refusal that must answer it, and how the verdict names it.
struct ForgeArm { Mode mode; D::Forgery forgery; HS::Refusal refusal; const char* what; };
constexpr ForgeArm kForgeArms[] = {
    {Mode::Forge,     D::Forgery::Class,        HS::Refusal::Forged,       "a record of a class the client's hand does not hold"},
    {Mode::Hostile,   D::Forgery::UnknownClass, HS::Refusal::ClassUnknown, "a record naming a class that resolves to no class"},
    {Mode::Keyless,   D::Forgery::Keyless,      HS::Refusal::Keyless,      "a record without its key"},
    {Mode::Oversize,  D::Forgery::Oversize,     HS::Refusal::Oversize,     "a record over the lane's size ceiling"},
    {Mode::Bounds,    D::Forgery::Bounds,       HS::Refusal::Bounds,       "a record with a string past the lane's field bounds"},
    {Mode::NonFinite, D::Forgery::NonFinite,    HS::Refusal::NonFinite,    "a record with a non-finite number"},
};
const ForgeArm* ForgeArmOf(Mode m) {
    for (const ForgeArm& a : kForgeArms) if (a.mode == m) return &a;
    return nullptr;
}
D::Forgery ForgeryOf(Mode m) { const ForgeArm* a = ForgeArmOf(m); return a ? a->forgery : D::Forgery::Class; }

// Every refusal the host counted for `slot`, whatever its reason.
uint32_t RefusedTotal(uint8_t slot) {
    uint32_t n = 0;
    for (int i = 0; i < static_cast<int>(HS::Refusal::kCount); ++i) n += HS::RefusedCount(slot, static_cast<HS::Refusal>(i));
    return n;
}

bool     g_isHost = false;
int      g_session = 1;
uint64_t g_stepMs = 0;
uint64_t g_nextMs = 0;
int      g_op = 0;            // the client's op in the hand arm: take, open, eat, eat, empty
uint32_t g_stale0 = 0;        // the host's stale-drop count at the forge go
bool     g_armed = false;
ue_wrap::FVector g_lastAt{};
bool     g_lastKnown = false;

}  // namespace

// What the can arm (hand_record_drill_can.cpp) shares: the step state, the marks and the verdict's verbs.
namespace detail {

coop::net::Session* g_s = nullptr;
int      g_step = 0;          // this peer's step in its half of the arm; 99 is done
std::wstring g_key;           // the can's Key
uint32_t g_c0 = 0;            // a counter at the step's start
uint32_t g_c1 = 0;
Can      g_mark;              // the state a mark carried

void Next(int step) { g_step = step; g_stepMs = coop::net::NowMs(); g_lastKnown = false; }
bool Expired() { return coop::net::NowMs() - g_stepMs > kStepBoundMs; }

std::string ClientLine(const std::string& what) { return std::string(kC) + what; }
std::string HostLine(const std::string& what) { return std::string(kH) + what; }

// HOST: the one verdict.
void HostEnds(RV::Outcome o, const char* why) {
    if (o == RV::Outcome::Fail) UE_LOGW("%sFAIL in session %d: %s", kHead, g_session, why);
    else if (o == RV::Outcome::Invalid) UE_LOGW("%sABANDONED in session %d: %s", kHead, g_session, why);
    else UE_LOGI("%sPASS in session %d: %s", kHead, g_session, why);
    RV::Say(kVerdict, o, "%s", why);
    g_step = 99;
}

// CLIENT: what stopped this half, told to the host, which says the verdict.
void ClientReport(const char* word, const char* why) {
    UE_LOGI("%sclient %s in session %d: %s", kHead, word, g_session, why);
    DC::Say(ClientLine(std::string("done ") + word + " " + std::string(why).substr(0, 110)));
    g_step = 99;
}
void Fail(const char* why) { if (g_isHost) HostEnds(RV::Outcome::Fail, why); else ClientReport("fail", why); }
void Abandon(const char* why) { if (g_isHost) HostEnds(RV::Outcome::Invalid, why); else ClientReport("invalid", why); }

bool ClientReady() {
    return coop::net_pump::HasAnnouncedWorldReady() &&
           coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle;
}

// The can's own state read where this peer's drill changed it: the hand's, written here when the hold slot left it empty.
bool PutCanInHand() {
    return coop::dev::drill_hand::TakeIntoHandNow(D::kCanRow, D::kCanClass);
}

// The can the hold slot made reads foods 0 when its record carried none: it gets the class's four, and its own init
// names it.
bool PrepareHeldCan(void* can) {
    D::Can c = D::ReadCan(can);
    if (c.foods >= 3) return true;
    const int32_t off = ue_wrap::reflection::FindPropertyOffset(R::ClassOf(can), L"foods");
    if (off < 0) return false;
    *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(can) + off) = 4;
    return D::CallOn(can, L"init", nullptr);
}

// A copy is at rest when its place did not move since the last check.
bool AtRest(void* actor) {
    ue_wrap::FVector at{};
    if (!actor || !E::TryGetActorLocation(actor, at)) return false;
    const float dx = at.X - g_lastAt.X, dy = at.Y - g_lastAt.Y, dz = at.Z - g_lastAt.Z;
    const bool steady = g_lastKnown && dx * dx + dy * dy + dz * dz < kRestCm * kRestCm;
    g_lastAt = at;
    g_lastKnown = true;
    return steady;
}

}  // namespace detail

using namespace detail;

namespace {

// HOST: the client's own stop: a precondition it could not meet, or a failure it found alone.
bool PollClientStop() {
    std::string rest;
    if (!DC::SeenRestOf(std::string(kC) + "done ", &rest)) return false;
    const size_t sp = rest.find(' ');
    const std::string word = rest.substr(0, sp);
    if (word == "ok") return false;
    HostEnds(word == "invalid" ? RV::Outcome::Invalid : RV::Outcome::Fail,
             ("the client: " + (sp == std::string::npos ? std::string() : rest.substr(sp + 1))).c_str());
    return true;
}

// ---- The client's halves -----------------------------------------------------------------------------------

// hand: take, open, eat, eat, empty -- each marked once its record has gone, after the host's answer to the one before.
void ClientHand(void* player) {
    enum { Arm, Do, Hold, Sent, Ack };
    switch (g_step) {
    case Arm:
        if (!ClientReady()) return;
        g_op = 0;
        Next(Do);
        return;
    case Do: {
        if (g_op == 0) {
            if (!PutCanInHand()) { Abandon("the can could not be taken into the hand"); return; }
            Next(Hold);
            return;
        }
        void* can = D::HeldCan();
        if (!can) { Abandon("the can left the hand"); return; }
        if (g_op == 4) {
            if (!coop::dev::drill_hand::TakeIntoHandNow(L"None", nullptr)) { Abandon("the hand could not be emptied"); return; }
            DC::Say(ClientLine("m4 empty"));
            ClientReport("ok", "the hand emptied and marked");
            return;
        }
        if (!D::CallOn(can, g_op == 1 ? L"open" : L"eat", player)) { Abandon("the can's own verb did not run"); return; }
        g_mark = D::ReadCan(can);
        Next(Sent);
        return;
    }
    case Hold: {
        void* can = D::HeldCan();
        if (!can || !coop::hand_item::OwnAnnounced(D::kCanClass)) {
            if (Expired()) Abandon("the hand never held the can, announced");
            return;
        }
        if (!PrepareHeldCan(can)) { Abandon("the can's foods could not be set"); return; }
        g_mark = D::ReadCan(can);
        Next(Sent);
        return;
    }
    case Sent:
        // The mark follows the record on the one ordered lane, so the host reads its mirror once that record is judged: the
        // verb's init marked the hand's record owed, and the lane's tick sends it and clears the mark.
        if (HS::LaneCountsNow().ownPending) { if (Expired()) Abandon("the can's record was never sent"); return; }
        DC::Say(ClientLine("m" + std::to_string(g_op) + " " + D::Fmt(g_mark)));
        Next(Ack);
        return;
    case Ack:
        if (!DC::SeenHas(HostLine("ack" + std::to_string(g_op)))) { if (Expired()) Abandon("the host never answered a mark"); return; }
        ++g_op;
        Next(Do);
        return;
    default:
        return;
    }
}

// joiner: wait for the host's mark, read the mirror of the host's hand once the record behind it is in the mirror (or, when
// no record came with the mark, at once: that is the red's event), then read whether this peer still holds the host's
// record once the host says its hand is empty.
void ClientJoiner() {
    enum { Arm, Snap, Mirror, Empty };
    switch (g_step) {
    case Arm:
        if (!ClientReady()) return;
        Next(Snap);
        return;
    case Snap: {
        std::string rest;
        if (!DC::SeenRestOf(HostLine("m1 "), &rest)) { if (Expired()) Abandon("the host's mark never came"); return; }
        Next(Mirror);
        return;
    }
    case Mirror: {
        // The host's mark is behind the snapshot on the one ordered lane: a record that was sent is stored here already,
        // and the mirror made from the hand state shows it once it is written (Settled). A mark with no record behind it
        // is read as it stands.
        void* m = coop::hand_item::MirrorActorForSlot(0);
        if (!m) { if (Expired()) Abandon("no mirror of the host's hand stood"); return; }
        if (HS::CountsFor(0).stored && !HS::Settled(0)) { if (Expired()) Abandon("the host's record never settled in the mirror"); return; }
        DC::Say(ClientLine("v1 " + D::Fmt(D::ReadCan(m))));
        Next(Empty);
        return;
    }
    case Empty: {
        if (!DC::SeenHas(HostLine("m2 empty"))) { if (Expired()) Abandon("the host's empty mark never came"); return; }
        DC::Say(ClientLine("v2 stored=" + std::to_string(HS::CountsFor(0).stored ? 1 : 0)));
        ClientReport("ok", "both marks said");
        return;
    }
    default:
        return;
    }
}

// forge, hostile, keyless, oversize, bounds, nonfinite: hold the can announced, say so, send the forged record when the
// host says go.
void ClientForge() {
    enum { Arm, Take, Hold, Go, Sent };
    switch (g_step) {
    case Arm:
        if (!ClientReady()) return;
        Next(Take);
        return;
    case Take:
        if (!PutCanInHand()) { Abandon("the can could not be taken into the hand"); return; }
        Next(Hold);
        return;
    case Hold: {
        void* can = D::HeldCan();
        if (!can || !coop::hand_item::OwnAnnounced(D::kCanClass)) {
            if (Expired()) Abandon("the hand never held the can, announced");
            return;
        }
        if (!PrepareHeldCan(can)) { Abandon("the can's foods could not be set"); return; }
        g_mark = D::ReadCan(can);
        DC::Say(ClientLine("m1 " + D::Fmt(g_mark)));
        Next(Go);
        return;
    }
    case Go:
        if (!DC::SeenHas(HostLine("go"))) { if (Expired()) Abandon("the host never said go"); return; }
        if (!D::SendForged(*g_s, ForgeryOf(TheMode()))) { Abandon("the forged record could not be sent"); return; }
        DC::Say(ClientLine("m2 sent"));
        ClientReport("ok", "the forged record sent");
        return;
    default:
        return;
    }
}

// box: an order box into the hand (its getData writes a classes[] group, which a blanket refusal would drop), announced and
// keyed, its record sent, marked.
void ClientBox() {
    enum { Arm, Take, Hold, Sent };
    switch (g_step) {
    case Arm:
        if (!ClientReady()) return;
        Next(Take);
        return;
    case Take:
        if (!coop::dev::drill_hand::TakeIntoHandNow(D::kBoxRow, D::kBoxClass)) { Abandon("the order box could not be taken into the hand"); return; }
        Next(Hold);
        return;
    case Hold:
        if (!D::HeldBox() || !coop::hand_item::OwnAnnounced(D::kBoxClass)) {
            if (Expired()) Abandon("the hand never held the order box, announced and keyed");
            return;
        }
        Next(Sent);
        return;
    case Sent:
        if (HS::LaneCountsNow().ownPending) { if (Expired()) Abandon("the order box's record was never sent"); return; }
        DC::Say(ClientLine("m1 box"));
        ClientReport("ok", "the order box held and its record sent");
        return;
    default:
        return;
    }
}

void ClientTick() {
    void* player = coop::players::Registry::Get().Local();
    if (!player) return;
    switch (TheMode()) {
    case Mode::Can:    ClientCan(player); break;
    case Mode::Hand:   ClientHand(player); break;
    case Mode::Joiner: ClientJoiner(); break;
    case Mode::Forge:
    case Mode::Hostile:
    case Mode::Keyless:
    case Mode::Oversize:
    case Mode::Bounds:
    case Mode::NonFinite: ClientForge(); break;
    case Mode::Box:     ClientBox(); break;
    default: break;
    }
}

// ---- The host's halves -------------------------------------------------------------------------------------

// hand: the client's mark of each state, the host's mirror of that hand read once the record behind the mark is judged
// and written into it (Settled): the mark follows its record on the one ordered lane, so nothing can still move.
void HostHand() {
    enum { Arm, Mark, Empty };
    switch (g_step) {
    case Arm:
        if (!g_s->IsSlotWorldReady(1)) return;
        g_op = 0;
        Next(Mark);
        return;
    case Mark: {
        std::string rest;
        if (!DC::SeenRestOf(std::string(kC) + "m" + std::to_string(g_op) + " ", &rest)) { if (Expired()) Abandon("a client mark never came"); return; }
        D::Can mark;
        if (!D::ParseCan(rest, mark)) { Abandon("the client's mark carries no state"); return; }
        if (!HS::Settled(1)) { if (Expired()) Abandon("the host's mirror of the client's hand never settled on its record"); return; }
        void* m = coop::hand_item::MirrorActorForSlot(1);
        const D::Can mine = D::ReadCan(m);
        if (!(mine == mark)) {
            char why[150];
            std::snprintf(why, sizeof(why), "step %d: the host's mirror of the client's hand reads %s, the client's can reads %s",
                          g_op, D::Fmt(mine).c_str(), D::Fmt(mark).c_str());
            HostEnds(RV::Outcome::Fail, why);
            return;
        }
        DC::Say(HostLine("ack" + std::to_string(g_op)));
        if (++g_op == 4) { Next(Empty); return; }
        Next(Mark);
        return;
    }
    case Empty: {
        // The hand lane applied the empty hand: its record is cleared by then, or never will be.
        if (!DC::SeenHas(ClientLine("m4 empty"))) { if (Expired()) Abandon("the client's empty mark never came"); return; }
        if (coop::hand_item::HoldsItem(1)) { if (Expired()) Abandon("the client's hand never applied as empty here"); return; }
        if (HS::CountsFor(1).stored) { HostEnds(RV::Outcome::Fail, "the hand is empty but the host still holds its record"); return; }
        HostEnds(RV::Outcome::Pass, "the mirror showed each state at the client's marks and the record went with the hand");
        return;
    }
    default:
        return;
    }
}

// joiner: the host holds an opened can before the client joins, marks its state once the snapshot is sent, then empties
// its hand and marks that once its own record is cleared.
void HostJoiner(void* player) {
    enum { Arm, Take, Hold, Snap, V1, Empty, V2 };
    switch (g_step) {
    case Arm:
        if (!coop::host_world_settle::Settled()) return;
        Next(Take);
        return;
    case Take:
        if (!PutCanInHand()) { Abandon("the can could not be taken into the hand"); return; }
        Next(Hold);
        return;
    case Hold: {
        void* can = D::HeldCan();
        if (!can) { if (Expired()) Abandon("the hand never held the can"); return; }
        if (!PrepareHeldCan(can) || !D::CallOn(can, L"open", player) || !D::CallOn(can, L"eat", player)) {
            Abandon("the can's own open or eat did not run");
            return;
        }
        g_mark = D::ReadCan(can);
        if (g_mark.opened != 1) { Abandon("the can in the hand did not open"); return; }
        UE_LOGI("%shost: its can reads %s -- waiting for the client's snapshot", kHead, D::Fmt(g_mark).c_str());
        Next(Snap);
        return;
    }
    case Snap:
        // The snapshot is sent at the client's world-ready edge, and the host's own record is stored by then or goes to
        // the client when it is: the mark follows both on the one ordered lane.
        if (!g_s->IsSlotWorldReady(1) || HS::LaneCountsNow().snapshots == 0 || !HS::CountsFor(0).stored) {
            if (Expired()) Abandon("the client's snapshot was never sent with the host's record stored");
            return;
        }
        DC::Say(HostLine("m1 " + D::Fmt(g_mark)));
        Next(V1);
        return;
    case V1: {
        std::string rest;
        if (!DC::SeenRestOf(std::string(kC) + "v1 ", &rest)) { if (Expired()) Abandon("the client's mirror mark never came"); return; }
        D::Can theirs;
        if (!D::ParseCan(rest, theirs)) { Abandon("the client's mark carries no state"); return; }
        if (!(theirs == g_mark)) {
            char why[170];
            std::snprintf(why, sizeof(why), "the client's mirror of the host's hand reads %s at the snapshot, the host's can reads %s",
                          D::Fmt(theirs).c_str(), D::Fmt(g_mark).c_str());
            HostEnds(RV::Outcome::Fail, why);
            return;
        }
        g_c0 = HS::CountsFor(0).cleared;
        if (!coop::dev::drill_hand::TakeIntoHandNow(L"None", nullptr)) { Abandon("the hand could not be emptied"); return; }
        Next(Empty);
        return;
    }
    case Empty:
        // The host's own record goes at its next tick: the empty record to the client is behind that clear, and the mark
        // behind the empty record.
        if (HS::CountsFor(0).cleared == g_c0) { if (Expired()) Abandon("the host never cleared its record when its hand emptied"); return; }
        DC::Say(HostLine("m2 empty"));
        Next(V2);
        return;
    case V2: {
        std::string rest;
        if (!DC::SeenRestOf(std::string(kC) + "v2 stored=", &rest)) { if (Expired()) Abandon("the client's second mark never came"); return; }
        if (rest != "0") {
            HostEnds(RV::Outcome::Fail, "the host's hand is empty but the client still holds the host's record");
            return;
        }
        HostEnds(RV::Outcome::Pass, "the client's mirror showed the host's can at the snapshot and its record went with the hand");
        return;
    }
    default:
        return;
    }
}

// forge, hostile, keyless, oversize, bounds, nonfinite: the client's can is in its hand and applied on the host; the host
// says go and counts what follows. The refusal for the forgery is the PASS and a record of the client stored after the go
// is the FAIL: both are events of the host's judge, whichever comes first.
void HostForge() {
    enum { Arm, M1, Count };
    const ForgeArm* arm = ForgeArmOf(TheMode());
    if (!arm) return;
    switch (g_step) {
    case Arm:
        if (!g_s->IsSlotWorldReady(1)) return;
        Next(M1);
        return;
    case M1: {
        // Past the client's mark, its hand is applied here (no state owed): a swap in flight cannot be taken for a forgery.
        std::string mark;
        if (!DC::SeenRestOf(std::string(kC) + "m1 ", &mark)) {
            if (Expired()) Abandon("the client's mark never came");
            return;
        }
        if (coop::hand_item::HandStateOwed(1) || !coop::hand_item::HeldClassIs(1, D::kCanClass)) {
            if (Expired()) Abandon("the client's can was never applied as its hand here");
            return;
        }
        g_c0 = HS::RefusedCount(1, arm->refusal);
        g_c1 = HS::CountsFor(1).accepted;
        g_stale0 = HS::StaleDropped(1);
        DC::Say(HostLine("go"));
        Next(Count);
        return;
    }
    case Count: {
        char why[200];
        if (HS::CountsFor(1).accepted != g_c1) {
            std::snprintf(why, sizeof(why), "the host stored %s", arm->what);
            HostEnds(RV::Outcome::Fail, why);
            return;
        }
        if (HS::RefusedCount(1, arm->refusal) == g_c0) {
            if (HS::StaleDropped(1) != g_stale0) { Abandon("the forged record was dropped as stale: this hand held its class a moment ago"); return; }
            if (Expired()) { std::snprintf(why, sizeof(why), "%s was never refused", arm->what); Abandon(why); }
            return;
        }
        std::snprintf(why, sizeof(why), "the host refused %s, and stored none", arm->what);
        HostEnds(RV::Outcome::Pass, why);
        return;
    }
    default:
        return;
    }
}

// box: the client holds an order box, whose record carries a classes[] group; the host must store it, and a refusal of any
// reason is the FAIL.
void HostBox() {
    enum { Arm, M1, Count };
    switch (g_step) {
    case Arm:
        if (!g_s->IsSlotWorldReady(1)) return;
        g_c0 = HS::CountsFor(1).accepted;
        g_c1 = RefusedTotal(1);
        Next(M1);
        return;
    case M1:
        if (!DC::SeenHas(std::string(kC) + "m1 box")) { if (Expired()) Abandon("the client's mark never came"); return; }
        Next(Count);
        return;
    case Count:
        if (RefusedTotal(1) != g_c1) { HostEnds(RV::Outcome::Fail, "the host refused the held order box's record"); return; }
        if (HS::CountsFor(1).accepted == g_c0 || !HS::StoredClassIs(1, D::kBoxClass)) {
            if (Expired()) Abandon("the order box's record was never stored");
            return;
        }
        HostEnds(RV::Outcome::Pass, "the host stored the held order box's record, whose classes[] group a blanket refusal would have dropped");
        return;
    default:
        return;
    }
}

void HostTick() {
    void* player = coop::players::Registry::Get().Local();
    if (!player) return;
    if (PollClientStop()) return;
    switch (TheMode()) {
    case Mode::Can:    HostCan(); break;
    case Mode::Hand:   HostHand(); break;
    case Mode::Joiner: HostJoiner(player); break;
    case Mode::Forge:
    case Mode::Hostile:
    case Mode::Keyless:
    case Mode::Oversize:
    case Mode::Bounds:
    case Mode::NonFinite: HostForge(); break;
    case Mode::Box:     HostBox(); break;
    default: break;
    }
}

}  // namespace

void Tick(coop::net::Session* s) {
    if (TheMode() == Mode::Off || !s || g_step == 99) return;
    const uint64_t now = coop::net::NowMs();
    if (now < g_nextMs) return;
    g_nextMs = now + kCheckEveryMs;
    g_s = s;
    g_isHost = s->role() == coop::net::Role::Host;
    if (g_stepMs == 0) g_stepMs = now;
    // The chat history records the rows seen from the first read on, so the first read is the first tick, before either
    // peer's first line.
    if (!g_armed) {
        g_armed = true;
        DC::SeenHas("HREC arm");
    }
    if (g_isHost) HostTick();
    else if (s->connected()) ClientTick();
}

void OnDisconnect() {
    if (TheMode() == Mode::Off) return;
    ++g_session;
    g_step = 0;
    g_stepMs = g_nextMs = 0;
    g_op = 0;
    g_armed = false;
    g_key.clear();
    g_lastKnown = false;
    g_c0 = g_c1 = 0;
    ResetCan();
    g_mark = D::Can{};
}

}  // namespace coop::dev::hand_record_drill
