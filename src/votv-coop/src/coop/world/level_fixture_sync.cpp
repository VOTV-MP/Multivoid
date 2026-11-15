// coop/world/level_fixture_sync.cpp -- see coop/world/level_fixture_sync.h. The cooked graphs this rests on (0.10.0):
//   church_C.ReceiveTick      tp() rolls a nav point; unseen and with `cantp`, K2_SetActorTransform to it, then bell().
//   teleBooth_C.timer         every 7200 s: shown, tp(), and one time in ten, unseen, moved to the rolled point.
//   sirenPost_C.ReceiveTick   unrendered for 60 s: K2_SetActorLocation along its heading.
//   hallwayDoor_C begin play  K2_DestroyActor unless the clock reads 03:33.
//   TheDoor_C                 hourly, one time in ten and unseen, moves to a rolled point; every 10-15 s, one time in ten,
//                             open() at hour 3 else close() (`movedir`). Its spheres' close-on-approach stays each copy's own.
//   rorGraf_C move()          an unseen decal re-places itself on a random wall.
//   log_C                     the Ghost Log: a roll or a near pawn fades it out, and the fade's end calls teleport().
//   prop_duc_C                every 60-300 s, unseen, moves near `orig` (the Rubber Duck).
//   prop_mannequin_C          moved by mannequinTp_C's tick while no camera renders it.
//   fprints_C                 hidden until show() (run by the old note's use); flickers while unseen.
// A client refuses those moves and rolls (spawn_authority), so only this lane places its copies; a client's show() is asked of
// the host (PropIgniteIntent, kVerbFootprints).

#include "coop/world/level_fixture_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/local_streams.h"
#include "coop/props/remote_prop.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace coop::level_fixture_sync {
namespace {

namespace E = ue_wrap::engine;
namespace R = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;
using Clock = std::chrono::steady_clock;
using Payload = coop::net::AmbientFixtureStatePayload;

UE_FACT_FN(kRowBell, "level_fixture_sync", L"church_C", L"bell", Declared);
UE_FACT_FN(kRowFprintsShow, "level_fixture_sync", L"fprints_C", L"show", Declared);
constexpr int kTagShow = 0x5348574E;  // 'SHWN'
bool g_showWatchAsked = false;

// What a row carries besides the pose and shown.
enum class Kind : uint8_t {
    Plain,   // the pose and shown only
    Door,    // plus the door's open state, in the payload's first pad byte (0 not read, 1 closed, 2 open)
    Prop,    // a physics prop of the prop lane: not read and not placed while a hand or a drive holds it
};
// The row number is on the wire as the payload's id: rows are appended, never reordered.
struct Row {
    const wchar_t* cls;
    bool rings;       // its copy rings bell() when it takes a new place
    bool existence;   // a client's own that the host did not name is destroyed
    Kind kind = Kind::Plain;
};
constexpr Row kRows[] = {
    {L"church_C",      true,  false},
    {L"teleBooth_C",   false, false},
    {L"sirenPost_C",   false, false},
    {L"hallwayDoor_C", false, true},
    {L"TheDoor_C",     false, false, Kind::Door},
    {L"rorGraf_C",     false, false},
    {L"log_C",         false, false},
    {L"prop_duc_C",    false, false, Kind::Prop},
    {L"prop_mannequin_C", false, false, Kind::Prop},
    {L"fprints_C",     false, false},
};
constexpr uint32_t kRowCount = sizeof(kRows) / sizeof(kRows[0]);
constexpr uint32_t kCensusEndId = 0xFFFFFFFFu;   // not a fixture: the host named every one it holds

constexpr long long kHostPollMs = 1000;
constexpr long long kClientPollMs = 250;
constexpr float kMovedCm = 1.f;
constexpr float kTurnedDeg = 0.5f;
constexpr float kNewPlaceCm = 1000.f;     // a move this long is a new place: the church's copy rings
constexpr float kSeenWithinS = 0.2f;      // the church's own tolerance for "seen"

std::atomic<coop::net::Session*> g_session{nullptr};
Clock::time_point g_lastPoll{};

std::string NarrowName(void* obj) {
    const std::wstring w = R::ToString(R::NameOf(obj));
    std::string s;
    s.reserve(w.size());
    for (wchar_t c : w) s.push_back(c < 128 ? static_cast<char>(c) : '?');
    return s;
}

// What names an actor across peers. A level actor: its level name, the same in every world. A prop of the prop lane (the duck, the
// mannequins) is loaded from a save and takes a generated name that differs per peer, so its interactable key names it.
std::string IdOf(const Row& row, void* actor) {
    if (row.kind == Kind::Prop) {
        const std::wstring key = ue_wrap::prop::GetInteractableKeyString(actor);
        if (!key.empty() && key != L"None") {
            std::string s;
            for (wchar_t c : key) s.push_back(c < 128 ? static_cast<char>(c) : '?');
            return s;
        }
    }
    return NarrowName(actor);
}

bool IsShown(void* actor) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!R::FindBoolProperty(R::ClassOf(actor), L"bHidden", off, mask) || off < 0) return true;
    return (*(static_cast<const uint8_t*>(actor) + off) & mask) == 0;
}

