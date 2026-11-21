// coop/interactables/car_charger_sync.cpp -- see coop/interactables/car_charger_sync.h.

#include "coop/interactables/car_charger_sync.h"

#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/facts/function_row.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace coop::car_charger_sync {
namespace {

namespace N  = coop::net;
namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace OI = ue_wrap::object_index;
namespace sg = ue_wrap::script_gate;
using ue_wrap::FVector;

UE_FACT_FN(kRowTick, "car_charger_sync", nullptr, L"ReceiveTick", AnyClass);
constexpr int kTag = 0x43434847;   // 'CCHG'

constexpr uint64_t kPollMs = 250;        // the host reads, and a client compares, this often
constexpr uint64_t kChargeGapMs = 2000;  // a moving charge is told at most this often
constexpr float kChargeEps = 0.5f;       // once it has moved this much
constexpr float kChargeJump = 5.f;       // and at once past this
constexpr uint64_t kParkMs = 10000;      // a row for an actor not bound here yet waits this long
constexpr float kReachCm = 400.f;        // a client's clip move names a charger this near its player

std::atomic<N::Session*> g_session{nullptr};

uint64_t Now() { return ::GetTickCount64(); }

bool IsClient() {
    N::Session* s = g_session.load(std::memory_order_acquire);
    return s && s->running() && s->role() != N::Role::Host;
}

struct Bool {
    int32_t off = -1;
    uint8_t mask = 0;
};

// The classes and fields, per world.
struct Layout {
    uint32_t gen = 0xFFFFFFFFu;
    void* charger = nullptr;
    void* battery = nullptr;
    int32_t red = -1, blue = -1;     // the charger's carBatteryRed, carBatteryBlue
    Bool redToRed, blueToRed, done, plugged;  // pluggedRedToRed, pluggedBlueToRed, doneCharging, plugged (its cord)
    int32_t charge = -1;             // the battery's battery
    Bool pinRed, pinBlue;            // plugged_red, plugged_blue
};
Layout g_l;

Bool BoolOf(void* cls, const wchar_t* name) {
    Bool b;
    if (!R::FindBoolProperty(cls, name, b.off, b.mask)) b.off = -1;
    return b;
}

void Refresh() {
    const uint32_t gen = ue_wrap::world_identity::Generation();
    if (gen != g_l.gen) g_l = Layout{gen};
    if (!g_l.charger && (g_l.charger = OI::ClassByName(L"prop_carBatteryCharger_C"))) {
        g_l.red = R::FindPropertyOffset(g_l.charger, L"carBatteryRed");
        g_l.blue = R::FindPropertyOffset(g_l.charger, L"carBatteryBlue");
        g_l.redToRed = BoolOf(g_l.charger, L"pluggedRedToRed");
        g_l.blueToRed = BoolOf(g_l.charger, L"pluggedBlueToRed");
        g_l.done = BoolOf(g_l.charger, L"doneCharging");
        g_l.plugged = BoolOf(g_l.charger, L"plugged");
    }
    if (!g_l.battery && (g_l.battery = OI::ClassByName(L"prop_atvcarbattery_C"))) {
        g_l.charge = R::FindPropertyOffset(g_l.battery, L"battery");
        g_l.pinRed = BoolOf(g_l.battery, L"plugged_red");
        g_l.pinBlue = BoolOf(g_l.battery, L"plugged_blue");
    }
}

bool ChargerReady() {
    return g_l.charger && g_l.red >= 0 && g_l.blue >= 0 && g_l.redToRed.off >= 0 && g_l.blueToRed.off >= 0 &&
           g_l.done.off >= 0 && g_l.plugged.off >= 0;
}
bool BatteryReady() { return g_l.battery && g_l.charge >= 0 && g_l.pinRed.off >= 0 && g_l.pinBlue.off >= 0; }

bool Of(void* obj, void* base) {
    return obj && base && R::IsDescendantOfAny(R::ClassOf(obj), &base, 1, 8);
}

bool GetB(void* obj, const Bool& b) { return (static_cast<const uint8_t*>(obj)[b.off] & b.mask) != 0; }
void SetB(void* obj, const Bool& b, bool v) {
    uint8_t& x = static_cast<uint8_t*>(obj)[b.off];
    x = v ? static_cast<uint8_t>(x | b.mask) : static_cast<uint8_t>(x & ~b.mask);
}
void*& Ref(void* obj, int32_t off) { return *reinterpret_cast<void**>(static_cast<uint8_t*>(obj) + off); }
float& Charge(void* bat) { return *reinterpret_cast<float*>(static_cast<uint8_t*>(bat) + g_l.charge); }

uint32_t EidOf(void* actor) {
    if (!actor || !R::IsLive(actor)) return 0u;
    const coop::element::ElementId e = coop::element::Registry::Get().EidForActor(actor);
    return e == coop::element::kInvalidId ? 0u : static_cast<uint32_t>(e);
}
void* ActorOf(uint32_t eid) {
    return eid ? coop::element::LivePropActor(static_cast<coop::element::ElementId>(eid)) : nullptr;
}

std::vector<void*> All(void* cls) {
    std::vector<void*> out;
    if (cls)
        OI::ForEachInstance(cls, [](void* ctx, void* obj, int32_t) {
            if (obj && R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__"))
                static_cast<std::vector<void*>*>(ctx)->push_back(obj);
        }, &out);
    return out;
}

N::CarChargerRow ReadCharger(void* ch, uint32_t eid) {
    N::CarChargerRow r{};
    r.eid = eid;
    r.red = EidOf(Ref(ch, g_l.red));
    r.blue = EidOf(Ref(ch, g_l.blue));
    r.flags = static_cast<uint8_t>((GetB(ch, g_l.redToRed) ? N::kCarClipRedToRed : 0) |
                                   (GetB(ch, g_l.blueToRed) ? N::kCarClipBlueToRed : 0) |
                                   (GetB(ch, g_l.done) ? N::kCarChargeDone : 0) |
                                   (GetB(ch, g_l.plugged) ? N::kCarChargerPlugged : 0));
    return r;
}

bool SameClips(const N::CarChargerRow& a, const N::CarChargerRow& b) {
    const uint8_t clips = N::kCarClipRedToRed | N::kCarClipBlueToRed;
    return a.red == b.red && a.blue == b.blue && (a.flags & clips) == (b.flags & clips);
}

N::CarBatteryRow ReadBattery(void* bat, uint32_t eid) {
    N::CarBatteryRow r{};
    r.eid = eid;
    r.charge = std::clamp(Charge(bat), 0.f, 100.f);
    r.pins = static_cast<uint8_t>((GetB(bat, g_l.pinRed) ? N::kCarPinRed : 0) | (GetB(bat, g_l.pinBlue) ? N::kCarPinBlue : 0));
    return r;
}

bool CallNoArgs(void* obj, const wchar_t* fn) {
    void* const f = obj ? R::FindDispatchFunctionCached(R::ClassOf(obj), fn) : nullptr;
    ue_wrap::ParamFrame frame(f);
    return f && frame.valid() && ue_wrap::Call(obj, frame);
}

// A charger's clips as `r` names them, the done flag with them, its bulb redrawn.
void WriteCharger(void* ch, const N::CarChargerRow& r) {
    Ref(ch, g_l.red) = ActorOf(r.red);
    Ref(ch, g_l.blue) = ActorOf(r.blue);
    SetB(ch, g_l.redToRed, (r.flags & N::kCarClipRedToRed) != 0);
    SetB(ch, g_l.blueToRed, (r.flags & N::kCarClipBlueToRed) != 0);
    SetB(ch, g_l.done, (r.flags & N::kCarChargeDone) != 0);
    SetB(ch, g_l.plugged, (r.flags & N::kCarChargerPlugged) != 0);   // the host's cord, which charges
    CallNoArgs(ch, L"setBulb");
}

// ---------------------------------------------------------------- HOST

struct SentCharger {
    N::CarChargerRow row{};
    bool any = false;
};
struct SentBattery {
    N::CarBatteryRow row{};
    uint64_t at = 0;
    bool any = false;
};
std::unordered_map<void*, SentCharger> g_sentCh;
std::unordered_map<void*, SentBattery> g_sentBat;
uint64_t g_nextPoll = 0;
uint32_t g_rowsSent = 0, g_edits = 0, g_refused = 0;

void Send(N::Session& s, const std::vector<N::CarChargerRow>& ch, const std::vector<N::CarBatteryRow>& bat, int slot) {
    size_t ci = 0, bi = 0;
    while (ci < ch.size() || bi < bat.size()) {
        N::CarChargerStatePayload p{};
        while (ci < ch.size() && p.chargers < N::kCarChargerRowsMax) p.charger[p.chargers++] = ch[ci++];
        while (bi < bat.size() && p.batteries < N::kCarBatteryRowsMax) p.battery[p.batteries++] = bat[bi++];
        const bool ok = slot < 0 ? s.SendReliable(N::ReliableKind::CarChargerState, &p, sizeof(p))
                                 : s.SendReliableToSlot(slot, N::ReliableKind::CarChargerState, &p, sizeof(p));
        if (ok) g_rowsSent += p.chargers + p.batteries;
        else UE_LOGW("car_charger_sync: %u charger and %u battery row(s) were not sent", p.chargers, p.batteries);
    }
}

// Each battery's pins as the host's chargers' clips leave them: a clip names the pin it is on by its polarity.
void SetPins() {
    if (!BatteryReady() || !ChargerReady()) return;
    std::unordered_map<void*, uint8_t> pins;
    for (void* bat : All(g_l.battery)) pins[bat] = 0;
    for (void* ch : All(g_l.charger)) {
        void* const red = Ref(ch, g_l.red);
        void* const blue = Ref(ch, g_l.blue);
        if (red && pins.count(red)) pins[red] |= GetB(ch, g_l.redToRed) ? N::kCarPinRed : N::kCarPinBlue;
        if (blue && pins.count(blue)) pins[blue] |= GetB(ch, g_l.blueToRed) ? N::kCarPinRed : N::kCarPinBlue;
    }
    for (const auto& e : pins) {
        SetB(e.first, g_l.pinRed, (e.second & N::kCarPinRed) != 0);
        SetB(e.first, g_l.pinBlue, (e.second & N::kCarPinBlue) != 0);
    }
}

void HostTick(N::Session& s, bool force) {
    const uint64_t t = Now();
    if (!force && t < g_nextPoll) return;
    g_nextPoll = t + kPollMs;
    if (!s.AnyWorldReadyPeer()) return;
    std::vector<N::CarChargerRow> chs;
    std::vector<N::CarBatteryRow> bats;
    if (ChargerReady())
        for (void* ch : All(g_l.charger)) {
            const uint32_t eid = EidOf(ch);
            if (!eid) continue;
            const N::CarChargerRow now = ReadCharger(ch, eid);
            SentCharger& sent = g_sentCh[ch];
            if (sent.any && sent.row.eid == eid && SameClips(sent.row, now) && sent.row.flags == now.flags) continue;
            sent.row = now;
            sent.any = true;
            chs.push_back(now);
        }
    if (BatteryReady())
        for (void* bat : All(g_l.battery)) {
            const uint32_t eid = EidOf(bat);
            if (!eid) continue;
            const N::CarBatteryRow now = ReadBattery(bat, eid);
            SentBattery& sent = g_sentBat[bat];
            const float moved = sent.any ? std::fabs(sent.row.charge - now.charge) : 0.f;
            const bool due = !sent.any || sent.row.eid != eid || sent.row.pins != now.pins || moved >= kChargeJump ||
                             (moved >= kChargeEps && t - sent.at >= kChargeGapMs);
            if (!due) continue;
            sent.row = now;
            sent.at = t;
            sent.any = true;
            bats.push_back(now);
        }
    if (!chs.empty() || !bats.empty()) Send(s, chs, bats, -1);
    if (g_sentBat.size() > 128)
        for (auto it = g_sentBat.begin(); it != g_sentBat.end();) it = R::IsLive(it->first) ? std::next(it) : g_sentBat.erase(it);
}

bool InReach(int slot, void* actor) {
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(static_cast<uint8_t>(slot));
    FVector p{}, a{};
    if (!rp || !rp->valid() || !rp->TryGetLocation(p) || !E::TryGetActorLocation(actor, a)) return false;
    const float x = p.X - a.X, y = p.Y - a.Y, z = p.Z - a.Z;
    return x * x + y * y + z * z <= kReachCm * kReachCm;
}

// ---------------------------------------------------------------- CLIENT

// What the host last told of each charger, by eid: a client's own copy differing from it is this player's clip move.
std::unordered_map<uint32_t, N::CarChargerRow> g_told;
struct ParkedCharger {
    N::CarChargerRow row;
    uint64_t until;
};
struct ParkedBattery {
    N::CarBatteryRow row;
    uint64_t until;
};
std::unordered_map<uint32_t, ParkedCharger> g_parkedCh;
std::unordered_map<uint32_t, ParkedBattery> g_parkedBat;
uint64_t g_nextLook = 0;
uint32_t g_applied = 0, g_asked = 0, g_replugs = 0;

bool ApplyCharger(const N::CarChargerRow& r) {
    void* const ch = ActorOf(r.eid);
    if (!ch || !Of(ch, g_l.charger) || (r.red && !ActorOf(r.red)) || (r.blue && !ActorOf(r.blue))) return false;
    WriteCharger(ch, r);
    g_told[r.eid] = r;
    ++g_applied;
    return true;
}

bool ApplyBattery(const N::CarBatteryRow& r) {
    void* const bat = ActorOf(r.eid);
    if (!bat || !Of(bat, g_l.battery)) return false;
    Charge(bat) = r.charge;
    SetB(bat, g_l.pinRed, (r.pins & N::kCarPinRed) != 0);
    SetB(bat, g_l.pinBlue, (r.pins & N::kCarPinBlue) != 0);
    ++g_applied;
    return true;
}

// This player's clip moves: each charger whose clips differ from the host's last row, as its used event or drag left
// them here.
void ClientLook(N::Session& s) {
    const uint64_t t = Now();
    if (t < g_nextLook || !ChargerReady()) return;
    g_nextLook = t + kPollMs;
    for (void* ch : All(g_l.charger)) {
        const uint32_t eid = EidOf(ch);
        const auto told = g_told.find(eid);
        if (!eid || told == g_told.end()) continue;   // nothing told yet: the host's first row primes it
        const N::CarChargerRow now = ReadCharger(ch, eid);
        // Its cord is the host's: a local plug or unplug (this copy's own cord event) is put back, never asked.
        if ((now.flags ^ told->second.flags) & N::kCarChargerPlugged) {
            SetB(ch, g_l.plugged, (told->second.flags & N::kCarChargerPlugged) != 0);
            CallNoArgs(ch, L"setBulb");
            ++g_replugs;
            if (g_replugs == 1 || g_replugs % 100 == 0)
                UE_LOGI("car_charger_sync: car charger eid=%u's cord read %s here -- the host's %s put back (#%u)", eid,
                        (now.flags & N::kCarChargerPlugged) ? "in" : "out",
                        (told->second.flags & N::kCarChargerPlugged) ? "in" : "out", g_replugs);
        }
        if (SameClips(told->second, now)) continue;
        N::CarChargerIntentPayload p{};
        p.row = now;
        if (s.SendReliable(N::ReliableKind::CarChargerIntent, &p, sizeof(p))) ++g_asked;
        // Asked once; the host's row answers. Only the clips are this player's: the cord and done stay the host's.
        const uint8_t clips = N::kCarClipRedToRed | N::kCarClipBlueToRed;
        told->second.red = now.red;
        told->second.blue = now.blue;
        told->second.flags = static_cast<uint8_t>((now.flags & clips) | (told->second.flags & ~clips));
        UE_LOGI("car_charger_sync: this player moved a clip of car charger eid=%u (red on %u, blue on %u, flags %u) -- "
                "told the host", eid, now.red, now.blue, static_cast<unsigned>(now.flags));
    }
}

// ---------------------------------------------------------------- the watch

// A client's car charger runs no charge or spark of its own: the host's rows carry them.
sg::Verdict OnTickPre(const sg::Call& c) {
    if (!c.object || !IsClient() || !Of(c.object, g_l.charger)) return sg::Verdict::Run;
    return sg::Verdict::Cancel;
}

bool g_watchRegistered = false, g_watchSettled = false;

void SettleWatch() {
    if (g_watchSettled) return;
    if (!g_watchRegistered) {
        g_watchRegistered = true;
        if (!sg::WatchName(kRowTick, kTag, &OnTickPre, nullptr)) {
            g_watchSettled = true;
            UE_LOGE("car_charger_sync: the gate took no watch on %ls -- a client's car chargers charge on their own",
                    kRowTick.Name());
            return;
        }
    }
    sg::ResolvePendingNames();
    if (!sg::NameWatchSettled(kRowTick, kTag)) return;
    g_watchSettled = true;
    if (sg::NameWatchLive(kRowTick, kTag)) UE_LOGI("car_charger_sync: the watch on %ls is live", kRowTick.Name());
    else UE_LOGE("car_charger_sync: the watch settled dead -- a client's car chargers charge on their own");
}

}  // namespace

void Install(coop::net::Session* session) { g_session.store(session, std::memory_order_release); }

void Tick() {
    N::Session* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || !s->connected()) return;
    SettleWatch();
    Refresh();
    if (s->role() == N::Role::Host) return HostTick(*s, false);
    const uint64_t t = Now();
    for (auto it = g_parkedCh.begin(); it != g_parkedCh.end();)
        it = (ApplyCharger(it->second.row) || t >= it->second.until) ? g_parkedCh.erase(it) : std::next(it);
    for (auto it = g_parkedBat.begin(); it != g_parkedBat.end();)
        it = (ApplyBattery(it->second.row) || t >= it->second.until) ? g_parkedBat.erase(it) : std::next(it);
    ClientLook(*s);
}

