// coop/world/arir_rep_sync.cpp -- see coop/world/arir_rep_sync.h.

#include "coop/world/arir_rep_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/slot_count.h"
#include "coop/world/spawn_authority.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/economy.h"
#include "ue_wrap/world/world_singleton.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

namespace coop::arir_rep_sync {
namespace {

namespace N  = coop::net;
namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;

UE_FACT_FN(kRowAddRep, "arir_rep_sync", L"lib_C", L"addAriralRep", Declared);
constexpr int kTag = 0x41525250;   // 'ARRP'

std::atomic<N::Session*> g_session{nullptr};
bool g_registered = false;
bool g_haveSent = false;
float g_sent = 0.f;       // HOST: the value the clients were last sent
uint64_t g_nextPoll = 0;

uint64_t Now() { return ::GetTickCount64(); }

namespace C = coop::net::arir_rep_cause;
bool g_shipPhotoGranted = false;          // HOST: the visit ship's photo has paid, the host's own or a client's
uint64_t g_lastAsk[coop::players::kMaxPeers] = {};
int g_applyingIntent = 0;

// addAriralRep's two parameters, from lib_C's class once it has loaded.
int32_t g_offRep = -1, g_offCtx = -1;
bool ResolveParams() {
    if (g_offRep >= 0 && g_offCtx >= 0) return true;
    void* const cls = ue_wrap::object_index::ClassByName(L"lib_C");
    void* const fn = cls ? R::FindFunction(cls, L"addAriralRep") : nullptr;
    g_offRep = fn ? R::FindParamOffset(fn, L"rep") : -1;
    g_offCtx = fn ? R::FindParamOffset(fn, L"__WorldContext") : -1;
    return g_offRep >= 0 && g_offCtx >= 0;
}

bool ReadCall(const sg::Call& call, float& rep, void*& ctx) {
    if (!call.locals || !ResolveParams()) return false;
    std::memcpy(&rep, call.locals + g_offRep, sizeof(rep));
    std::memcpy(&ctx, call.locals + g_offCtx, sizeof(ctx));
    return ctx && R::IsLive(ctx);
}

// What a caller is, by its class and its amount: a change this machine's own player made, or none (-1). The rest of
// the callers are the world's, which the host runs for itself.
int CauseOf(void* ctx, float rep) {
    const std::wstring cls = R::ClassNameOf(ctx);
    if (cls == L"mainGamemode_C" && std::fabs(std::fabs(rep) - 5.f) < 0.01f) return C::kWake;
    if (cls.find(L"arirFollower") != std::wstring::npos && std::fabs(rep + 2.f) < 0.01f) return C::kSwing;
    if (cls == L"arirShipAppear_C" && std::fabs(rep - 15.f) < 0.01f) return C::kShipPhoto;
    return -1;
}

// lib_C's addAriralRep, the host's own, for a change a client's player made.
bool AddRep(float v) {
    void* const lib = R::FindClassDefaultObject(L"lib_C");
    void* const cls = ue_wrap::object_index::ClassByName(L"lib_C");
    void* const ctx = ue_wrap::world_singleton::Gamemode();
    ue_wrap::ParamFrame f(cls ? R::FindFunction(cls, L"addAriralRep") : nullptr);
    ++g_applyingIntent;
    const bool ran = lib && ctx && f.valid() && f.Set<float>(L"rep", v) && f.Set<void*>(L"__WorldContext", ctx) &&
                     ue_wrap::Call(lib, f);
    --g_applyingIntent;
    return ran;
}

float* RepField() {
    void* const ss = ue_wrap::economy::SaveSlotPtr();
    const int32_t off = ss ? R::FindPropertyOffset(R::ClassOf(ss), L"arirReputation") : -1;
    return off < 0 ? nullptr : reinterpret_cast<float*>(static_cast<uint8_t*>(ss) + off);
}

void SendValue(N::Session& s, float v, int slot) {
    N::EventCuePayload p{};
    p.cueId = kCueArirRep;
    p.x = v;
    const bool ok = slot < 0 ? s.SendReliable(N::ReliableKind::EventCue, &p, sizeof(p))
                             : s.SendReliableToSlot(slot, N::ReliableKind::EventCue, &p, sizeof(p));
    if (!ok || slot >= 0) return;
    g_sent = v;
    g_haveSent = true;
    UE_LOGI("arir_rep: the Arirals' reputation %.2f sent", v);
}

// HOST: the value, sent when it moved.
void HostRead() {
    N::Session* const s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != N::Role::Host) return;
    const float* rep = RepField();
    if (!rep || !std::isfinite(*rep)) return;
    if (g_haveSent && *rep == g_sent) return;
    SendValue(*s, *rep, -1);
}

sg::Verdict OnAddPre(const sg::Call& call) {
    if (call.tag != kTag) return sg::Verdict::Run;
    if (!coop::spawn_authority::IsActiveClientSession()) return sg::Verdict::Run;
    // A change this client's own player made is asked of the host, which makes it as its own.
    float rep = 0.f;
    void* ctx = nullptr;
    const int cause = ReadCall(call, rep, ctx) ? CauseOf(ctx, rep) : -1;
    N::Session* const s = g_session.load(std::memory_order_acquire);
    if (cause >= 0 && s && s->connected()) {
        N::ArirRepIntentPayload p{};
        p.rep = rep;
        p.cause = static_cast<uint8_t>(cause);
        const bool sent = s->SendReliable(N::ReliableKind::ArirRepIntent, &p, sizeof(p));
        UE_LOGI("arir_rep: this player's change %+.0f (cause %d) asked of the host (sent=%d)", rep, cause, sent ? 1 : 0);
    }
    return sg::Verdict::Cancel;
}

void OnAddPost(const sg::Call& call) {
    if (call.tag != kTag) return;
    // The host's own photo of the visit ship has paid: a client's after it pays nothing, as the game's do-once.
    float rep = 0.f;
    void* ctx = nullptr;
    if (!g_applyingIntent && ReadCall(call, rep, ctx) && CauseOf(ctx, rep) == C::kShipPhoto) g_shipPhotoGranted = true;
    HostRead();
}

}  // namespace

