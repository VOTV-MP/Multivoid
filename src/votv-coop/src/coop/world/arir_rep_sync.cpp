// coop/world/arir_rep_sync.cpp -- see coop/world/arir_rep_sync.h.

#include "coop/world/arir_rep_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/world/spawn_authority.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/economy.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

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
    return coop::spawn_authority::IsActiveClientSession() ? sg::Verdict::Cancel : sg::Verdict::Run;
}

void OnAddPost(const sg::Call& call) {
    if (call.tag == kTag) HostRead();
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
    g_haveSent = false;
    g_sent = 0.f;
    g_nextPoll = 0;
    g_session.store(nullptr, std::memory_order_release);
}

}  // namespace coop::arir_rep_sync
