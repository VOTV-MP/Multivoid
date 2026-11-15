// coop/world/easter_hunt.cpp -- see coop/world/easter_hunt.h.
//
// easterZone_C's graph, cooked (0.10.0; the row below ties these offsets to that bytecode):
//   2337  playAnimation: setEvent(true), the roar, five explode() a second apart (each a zero-damage explosion_C, which a
//         client's explosion lane dissolves: the host's is sent), the thrusters, the mesh shown, the timeline started
//   1276  the resume of the timeline's finish, 2 s on: the bunny's mint                 <- refused on a client
//   1456  the tail after the mint: thrusters and rocket sound off, tick off, mesh hidden, setEvent(false)   <- entered instead

#include "coop/world/easter_hunt.h"

#include "coop/element/mirror_manager.h"
#include "coop/element/mirror_managers.h"
#include "coop/element/prop.h"
#include "coop/element/registry.h"
#include "coop/element/world_actor.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/props/prop_element_tracker.h"
#include "coop/props/prop_key_index.h"
#include "coop/world/world_actor_sync.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/profile.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace coop::easter_hunt {
namespace {

namespace E = ue_wrap::engine;
namespace R = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;
using Clock = std::chrono::steady_clock;
using Payload = coop::net::AmbientFixtureStatePayload;

constexpr const wchar_t* kZoneClass = L"easterZone_C";
constexpr const wchar_t* kBunnyClass = L"ominousBunny_C";
constexpr const wchar_t* kEggClass = L"prop_easteregg_bunnyOwn_C";

UE_FACT_FN(kRowZoneGraph, "easter_hunt", L"easterZone_C", L"ExecuteUbergraph_easterZone", Declared);
UE_FACT_FN(kRowZoneAnim, "easter_hunt", L"easterZone_C", L"playAnimation", Declared);
UE_FACT_FN(kRowFin, "easter_hunt", L"ominousBunny_C", L"fin", Declared);
UE_FACT_FN(kRowRepeat, "easter_hunt", L"ominousBunny_C", L"CustomEvent", Declared);
UE_FACT_FN(kRowUsedOn, "easter_hunt", L"ominousBunny_C", L"playerUsedOn", Declared);
UE_FACT_FN(kRowEnd, "easter_hunt", L"ominousBunny_C", L"finishTalking_eggsCollected", Declared);
constexpr int kTagGraph = 0x45485A47;   // 'EHZG'
constexpr int kTagAnim = 0x45485A41;    // 'EHZA'
constexpr int kTagFin = 0x45484649;     // 'EHFI'
constexpr int kTagRepeat = 0x45485250;  // 'EHRP'
constexpr int kTagUsedOn = 0x4548554F;  // 'EHUO'
constexpr int kTagEnd = 0x4548454E;     // 'EHEN'

constexpr int32_t kEntryMintResume = 1276;
constexpr int32_t kEntryTail = 1456;

// The bunny's talk objects, in the order of the state's bits: bit 2i says active, bit 2i+1 says shown.
constexpr const wchar_t* kTalks[] = {L"talkObject_start",   L"talkObject_repeat",     L"talkObject_noGive",
                                     L"talkObject_gotReply", L"talkObject_existsReply", L"talkObject_eggsCollected"};
constexpr size_t kTalkCount = sizeof(kTalks) / sizeof(kTalks[0]);
constexpr size_t kTalkClosing = 5;
constexpr uint8_t kFlagSpawnedEggs = 1, kFlagCollectedEggs = 2;

constexpr long long kPollMs = 500;

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_watchAsked = false;
bool g_drillCalls = false;   // [dev] see SetDrillCallsCount
Clock::time_point g_lastPoll{};

// HOST: what was last sent of its bunny.
void*    g_sentBunny = nullptr;
uint16_t g_sentTalk = 0xFFFF;
uint8_t  g_sentFlags = 0xFF;
bool     g_resend = false;

// CLIENT: a state whose copy was not there yet; the zone whose tail is owed; the flags last applied.
bool     g_havePending = false;
Payload  g_pending{};
void*    g_tailZone = nullptr;
int32_t  g_tailZoneIdx = -1;
uint8_t  g_appliedFlags = 0;

coop::net::Session* Client() {
    auto* s = g_session.load(std::memory_order_acquire);
    return (s && s->running() && s->connected() && s->role() == coop::net::Role::Client) ? s : nullptr;
}

bool IsHost() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->role() == coop::net::Role::Host;
}