// The door's state: 1 closed, 2 open (its `movedir`), 0 when the class has none or it cannot be read.
uint8_t DoorState(void* actor) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!R::FindBoolProperty(R::ClassOf(actor), L"movedir", off, mask) || off < 0) return 0;
    return (*(static_cast<const uint8_t*>(actor) + off) & mask) != 0 ? 2 : 1;
}

// A prop a hand or a drive holds is the prop lane's: this lane neither reads nor places it.
bool PropHeld(void* actor) {
    return actor == coop::local_streams::LastHeldActor() || coop::remote_prop::IsActorUnderAnyDrive(actor);
}

struct Collect {
    void*              world;
    std::vector<void*> found;
};

// This world's live actors of the row's class. Empty while the class is not loaded.
void LiveOf(const Row& row, std::vector<void*>& out) {
    out.clear();
    void* cls = ue_wrap::object_index::ClassByName(row.cls);
    void* world = ue_wrap::world_identity::CurrentWorld();
    if (!cls || !world) return;
    Collect c{world, {}};
    ue_wrap::object_index::ForEachInstance(cls, [](void* ctx, void* obj, int32_t) {
        if (obj) static_cast<Collect*>(ctx)->found.push_back(obj);
    }, &c);
    for (void* obj : c.found) {
        if (!R::IsLive(obj) || R::NameStartsWith(R::NameOf(obj), L"Default__")) continue;
        if (ue_wrap::world_identity::WorldOf(obj) != world) continue;
        out.push_back(obj);
    }
}

// ---- HOST ----

struct Sent {
    void*       actor;
    uint32_t    row;
    std::string name;
    ue_wrap::FVector  loc;
    ue_wrap::FRotator rot;
    bool        shown;
    bool        said;   // sent at least once since the last connect
    uint8_t     state = 0;   // a door's state (DoorState)
    uint32_t    sends = 0;   // how often it was sent: a fixture that never rests is said in the log
};
std::unordered_map<int32_t, Sent> g_sent;   // by the actor's internal index
bool g_resendAll = false;
bool g_censusOwed = false;

Payload MakePayload(const Sent& s, bool alive) {
    Payload p{};
    p.id = s.row;
    p.x = s.loc.X; p.y = s.loc.Y; p.z = s.loc.Z;
    p.pitch = s.rot.Pitch; p.yaw = s.rot.Yaw; p.roll = s.rot.Roll;
    p.family = coop::net::ambient_fixture::kLevelFixture;
    p.alive = alive ? 1 : 0;
    p.hasNest = s.shown ? 1 : 0;   // this family reads the byte as "shown"
    p._pad[0] = s.state;           // and the first pad byte as a door's state (a row of kind Door)
    const size_t n = s.name.size() < sizeof(p.birthKey.data) ? s.name.size() : sizeof(p.birthKey.data);
    p.birthKey.len = static_cast<uint8_t>(n);
    std::memcpy(p.birthKey.data, s.name.data(), n);
    return p;
}

