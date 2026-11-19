// coop/creatures/kerfur_color_sync.cpp -- a custom-colour kerfur's colour, host-owned. See the header.

#include "coop/creatures/kerfur_color_sync.h"

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
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/facts/function_row.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace coop::kerfur_color_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace N  = coop::net;
namespace OI = ue_wrap::object_index;
namespace sg = ue_wrap::script_gate;
using ue_wrap::FVector;

std::atomic<N::Session*> g_session{nullptr};

// The picker's apply, on the two props that offer it (the Omega takes its colour from its prop at turn-on).
UE_FACT_FN(kRowApplyOmega, "kerfur_color_sync", L"prop_kerfurOmega_col_C", L"intComs_applyColor", Declared);
UE_FACT_FN(kRowApplyKerfus, "kerfur_color_sync", L"p_kerfus_col_C", L"intComs_applyColor", Declared);
constexpr int kTag = 0x4B4C0001;  // 'KL' 1

constexpr uint64_t kParkMs = 120000;  // a colour for an actor not bound here yet (a joiner's snapshot is paced)
constexpr uint64_t kAskGapMs = 50;    // a client's picks on one actor, coalesced: the picker may apply as it drags
constexpr float kReachCm = 800.f;     // the picker stays open while its player steps about

// Each coloured class and the function of its own that paints `hsv` onto its mesh (its bool parameter left false).
struct Painted {
    const wchar_t* cls;
    const wchar_t* paint;
    void* resolved;
};
Painted g_classes[] = {
    {L"prop_kerfurOmega_col_C", L"setColor", nullptr},
    {L"kerfurOmega_col_C", L"SetStyle", nullptr},
    {L"p_kerfus_col_C", L"upd", nullptr},
};

uint64_t Now() { return ::GetTickCount64(); }

void ResolveClasses() {
    static uint64_t next = 0;
    if (Now() < next) return;
    next = Now() + 1000;
    for (Painted& p : g_classes)
        if (!p.resolved) p.resolved = OI::ClassByName(p.cls);
}

const Painted* PaintedOf(void* actor) {
    void* const cls = actor ? R::ClassOf(actor) : nullptr;
    if (!cls) return nullptr;
    for (const Painted& p : g_classes)
        if (p.resolved && cls == p.resolved) return &p;
    return nullptr;
}

uint32_t EidOf(void* actor) {
    const coop::element::ElementId e = coop::element::Registry::Get().EidForActor(actor);
    return (e == coop::element::kInvalidId) ? 0u : static_cast<uint32_t>(e);
}

void* ActorOf(uint32_t eid) {
    coop::element::Element* el = coop::element::Registry::Get().Get(static_cast<coop::element::ElementId>(eid));
    void* actor = el ? el->GetActor() : nullptr;
    if (!actor || !R::IsLiveByIndex(actor, el->GetInternalIdx())) return nullptr;
    return actor;
}

bool ReadHsv(void* actor, float out[3]) {
    const int32_t off = R::FindPropertyOffset(R::ClassOf(actor), L"hsv");
    if (off < 0) return false;
    std::memcpy(out, static_cast<const char*>(actor) + off, sizeof(float) * 3);
    return true;
}

bool IsHost() {
    N::Session* s = g_session.load(std::memory_order_acquire);
    return s && s->running() && s->role() == N::Role::Host;
}
bool IsClient() {
    N::Session* s = g_session.load(std::memory_order_acquire);
    return s && s->running() && s->role() == N::Role::Client;
}

bool MakePayload(void* actor, N::KerfurColorPayload& p) {
    if (!PaintedOf(actor)) return false;
    p = N::KerfurColorPayload{};
    p.eid = EidOf(actor);
    return p.eid && ReadHsv(actor, p.hsv);
}

// ---- the client --------------------------------------------------------------------------------

struct Ask {
    N::KerfurColorIntentPayload p;
    uint64_t lastSent = 0;
    bool due = false;
};
std::unordered_map<uint32_t, Ask> g_asks;
struct Parked {
    N::KerfurColorPayload p;
    uint64_t until;
};
std::unordered_map<uint32_t, Parked> g_parked;
uint64_t g_applied = 0, g_asked = 0;