struct Collect {
    std::vector<void*> found;
};

// This world's first live actor of the class. Null while the class is not loaded.
void* FirstLive(const wchar_t* className) {
    void* cls = ue_wrap::object_index::ClassByName(className);
    void* world = ue_wrap::world_identity::CurrentWorld();
    if (!cls || !world) return nullptr;
    Collect c;
    ue_wrap::object_index::ForEachInstance(cls, [](void* ctx, void* obj, int32_t) {
        if (obj) static_cast<Collect*>(ctx)->found.push_back(obj);
    }, &c);
    for (void* obj : c.found)
        if (R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__") && ue_wrap::world_identity::WorldOf(obj) == world)
            return obj;
    return nullptr;
}

bool* FlagByte(void* obj, const wchar_t* name, uint8_t& mask) {
    int32_t off = -1;
    mask = 0;
    if (!obj || !R::FindBoolProperty(R::ClassOf(obj), name, off, mask) || off < 0) return nullptr;
    return reinterpret_cast<bool*>(static_cast<uint8_t*>(obj) + off);
}

bool ReadFlag(void* obj, const wchar_t* name) {
    uint8_t mask;
    const bool* b = FlagByte(obj, name, mask);
    return b && (*reinterpret_cast<const uint8_t*>(b) & mask) != 0;
}

void WriteFlag(void* obj, const wchar_t* name, bool on) {
    uint8_t mask;
    bool* b = FlagByte(obj, name, mask);
    if (!b) return;
    uint8_t* u = reinterpret_cast<uint8_t*>(b);
    *u = on ? static_cast<uint8_t>(*u | mask) : static_cast<uint8_t>(*u & ~mask);
}

void* TalkOf(void* bunny, size_t i) {
    const int32_t off = R::FindPropertyOffset(R::ClassOf(bunny), kTalks[i]);
    if (off < 0) return nullptr;
    void* t = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(bunny) + off);
    return (t && R::IsLive(t)) ? t : nullptr;
}

bool Call0(void* obj, const wchar_t* fn) {
    void* f = obj ? R::FindDispatchFunctionCached(R::ClassOf(obj), fn) : nullptr;
    return f && R::CallFunction(obj, f, nullptr);
}

bool CallBool(void* obj, const wchar_t* fn, const wchar_t* param, bool value) {
    void* f = obj ? R::FindDispatchFunctionCached(R::ClassOf(obj), fn) : nullptr;
    ue_wrap::ParamFrame frame(f);
    return f && frame.valid() && frame.Set<bool>(param, value) && ue_wrap::Call(obj, frame);
}

// ---- the zone ----

// HOST: the animation began (the zone's own tick counted seven carrots): every client plays its own.
void OnAnimPost(const sg::Call& c) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host || (c.fromOurCode && !g_drillCalls)) return;
    coop::net::EventCuePayload p{};
    p.cueId = kCueZoneStart;
    const bool sent = s->connected() && s->SendReliable(coop::net::ReliableKind::EventCue, &p, sizeof(p));
    UE_LOGI("easter_hunt: the well's animation began on the host -- %s", sent ? "cue sent" : "no peer to send the cue to");
}

// CLIENT: the resume that mints the bunny is refused; the tail after it is owed (entered from Tick, outside this call).
sg::Verdict OnZoneGraphPre(const sg::Call& c) {
    if (!Client() || c.fromOurCode) return sg::Verdict::Run;
    static R::ParamOffset s_entry{L"EntryPoint"};
    int32_t entry = -1;
    const int32_t off = s_entry.Of(c.function);
    if (off >= 0 && c.locals) std::memcpy(&entry, c.locals + off, sizeof(entry));
    if (entry != kEntryMintResume) return sg::Verdict::Run;
    g_tailZone = c.object;
    g_tailZoneIdx = R::InternalIndexOf(c.object);
    UE_LOGI("easter_hunt: this peer's zone reached the bunny's mint -- refused (the host's bunny is sent); its tail is run instead");
    return sg::Verdict::Cancel;
}

void RunZoneTail() {
    void* zone = g_tailZone;
    const int32_t idx = g_tailZoneIdx;
    g_tailZone = nullptr;
    g_tailZoneIdx = -1;
    if (!zone || !R::IsLiveByIndex(zone, idx)) return;
    const auto graph = ue_wrap::facts::Resolve(kRowZoneGraph);
    ue_wrap::ParamFrame f(graph.Fn());
    const bool ran = graph.Fn() && f.valid() && f.Set<int32_t>(L"EntryPoint", kEntryTail) && ue_wrap::Call(zone, f);
    UE_LOGI("easter_hunt: the zone's tail ran here: %d", ran ? 1 : 0);
}