void HostPoll(coop::net::Session* s) {
    const bool connected = s->connected();
    std::vector<void*> live;
    std::vector<int32_t> seen;
    for (uint32_t r = 0; r < kRowCount; ++r) {
        LiveOf(kRows[r], live);
        for (void* actor : live) {
            ue_wrap::FVector loc{};
            ue_wrap::FRotator rot{};
            if (!E::TryGetActorLocation(actor, loc) || !E::TryGetActorRotation(actor, rot)) continue;
            const int32_t idx = R::InternalIndexOf(actor);
            seen.push_back(idx);
            const bool shown = IsShown(actor);
            const uint8_t state = kRows[r].kind == Kind::Door ? DoorState(actor) : 0;
            if (kRows[r].kind == Kind::Prop && PropHeld(actor)) continue;
            auto it = g_sent.find(idx);
            const bool fresh = it == g_sent.end() || it->second.actor != actor;
            if (fresh) {
                Sent n{actor, r, IdOf(kRows[r], actor), loc, rot, shown, false, state};
                UE_LOGI("level_fixture: the host's '%ls' %s stands at (%.0f, %.0f, %.0f), %s", kRows[r].cls, n.name.c_str(),
                        loc.X, loc.Y, loc.Z, shown ? "shown" : "hidden");
                it = g_sent.insert_or_assign(idx, std::move(n)).first;
            }
            Sent& e = it->second;
            const float dx = loc.X - e.loc.X, dy = loc.Y - e.loc.Y, dz = loc.Z - e.loc.Z;
            const bool moved = dx * dx + dy * dy + dz * dz > kMovedCm * kMovedCm ||
                               std::fabs(rot.Yaw - e.rot.Yaw) > kTurnedDeg || std::fabs(rot.Pitch - e.rot.Pitch) > kTurnedDeg ||
                               std::fabs(rot.Roll - e.rot.Roll) > kTurnedDeg || shown != e.shown || state != e.state;
            if (state != e.state && !fresh)
                UE_LOGI("level_fixture: the host's '%ls' %s is now %s", kRows[r].cls, e.name.c_str(), state == 2 ? "open" : "closed");
            if (moved && !fresh && (dx * dx + dy * dy + dz * dz > kNewPlaceCm * kNewPlaceCm || shown != e.shown))
                UE_LOGI("level_fixture: the host's '%ls' %s moved %.0f cm to (%.0f, %.0f, %.0f), %s", kRows[r].cls,
                        e.name.c_str(), std::sqrt(dx * dx + dy * dy + dz * dz), loc.X, loc.Y, loc.Z, shown ? "shown" : "hidden");
            e.loc = loc; e.rot = rot; e.shown = shown; e.state = state;
            if (!connected) { e.said = false; continue; }
            if (!moved && e.said && !g_resendAll) continue;
            const Payload p = MakePayload(e, true);
            if (s->SendReliable(coop::net::ReliableKind::AmbientFixtureState, &p, sizeof(p))) e.said = true;
            if (++e.sends >= 16 && (e.sends & (e.sends - 1)) == 0)
                UE_LOGI("level_fixture: the host's '%ls' %s has been sent %u times (it keeps moving)", kRows[r].cls, e.name.c_str(), e.sends);
        }
    }
    // One the host held and holds no more has ended: its copies end with it.
    for (auto it = g_sent.begin(); it != g_sent.end();) {
        bool still = false;
        for (int32_t idx : seen) still = still || idx == it->first;
        if (still) { ++it; continue; }
        UE_LOGI("level_fixture: the host's '%ls' %s is gone", kRows[it->second.row].cls, it->second.name.c_str());
        if (connected) {
            const Payload p = MakePayload(it->second, false);
            s->SendReliable(coop::net::ReliableKind::AmbientFixtureState, &p, sizeof(p));
        }
        it = g_sent.erase(it);
    }
    g_resendAll = false;
    if (g_censusOwed && connected) {
        Payload end{};
        end.id = kCensusEndId;
        end.family = coop::net::ambient_fixture::kLevelFixture;
        if (s->SendReliable(coop::net::ReliableKind::AmbientFixtureState, &end, sizeof(end))) {
            g_censusOwed = false;
            UE_LOGI("level_fixture: census sent -- %zu fixture(s) named", g_sent.size());
        }
    }
}