void OnPeerWorldReady(int slot) {
    N::Session* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != N::Role::Host) return;
    Refresh();
    std::vector<N::CarChargerRow> chs;
    std::vector<N::CarBatteryRow> bats;
    if (ChargerReady())
        for (void* ch : All(g_l.charger))
            if (const uint32_t eid = EidOf(ch)) chs.push_back(ReadCharger(ch, eid));
    if (BatteryReady())
        for (void* bat : All(g_l.battery))
            if (const uint32_t eid = EidOf(bat)) bats.push_back(ReadBattery(bat, eid));
    if (!chs.empty() || !bats.empty()) Send(*s, chs, bats, slot);
    UE_LOGI("car_charger_sync: slot %d's world is up -- %zu car charger(s) and %zu loose battery(ies) sent to it", slot,
            chs.size(), bats.size());
}

void OnIntent(int slot, const coop::net::CarChargerIntentPayload& p) {
    N::Session* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != N::Role::Host) return;
    Refresh();
    void* const ch = ActorOf(p.row.eid);
    void* const red = ActorOf(p.row.red);
    void* const blue = ActorOf(p.row.blue);
    const char* why = nullptr;
    if (!ChargerReady() || !BatteryReady()) why = "the classes' fields did not resolve";
    else if (!ch || !Of(ch, g_l.charger)) why = "the host holds no such car charger";
    else if (!InReach(slot, ch)) why = "out of the sender's reach";
    else if ((p.row.red && !Of(red, g_l.battery)) || (p.row.blue && !Of(blue, g_l.battery)))
        why = "a clip names no ATV battery the host holds";
    if (why) {
        ++g_refused;
        UE_LOGI("car_charger_sync: slot %d's clip move on car charger eid=%u refused: %s", slot, p.row.eid, why);
        if (ch) g_sentCh.erase(ch);   // its own copy moved the clip: the host's row is owed to it again
        HostTick(*s, true);
        return;
    }
    N::CarChargerRow r = p.row;
    r.flags = static_cast<uint8_t>((r.flags & (N::kCarClipRedToRed | N::kCarClipBlueToRed)) |
                                   (GetB(ch, g_l.done) ? N::kCarChargeDone : 0) |
                                   (GetB(ch, g_l.plugged) ? N::kCarChargerPlugged : 0));
    WriteCharger(ch, r);
    SetPins();
    ++g_edits;
    UE_LOGI("car_charger_sync: slot %d's clips on car charger eid=%u set here (red on %u, blue on %u, flags %u)", slot,
            p.row.eid, p.row.red, p.row.blue, static_cast<unsigned>(r.flags));
    HostTick(*s, true);
}

void OnState(const coop::net::CarChargerStatePayload& p) {
    if (!IsClient()) return;
    Refresh();
    if (ChargerReady())
        for (int i = 0; i < p.chargers; ++i)
            if (!ApplyCharger(p.charger[i])) g_parkedCh[p.charger[i].eid] = ParkedCharger{p.charger[i], Now() + kParkMs};
    if (BatteryReady())
        for (int i = 0; i < p.batteries; ++i)
            if (!ApplyBattery(p.battery[i])) g_parkedBat[p.battery[i].eid] = ParkedBattery{p.battery[i], Now() + kParkMs};
}

void OnDisconnect() {
    if (g_rowsSent || g_applied || g_asked)
        UE_LOGI("car_charger_sync: session end -- host rows sent %u, clip edits %u, refused %u; client rows applied %u, "
                "asks %u", g_rowsSent, g_edits, g_refused, g_applied, g_asked);
    g_sentCh.clear();
    g_sentBat.clear();
    g_told.clear();
    g_parkedCh.clear();
    g_parkedBat.clear();
    g_nextPoll = g_nextLook = 0;
    g_rowsSent = g_edits = g_refused = g_applied = g_asked = g_replugs = 0;
}

}  // namespace coop::car_charger_sync