// ---- the bunny ----

void SendVerb(coop::net::Session* s, uint8_t verb, void* egg, const char* what) {
    coop::net::PropIgniteIntentPayload p{};
    p.verb = verb;
    if (egg) {
        const std::wstring keyW = ue_wrap::prop::GetInteractableKeyString(egg);
        if (!keyW.empty() && keyW != L"None") coop::net::WireKeyFromString(keyW, p.key);
        const coop::element::ElementId eid = coop::prop_element_tracker::GetPropElementIdForActor(egg);
        p.elementId = (eid == coop::element::kInvalidId) ? 0u : static_cast<uint32_t>(eid);
    }
    const bool sent = s->SendReliable(coop::net::ReliableKind::PropIgniteIntent, &p, sizeof(p));
    UE_LOGI("easter_hunt: CLIENT %s -- refused here, the host is asked: %d", what, sent ? 1 : 0);
}

sg::Verdict OnFinPre(const sg::Call& c) {
    auto* s = Client();
    if (!s || (c.fromOurCode && !g_drillCalls)) return sg::Verdict::Run;
    SendVerb(s, kVerbFin, nullptr, "the bunny's first talk ended");
    return sg::Verdict::Cancel;
}

sg::Verdict OnRepeatPre(const sg::Call& c) {
    auto* s = Client();
    if (!s || (c.fromOurCode && !g_drillCalls)) return sg::Verdict::Run;
    SendVerb(s, kVerbRepeat, nullptr, "a later talk with the bunny ended");
    return sg::Verdict::Cancel;
}

bool IsEgg(void* obj) { return obj && R::IsLive(obj) && _wcsicmp(R::ClassNameOf(obj).c_str(), kEggClass) == 0; }

// A use with no egg in hand does nothing in the game's body, so it runs; an egg's is the host's to judge.
sg::Verdict OnUsedOnPre(const sg::Call& c) {
    auto* s = Client();
    if (!s || (c.fromOurCode && !g_drillCalls)) return sg::Verdict::Run;
    static R::ParamOffset s_hold{L"HoldObject"};
    void* held = nullptr;
    const int32_t off = s_hold.Of(c.function);
    if (off >= 0 && c.locals) std::memcpy(&held, c.locals + off, sizeof(held));
    if (!IsEgg(held)) return sg::Verdict::Run;
    SendVerb(s, kVerbEgg, held, "used an egg on the bunny");
    return sg::Verdict::Cancel;
}

// The closing talk's end. HOST: it runs (the host's achievement, the explosion, the bunny's end) and every client is told.
// CLIENT: refused; its achievement comes with the host's cue.
sg::Verdict OnEndPre(const sg::Call& c) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || c.fromOurCode) return sg::Verdict::Run;
    if (s->role() == coop::net::Role::Host) {
        coop::net::EventCuePayload p{};
        p.cueId = kCueComplete;
        const bool sent = s->connected() && s->SendReliable(coop::net::ReliableKind::EventCue, &p, sizeof(p));
        UE_LOGI("easter_hunt: the hunt ended on the host -- %s", sent ? "cue sent" : "no peer to send the cue to");
        return sg::Verdict::Run;
    }
    return Client() ? sg::Verdict::Cancel : sg::Verdict::Run;
}

uint16_t TalkBits(void* bunny) {
    uint16_t bits = 0;
    for (size_t i = 0; i < kTalkCount; ++i) {
        void* t = TalkOf(bunny, i);
        if (!t) continue;
        if (ReadFlag(t, L"active")) bits |= static_cast<uint16_t>(1u << (2 * i));
        if (!ReadFlag(t, L"bHidden")) bits |= static_cast<uint16_t>(1u << (2 * i + 1));
    }
    return bits;
}