// ---- CLIENT ----

struct Want {
    uint32_t row;
    ue_wrap::FVector  loc;
    ue_wrap::FRotator rot;
    bool shown;
    bool alive;
    bool placed;     // the copy stands as the host's does
    bool heldSaid;   // "held while seen" was logged for this state
    bool said;       // this fixture's first placing was logged (short moves after it are not: the siren pole walks)
    uint8_t state;        // a door's state as the host last said it (0 none)
    uint8_t stateDone;    // the state this copy was last driven to: a copy's own slam is not undone until the host's state changes
};
std::unordered_map<std::string, Want> g_want;   // by "<row>:<level name>"
bool g_census = false;

// A door the host holds and this peer's level lost (its own begin play ran before the session refused it): one spawned here
// stands for it, under the host's name for it.
struct StandIn {
    void*   actor;
    int32_t idx;
};
std::unordered_map<std::string, StandIn> g_standIn;   // by the same key

std::string KeyOf(uint32_t row, const std::string& name) { return std::to_string(row) + ":" + name; }

// Actor.WasRecentlyRendered(Tolerance). False when it cannot be asked: the copy is then placed rather than never.
bool SeenNow(void* actor) {
    static void* s_fn = nullptr;
    if (!s_fn)
        if (void* cls = R::FindClass(L"Actor")) s_fn = R::FindFunction(cls, L"WasRecentlyRendered");
    if (!s_fn) return false;
    ue_wrap::ParamFrame f(s_fn);
    if (!f.valid() || !f.Set<float>(L"Tolerance", kSeenWithinS) || !ue_wrap::Call(actor, f)) return false;
    return f.Get<bool>(L"ReturnValue");
}

// A door's copy is driven to the host's state by the game's own open() or close(), once per change of the host's word. Run from our
// code, so the client's refusal of those two bodies (spawn_authority) lets it through. A copy something stands beside keeps
// its own slam: the sphere overlaps run close inline and open() itself looks for a body in its sphere first.
void DriveDoor(void* actor, const std::string& name, Want& w) {
    if (w.state == 0 || w.state == w.stateDone) return;
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(actor), w.state == 2 ? L"open" : L"close");
    const uint8_t was = DoorState(actor);
    const bool ran = fn && R::CallFunction(actor, fn, nullptr);
    w.stateDone = w.state;
    UE_LOGI("level_fixture: '%ls' %s follows the host: %s (the copy was %s; the call ran: %d)", kRows[w.row].cls, name.c_str(),
            w.state == 2 ? "open" : "closed", was == 2 ? "open" : "closed", ran ? 1 : 0);
}

void Ring(void* church) {
    const auto bell = ue_wrap::facts::Resolve(kRowBell);
    const bool rang = bell.Fn() && R::CallFunction(church, bell.Fn(), nullptr);
    UE_LOGI("level_fixture: the church's copy took a new place -- its bell rings: %d", rang ? 1 : 0);
}