bool Apply(const N::KerfurColorPayload& p) {
    void* const actor = ActorOf(p.eid);
    const Painted* const k = PaintedOf(actor);
    const int32_t off = k ? R::FindPropertyOffset(R::ClassOf(actor), L"hsv") : -1;
    if (off < 0) return false;
    std::memcpy(static_cast<char*>(actor) + off, p.hsv, sizeof(p.hsv));
    void* const fn = R::FindDispatchFunctionCached(R::ClassOf(actor), k->paint);
    ue_wrap::ParamFrame f(fn);
    const bool painted = fn && f.valid() && ue_wrap::Call(actor, f);
    ++g_applied;
    UE_LOGI("kerfur_color_sync: eid=%u (%ls) takes the host's colour hsv=(%.1f,%.2f,%.2f), painted=%d", p.eid, k->cls,
            p.hsv[0], p.hsv[1], p.hsv[2], painted ? 1 : 0);
    return true;
}

// A client's pick: refused on its copy, and asked of the host. A prop the host does not know is the client's alone.
sg::Verdict OnApplyPre(const sg::Call& c) {
    if (!IsClient() || !c.object || !PaintedOf(c.object)) return sg::Verdict::Run;
    const uint32_t eid = EidOf(c.object);
    const int32_t off = c.locals ? R::FindParamOffset(c.function, L"color") : -1;
    if (!eid || off < 0) return sg::Verdict::Run;
    Ask& a = g_asks[eid];
    a.p.eid = eid;
    std::memcpy(a.p.color, c.locals + off, sizeof(a.p.color));
    a.due = true;
    return sg::Verdict::Cancel;
}

void ClientTick(N::Session& s) {
    const uint64_t now = Now();
    for (auto& [eid, a] : g_asks) {
        if (!a.due || now - a.lastSent < kAskGapMs) continue;
        if (!s.SendReliable(N::ReliableKind::KerfurColorIntent, &a.p, sizeof(a.p))) continue;
        a.due = false;
        a.lastSent = now;
        ++g_asked;
        UE_LOGI("kerfur_color_sync: this client picked (%.2f,%.2f,%.2f) for eid=%u -- asked of the host", a.p.color[0],
                a.p.color[1], a.p.color[2], eid);
    }
    for (auto it = g_parked.begin(); it != g_parked.end();) {
        if (now >= it->second.until || Apply(it->second.p)) it = g_parked.erase(it);
        else ++it;
    }
}

// ---- the host ----------------------------------------------------------------------------------

uint64_t g_announced = 0, g_intentsRun = 0, g_intentsRefused = 0;

void SendTo(int slot, const N::KerfurColorPayload& p) {
    N::Session* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    if (slot < 0) s->SendReliable(N::ReliableKind::KerfurColorState, &p, sizeof(p));
    else s->SendReliableToSlot(slot, N::ReliableKind::KerfurColorState, &p, sizeof(p));
}

void OnApplyPost(const sg::Call& c) {
    if (IsHost() && c.object) HostAnnounce(c.object);
}

bool InReach(int slot, void* actor) {
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(static_cast<uint8_t>(slot));
    FVector p{}, a{};
    if (!rp || !rp->valid() || !rp->TryGetLocation(p) || !E::TryGetActorLocation(actor, a)) return false;
    const float x = p.X - a.X, y = p.Y - a.Y, z = p.Z - a.Z;
    return x * x + y * y + z * z <= kReachCm * kReachCm;
}

// A refused pick: the host's colour, to the sender alone, so a copy that drifted is put back.
void Refuse(int slot, const char* why, void* actor, uint32_t eid) {
    ++g_intentsRefused;
    UE_LOGI("kerfur_color_sync: slot %d's pick on eid=%u refused: %s", slot, eid, why);
    N::KerfurColorPayload p{};
    if (actor && MakePayload(actor, p)) SendTo(slot, p);
}

bool g_watchesRegistered = false, g_watchesSettled = false;