void HostPoll(coop::net::Session* s) {
    void* bunny = FirstLive(kBunnyClass);
    if (!bunny) {
        g_sentBunny = nullptr;
        return;
    }
    const uint16_t talk = TalkBits(bunny);
    const uint8_t flags = static_cast<uint8_t>((ReadFlag(bunny, L"spawnedEggs") ? kFlagSpawnedEggs : 0) |
                                               (ReadFlag(bunny, L"collectedEggs") ? kFlagCollectedEggs : 0));
    if (bunny == g_sentBunny && talk == g_sentTalk && flags == g_sentFlags && !g_resend) return;
    if (!s->connected()) return;
    // The lane's own number for it: a bunny the save loaded is enrolled here, one the lane holds answers with its row.
    const unsigned eid = coop::world_actor_sync::HostEnrollExSpawn(bunny);
    if (eid == 0) return;
    Payload p{};
    p.id = eid;
    p.family = coop::net::ambient_fixture::kEasterBunny;
    p.alive = 1;
    p._pad[0] = static_cast<uint8_t>(talk & 0xFF);
    p._pad[1] = static_cast<uint8_t>(talk >> 8);
    p._pad2[0] = flags;
    if (!s->SendReliable(coop::net::ReliableKind::AmbientFixtureState, &p, sizeof(p))) return;
    UE_LOGI("easter_hunt: HOST sent the bunny's state: talk bits 0x%03X, eggs out %d, all eggs in %d", talk,
            (flags & kFlagSpawnedEggs) ? 1 : 0, (flags & kFlagCollectedEggs) ? 1 : 0);
    g_sentBunny = bunny;
    g_sentTalk = talk;
    g_sentFlags = flags;
    g_resend = false;
}

// The copy takes the host's flags and offers the host's talk. False while the copy is not there.
bool ApplyState(const Payload& p) {
    coop::element::WorldActor* el = coop::element::WaMirrors().Get(static_cast<coop::element::ElementId>(p.id));
    void* bunny = el ? el->GetActor() : nullptr;
    if (!bunny || !R::IsLiveByIndex(bunny, el->GetInternalIdx())) return false;
    const uint16_t talk = static_cast<uint16_t>(p._pad[0] | (p._pad[1] << 8));
    const uint8_t flags = p._pad2[0];
    WriteFlag(bunny, L"spawnedEggs", (flags & kFlagSpawnedEggs) != 0);
    WriteFlag(bunny, L"collectedEggs", (flags & kFlagCollectedEggs) != 0);
    for (size_t i = 0; i < kTalkCount; ++i) {
        void* t = TalkOf(bunny, i);
        if (!t) continue;
        const bool active = (talk & (1u << (2 * i))) != 0, shown = (talk & (1u << (2 * i + 1))) != 0;
        if (ReadFlag(t, L"active") != active) CallBool(t, L"setActive", L"active", active);
        if (!ReadFlag(t, L"bHidden") != shown) E::SetActorHiddenInGame(t, !shown);
    }
    // The last egg went in: this player hears the closing talk from its own copy.
    const bool closing = (flags & kFlagCollectedEggs) && !(g_appliedFlags & kFlagCollectedEggs);
    if (closing) {
        void* t = TalkOf(bunny, kTalkClosing);
        const bool ran = t && CallBool(t, L"startTalking", L"skip", false);
        UE_LOGI("easter_hunt: every egg is in -- this peer's copy starts the closing talk: %d", ran ? 1 : 0);
    }
    UE_LOGI("easter_hunt: this peer's bunny copy took the host's state: talk bits 0x%03X, eggs out %d, all eggs in %d", talk,
            (flags & kFlagSpawnedEggs) ? 1 : 0, (flags & kFlagCollectedEggs) ? 1 : 0);
    g_appliedFlags = flags;
    return true;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_watchAsked) return;
    g_watchAsked = true;
    const bool ok = sg::WatchName(kRowZoneAnim, kTagAnim, nullptr, &OnAnimPost) &
                    sg::WatchName(kRowZoneGraph, kTagGraph, &OnZoneGraphPre, nullptr) &
                    sg::WatchName(kRowFin, kTagFin, &OnFinPre, nullptr) &
                    sg::WatchName(kRowRepeat, kTagRepeat, &OnRepeatPre, nullptr) &
                    sg::WatchName(kRowUsedOn, kTagUsedOn, &OnUsedOnPre, nullptr) &
                    sg::WatchName(kRowEnd, kTagEnd, &OnEndPre, nullptr);
    if (!ok)
        UE_LOGE("easter_hunt: a watch on the zone or the bunny did not register -- in the Easter season each peer runs a hunt "
                "of its own for that part");
}

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    if (g_tailZone) RunZoneTail();
    const bool host = s->role() == coop::net::Role::Host;
    if (!host && !g_havePending) return;
    const auto now = Clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - g_lastPoll).count() < kPollMs) return;
    g_lastPoll = now;
    if (host) HostPoll(s);
    else if (ApplyState(g_pending)) g_havePending = false;
}