void ClientApply() {
    std::vector<void*> live;
    for (uint32_t r = 0; r < kRowCount; ++r) {
        LiveOf(kRows[r], live);
        for (void* actor : live) {
            std::string name = IdOf(kRows[r], actor);
            for (const auto& kv : g_standIn)
                if (kv.second.actor == actor) name = kv.first.substr(kv.first.find(':') + 1);
            const auto it = g_want.find(KeyOf(r, name));
            if (it == g_want.end() || !it->second.alive) {
                // The host named no such fixture, or said it ended. Only the door's standing is the host's to decide; the
                // others stand in every world, and one the host has not named yet is left as the level placed it.
                const bool ended = it != g_want.end();
                if (ended || (kRows[r].existence && g_census)) {
                    UE_LOGI("level_fixture: this peer's '%ls' %s %s -- destroyed", kRows[r].cls, name.c_str(),
                            ended ? "ended on the host" : "does not stand on the host");
                    E::DestroyActor(actor);
                    if (ended) g_want.erase(it);
                }
                continue;
            }
            Want& w = it->second;
            if (kRows[r].kind == Kind::Door) DriveDoor(actor, name, w);
            if (w.placed) continue;
            if (kRows[r].kind == Kind::Prop && PropHeld(actor)) continue;
            ue_wrap::FVector loc{};
            if (!E::TryGetActorLocation(actor, loc)) continue;
            const float dx = w.loc.X - loc.X, dy = w.loc.Y - loc.Y, dz = w.loc.Z - loc.Z;
            const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            // A copy under this player's eyes waits, as the game moves these only unseen. Showing one is no move.
            if (dist > kMovedCm && IsShown(actor) && SeenNow(actor)) {
                if (!w.heldSaid)
                    UE_LOGI("level_fixture: '%ls' %s is in view -- its move of %.0f cm waits until it is not", kRows[r].cls,
                            name.c_str(), dist);
                w.heldSaid = true;
                continue;
            }
            const bool wasShown = IsShown(actor);
            E::SetActorLocation(actor, w.loc);
            E::SetActorRotation(actor, w.rot);
            if (wasShown != w.shown) E::SetActorHiddenInGame(actor, !w.shown);
            w.placed = true;
            if (!w.said || dist > kNewPlaceCm || wasShown != w.shown)
                UE_LOGI("level_fixture: '%ls' %s follows the host: %.0f cm to (%.0f, %.0f, %.0f), %s", kRows[r].cls,
                        name.c_str(), dist, w.loc.X, w.loc.Y, w.loc.Z, w.shown ? "shown" : "hidden");
            w.said = true;
            if (kRows[r].rings && dist > kNewPlaceCm) Ring(actor);
        }
        if (!kRows[r].existence || !g_census) continue;
        // Named alive by the host and standing nowhere here: a stand-in at the host's place.
        for (auto& kv : g_want) {
            Want& w = kv.second;
            if (w.row != r || !w.alive) continue;
            const std::string name = kv.first.substr(kv.first.find(':') + 1);
            bool stands = false;
            for (void* actor : live) stands = stands || IdOf(kRows[r], actor) == name;
            const auto si = g_standIn.find(kv.first);
            if (si != g_standIn.end() && R::IsLiveByIndex(si->second.actor, si->second.idx)) stands = true;
            if (stands) continue;
            void* cls = R::FindClass(kRows[r].cls);
            void* made = cls ? E::BeginDeferredSpawn(cls, w.loc, w.rot) : nullptr;
            if (made && !E::FinishDeferredSpawn(made, w.loc, w.rot)) made = nullptr;
            UE_LOGI("level_fixture: the host's '%ls' %s stands nowhere here -- a stand-in spawned: %d", kRows[r].cls,
                    name.c_str(), made ? 1 : 0);
            if (!made) { w.alive = false; continue; }   // not tried again every pass
            if (!w.shown) E::SetActorHiddenInGame(made, true);
            g_standIn[kv.first] = {made, R::InternalIndexOf(made)};
            w.placed = true;
        }
    }
}