void SettleWatches() {
    if (g_watchesSettled) return;
    if (!g_watchesRegistered) {
        g_watchesRegistered = true;
        const bool a = sg::WatchName(kRowApplyOmega, kTag, &OnApplyPre, &OnApplyPost);
        const bool b = sg::WatchName(kRowApplyKerfus, kTag, &OnApplyPre, &OnApplyPost);
        if (!a || !b) {
            g_watchesSettled = true;
            UE_LOGE("kerfur_color_sync: the gate took no watch on the picker's apply -- a colour stays on its own copy");
            return;
        }
    }
    sg::ResolvePendingNames();
    if (!sg::NameWatchSettled(kRowApplyOmega, kTag) || !sg::NameWatchSettled(kRowApplyKerfus, kTag)) return;
    g_watchesSettled = true;
    UE_LOGI("kerfur_color_sync: the picker's apply watches settled (Omega live=%d, Kerfus live=%d)",
            sg::NameWatchLive(kRowApplyOmega, kTag) ? 1 : 0, sg::NameWatchLive(kRowApplyKerfus, kTag) ? 1 : 0);
}

}  // namespace

void Install(coop::net::Session* session) { g_session.store(session, std::memory_order_release); }

void Tick() {
    N::Session* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;
    ResolveClasses();
    SettleWatches();
    if (s->role() == N::Role::Client) ClientTick(*s);
}

void HostAnnounce(void* actor) {
    if (!IsHost() || !actor || !R::IsLive(actor)) return;
    N::KerfurColorPayload p{};
    if (!MakePayload(actor, p)) return;
    SendTo(-1, p);
    ++g_announced;
    UE_LOGI("kerfur_color_sync: host's eid=%u is hsv=(%.1f,%.2f,%.2f), sent to every peer", p.eid, p.hsv[0], p.hsv[1],
            p.hsv[2]);
}

void OnState(const coop::net::KerfurColorPayload& p) {
    if (!IsClient() || Apply(p)) return;
    g_parked[p.eid] = Parked{p, Now() + kParkMs};
}

void OnIntent(int senderSlot, const coop::net::KerfurColorIntentPayload& p) {
    if (!IsHost()) return;
    void* const actor = ActorOf(p.eid);
    const Painted* const k = PaintedOf(actor);
    if (!actor || !k) return Refuse(senderSlot, "the host holds no such coloured prop", actor, p.eid);
    void* const fn = R::FindDispatchFunctionCached(R::ClassOf(actor), L"intComs_applyColor");
    if (!fn) return Refuse(senderSlot, "its class takes no picked colour", actor, p.eid);
    if (!InReach(senderSlot, actor)) return Refuse(senderSlot, "out of the sender's reach", actor, p.eid);
    ue_wrap::ParamFrame f(fn);
    const bool ran = f.valid() && f.SetRaw(L"color", p.color, sizeof(p.color)) && ue_wrap::Call(actor, f);
    if (!ran) return Refuse(senderSlot, "the host's apply did not run", actor, p.eid);
    ++g_intentsRun;
    UE_LOGI("kerfur_color_sync: slot %d's pick (%.2f,%.2f,%.2f) applied on the host's eid=%u", senderSlot, p.color[0],
            p.color[1], p.color[2], p.eid);
}

void SendJoin(int slot) {
    if (!IsHost()) return;
    ResolveClasses();
    int sent = 0;
    for (const Painted& k : g_classes) {
        if (!k.resolved) continue;
        std::vector<void*> all;
        OI::ForEachInstance(k.resolved, [](void* ctx, void* obj, int32_t) {
            if (obj && R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__"))
                static_cast<std::vector<void*>*>(ctx)->push_back(obj);
        }, &all);
        for (void* a : all) {
            N::KerfurColorPayload p{};
            if (MakePayload(a, p)) {
                SendTo(slot, p);
                ++sent;
            }
        }
    }
    if (sent) UE_LOGI("kerfur_color_sync: %d coloured kerfur(s) sent to joiner slot %d", sent, slot);
}

void OnDisconnect() {
    g_asks.clear();
    g_parked.clear();
    UE_LOGI("kerfur_color_sync: session end (announced %llu, picks run %llu refused %llu; applied %llu, asked %llu)",
            static_cast<unsigned long long>(g_announced), static_cast<unsigned long long>(g_intentsRun),
            static_cast<unsigned long long>(g_intentsRefused), static_cast<unsigned long long>(g_applied),
            static_cast<unsigned long long>(g_asked));
    g_announced = g_intentsRun = g_intentsRefused = g_applied = g_asked = 0;
}

}  // namespace coop::kerfur_color_sync