bool OnCue(const coop::net::EventCuePayload& p) {
    if (p.cueId != kCueZoneStart && p.cueId != kCueComplete) return false;
    if (!Client()) return true;
    if (p.cueId == kCueZoneStart) {
        void* zone = FirstLive(kZoneClass);
        const auto anim = ue_wrap::facts::Resolve(kRowZoneAnim);
        const bool ran = zone && anim.Fn() && R::CallFunction(zone, anim.Fn(), nullptr);
        UE_LOGI("easter_hunt: the host's well went off -- this peer's zone plays its animation: %d", ran ? 1 : 0);
    } else {
        const bool granted = ue_wrap::profile::ProgressAchievement(L"eastercomplete");
        UE_LOGI("easter_hunt: the hunt ended on the host -- this player's achievement: %d", granted ? 1 : 0);
    }
    return true;
}

void OnState(const Payload& p) {
    if (!Client()) return;
    if (!coop::element::Registry::IsAllowedHostAllocatedEid(p.id)) {
        UE_LOGW("easter_hunt: a bunny state for element %u -- dropped", p.id);
        return;
    }
    if (ApplyState(p)) {
        g_havePending = false;
    } else {
        g_pending = p;   // its spawn is on the way
        g_havePending = true;
    }
}

bool TakeIntent(int senderSlot, const uint8_t* bytes, uint16_t len) {
    coop::net::PropIgniteIntentPayload p{};
    if (!bytes || len < sizeof(p)) return false;
    std::memcpy(&p, bytes, sizeof(p));
    if (p.verb != kVerbFin && p.verb != kVerbRepeat && p.verb != kVerbEgg) return false;
    if (!IsHost() || p.key.len > sizeof(p.key.data)) return true;
    void* bunny = FirstLive(kBunnyClass);
    if (!bunny) {
        UE_LOGW("easter_hunt: slot %d's verb %u found no bunny on the host -- dropped", senderSlot, p.verb);
        return true;
    }
    if (p.verb == kVerbFin) {
        // `fin` mints an egg at every point each time it runs: once a hunt.
        if (ReadFlag(bunny, L"spawnedEggs")) {
            UE_LOGI("easter_hunt: slot %d's first talk ended, the eggs are out already -- nothing to run", senderSlot);
            return true;
        }
        UE_LOGI("easter_hunt: HOST ran the bunny's fin for slot %d: %d", senderSlot, Call0(bunny, L"fin") ? 1 : 0);
    } else if (p.verb == kVerbRepeat) {
        UE_LOGI("easter_hunt: HOST ran the bunny's CustomEvent for slot %d: %d", senderSlot, Call0(bunny, L"CustomEvent") ? 1 : 0);
    } else {
        // A key resolves by the index and never falls back to the eid, which may name another prop by now.
        void* egg = p.key.len > 0 ? coop::prop_key_index::Find(coop::net::StringFromWireKey(p.key))
                                  : coop::element::LivePropActor(static_cast<coop::element::ElementId>(p.elementId));
        if (!IsEgg(egg)) {
            UE_LOGW("easter_hunt: slot %d used eid=%u on the bunny, which is no egg here -- dropped", senderSlot, p.elementId);
            return true;
        }
        void* fn = R::FindDispatchFunctionCached(R::ClassOf(bunny), L"playerUsedOn");
        ue_wrap::ParamFrame f(fn);
        const bool ran = fn && f.valid() && f.Set<void*>(L"HoldObject", egg) && ue_wrap::Call(bunny, f);
        UE_LOGI("easter_hunt: HOST ran slot %d's egg on the bunny: %d", senderSlot, ran ? 1 : 0);
    }
    return true;
}

void QueueConnectBroadcastForSlot(int /*slot*/) { g_resend = true; }

void SetDrillCallsCount(bool on) { g_drillCalls = on; }

void OnDisconnect() {
    g_sentBunny = nullptr;
    g_sentTalk = 0xFFFF;
    g_sentFlags = 0xFF;
    g_resend = false;
    g_havePending = false;
    g_tailZone = nullptr;
    g_tailZoneIdx = -1;
    g_appliedFlags = 0;
}

}  // namespace coop::easter_hunt