// A client's use of the old note runs fprints_C::show on its own copy of the footprints. The footprints are the host's: the call is
// refused here and asked of the host, whose own show() unhides its copy and reaches every peer through the shown byte.
sg::Verdict OnShowPre(const sg::Call& /*c*/) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || !s->connected() || s->role() != coop::net::Role::Client) return sg::Verdict::Run;
    coop::net::PropIgniteIntentPayload p{};
    p.verb = kVerbFootprints;
    const bool sent = s->SendReliable(coop::net::ReliableKind::PropIgniteIntent, &p, sizeof(p));
    UE_LOGI("level_fixture: CLIENT's footprints show() -- refused here, the host is asked to run it: %d", sent ? 1 : 0);
    return sg::Verdict::Cancel;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_showWatchAsked) return;
    g_showWatchAsked = true;
    if (!sg::WatchName(kRowFprintsShow, kTagShow, &OnShowPre, nullptr))
        UE_LOGE("level_fixture: the watch on fprints_C::show did not register -- a client's use of the note shows its own copy only");
}

bool TakeIntent(int senderSlot, const uint8_t* bytes, uint16_t len) {
    coop::net::PropIgniteIntentPayload p{};
    if (!bytes || len < sizeof(p)) return false;
    std::memcpy(&p, bytes, sizeof(p));
    if (p.verb != kVerbFootprints) return false;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return true;
    int ran = 0, held = 0;
    std::vector<void*> live;
    LiveOf({L"fprints_C", false, false}, live);
    for (void* a : live) {
        ++held;
        void* fn = R::FindDispatchFunctionCached(R::ClassOf(a), L"show");
        if (fn && R::CallFunction(a, fn, nullptr)) ++ran;
    }
    UE_LOGI("level_fixture: HOST ran slot %d's footprints show() on %d of %d copy(ies)", senderSlot, ran, held);
    return true;
}

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    const bool host = s->role() == coop::net::Role::Host;
    if (!host && (!s->connected() || g_want.empty()) && !g_census) return;
    const auto now = Clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - g_lastPoll).count() < (host ? kHostPollMs : kClientPollMs))
        return;
    g_lastPoll = now;
    if (host) HostPoll(s);
    else ClientApply();
}

void OnState(const Payload& p) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() == coop::net::Role::Host) return;
    if (p.id == kCensusEndId) {
        g_census = true;
        UE_LOGI("level_fixture: the host's census ended -- %zu fixture(s) named", g_want.size());
        return;
    }
    if (p.id >= kRowCount || p.birthKey.len == 0) {
        UE_LOGW("level_fixture: a fixture of row %u with a name of %u char(s) -- dropped", p.id, p.birthKey.len);
        return;
    }
    if (std::fabs(p.x) > coop::net::kMaxCoord || std::fabs(p.y) > coop::net::kMaxCoord || std::fabs(p.z) > coop::net::kMaxCoord) {
        UE_LOGW("level_fixture: a fixture placed outside the world -- dropped");
        return;
    }
    Want w{};
    w.row = p.id;
    w.loc = {p.x, p.y, p.z};
    w.rot.Pitch = p.pitch; w.rot.Yaw = p.yaw; w.rot.Roll = p.roll;
    w.shown = p.hasNest != 0;
    w.alive = p.alive != 0;
    w.state = kRows[p.id].kind == Kind::Door ? p._pad[0] : 0;
    const std::string key = KeyOf(p.id, std::string(p.birthKey.data, p.birthKey.len));
    // A prop's life is the prop lane's (a pick-up ends the host's actor, and the same key stands again at the drop): its end
    // here only forgets where it stood, and never destroys a copy.
    if (kRows[p.id].kind == Kind::Prop && !w.alive) {
        g_want.erase(key);
        return;
    }
    const auto had = g_want.find(key);
    w.said = had != g_want.end() && had->second.said;
    w.stateDone = had != g_want.end() ? had->second.stateDone : 0;
    g_want[key] = w;
}

void QueueConnectBroadcastForSlot(int /*slot*/) {
    // Every client is sent the lot again: a fixture's state is small and applying it twice changes nothing.
    g_resendAll = true;
    g_censusOwed = true;
}

void OnDisconnect() {
    g_want.clear();
    g_standIn.clear();
    g_census = false;
    g_resendAll = false;
    g_censusOwed = false;
    for (auto& kv : g_sent) kv.second.said = false;
}

}  // namespace coop::level_fixture_sync