void Install(N::Session* session) { g_session.store(session, std::memory_order_release); }

void Tick() {
    N::Session* const s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;
    const uint64_t now = Now();
    if (!g_registered) {
        static uint64_t nextTry = 0;
        if (now >= nextTry) {
            nextTry = now + 1000;
            if ((g_registered = sg::WatchName(kRowAddRep, kTag, &OnAddPre, &OnAddPost)))
                UE_LOGI("arir_rep: the Arirals' reputation writer is watched");
        }
    }
    if (now < g_nextPoll) return;
    g_nextPoll = now + 1000;
    HostRead();   // a load or another route moved it
}

void SendJoin(int slot) {
    N::Session* const s = g_session.load(std::memory_order_acquire);
    const float* rep = RepField();
    if (!s || s->role() != N::Role::Host || !rep || !std::isfinite(*rep)) return;
    SendValue(*s, *rep, slot);
}

void OnIntent(N::Session& session, int slot, const N::ArirRepIntentPayload& p) {
    if (session.role() != N::Role::Host || slot <= 0 || slot >= coop::players::kMaxPeers) return;
    const char* refused = nullptr;
    const uint64_t now = Now();
    if (!session.IsSlotWorldReady(slot)) refused = "not world-ready";
    else if (now - g_lastAsk[slot] < 1000u) refused = "more than one a second";
    else if (p.cause == C::kWake && std::fabs(std::fabs(p.rep) - 5.f) >= 0.01f) refused = "not the wake's amount";
    else if (p.cause == C::kSwing && std::fabs(p.rep + 2.f) >= 0.01f) refused = "not the swing's amount";
    else if (p.cause == C::kShipPhoto && std::fabs(p.rep - 15.f) >= 0.01f) refused = "not the photo's amount";
    else if (p.cause == C::kShipPhoto && g_shipPhotoGranted) refused = "the ship's photo has paid";
    else if (p.cause > C::kShipPhoto) refused = "an unknown cause";
    if (refused) {
        UE_LOGI("arir_rep: slot %d's change %+.0f (cause %u) refused -- %s", slot, p.rep, p.cause, refused);
        return;
    }
    g_lastAsk[slot] = now;
    if (p.cause == C::kShipPhoto) g_shipPhotoGranted = true;
    UE_LOGI("arir_rep: slot %d's change %+.0f (cause %u) made on the host (ran=%d)", slot, p.rep, p.cause,
            AddRep(p.rep) ? 1 : 0);
}

bool OnCue(const N::EventCuePayload& p) {
    if (p.cueId != kCueArirRep) return false;
    N::Session* const s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != N::Role::Client || !std::isfinite(p.x) || p.x < -1000.f || p.x > 1000.f) return true;
    float* rep = RepField();
    if (!rep) {
        UE_LOGW("arir_rep: the host's reputation %.2f came before this world's save slot -- not applied", p.x);
        return true;
    }
    *rep = p.x;
    UE_LOGI("arir_rep: the host's reputation %.2f applied", p.x);
    return true;
}

void OnDisconnect() {
    g_shipPhotoGranted = false;
    for (uint64_t& t : g_lastAsk) t = 0;
    g_haveSent = false;
    g_sent = 0.f;
    g_nextPoll = 0;
    g_session.store(nullptr, std::memory_order_release);
}

}  // namespace coop::arir_rep_sync
