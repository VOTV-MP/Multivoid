// coop/world/flood_sync.cpp -- see coop/world/flood_sync.h.

#include "coop/world/flood_sync.h"

#include "coop/element/intent_authority.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"
#include "coop/world/spawn_authority.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace coop::flood_sync {
namespace {

namespace N  = coop::net;
namespace R  = ue_wrap::reflection;
namespace OI = ue_wrap::object_index;
namespace sg = ue_wrap::script_gate;
using Payload = N::FloodPayload;

constexpr const wchar_t* kPipeClass = L"burstWaterPipe_C";
constexpr const wchar_t* kVolumeClass = L"waterVolume_basementFlooder_C";
constexpr const wchar_t* kToolboxClass = L"prop_toolbox_C";
constexpr float kReachUU = 400.f;
constexpr float kLevelEps = 1.f;         // a level moved less than this is not sent
constexpr uint64_t kReadMs = 250;        // the host reads its pipes and volumes four times a second
constexpr int kMaxIntentsPerSecond = 4;  // a sender's repairs past this in one second are dropped

UE_FACT_FN(kRowPipeTick, "flood_sync", L"burstWaterPipe_C", L"ReceiveTick", Declared);
UE_FACT_FN(kRowPipeDamage, "flood_sync", L"burstWaterPipe_C", L"addDamage", Declared);
UE_FACT_FN(kRowPipeFix, "flood_sync", L"burstWaterPipe_C", L"toolboxFix", Declared);

enum Row { kPipeTick, kPipeDamage, kPipeFix, kRowCount };
const ue_wrap::facts::Function* const kRows[kRowCount] = {&kRowPipeTick, &kRowPipeDamage, &kRowPipeFix};
constexpr int kTagBase = 0x464C4400;   // 'FLD', then the row

std::atomic<N::Session*> g_session{nullptr};
uint32_t g_registered = 0;   // bit i: row i's watch registered
int g_applying = 0;          // inside our own apply: no watch reads it as a player's act

uint64_t Now() { return ::GetTickCount64(); }

N::Session* Live() {
    N::Session* s = g_session.load(std::memory_order_acquire);
    return s && s->running() ? s : nullptr;
}

std::vector<void*> All(const wchar_t* cls) {
    std::vector<void*> out;
    if (void* const c = OI::ClassByName(cls))
        OI::ForEachInstance(c, [](void* ctx, void* obj, int32_t) {
            if (obj && R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__"))
                static_cast<std::vector<void*>*>(ctx)->push_back(obj);
        }, &out);
    return out;
}

std::wstring NameOf(void* o) {
    const std::wstring n = R::ToString(R::NameOf(o));
    return n.size() > sizeof(N::WireKey::data) ? std::wstring() : n;
}

void* ByName(const wchar_t* cls, const std::wstring& name) {
    if (name.empty()) return nullptr;
    for (void* o : All(cls))
        if (NameOf(o) == name) return o;
    return nullptr;
}

bool Damaged(void* pipe, bool& out) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!pipe || !R::FindBoolProperty(R::ClassOf(pipe), L"damaged", off, mask)) return false;
    out = (static_cast<const uint8_t*>(pipe)[off] & mask) != 0;
    return true;
}

bool SetPipe(void* pipe, bool damaged) {
    ue_wrap::ParamFrame f(R::FindDispatchFunctionCached(R::ClassOf(pipe), L"State"));
    return f.valid() && f.Set<bool>(L"damaged", damaged) && ue_wrap::Call(pipe, f);
}

bool Level(void* volume, float& out) {
    ue_wrap::ParamFrame f(R::FindDispatchFunctionCached(R::ClassOf(volume), L"getLevel"));
    if (!f.valid() || !ue_wrap::Call(volume, f)) return false;
    out = f.Get<float>(L"level");
    return std::isfinite(out);
}

bool SetLevel(void* volume, float level) {
    ue_wrap::ParamFrame f(R::FindDispatchFunctionCached(R::ClassOf(volume), L"setLevel"));
    if (!f.valid() || !f.Set<float>(L"alpha", level) || !ue_wrap::Call(volume, f)) return false;
    void* const upd = R::FindDispatchFunctionCached(R::ClassOf(volume), L"updateWaterData");
    if (upd) R::CallFunction(volume, upd, nullptr);
    return true;
}

void Fill(Payload& p, const std::wstring& name, uint8_t op, float level, bool damaged) {
    p = Payload{};
    N::WireKeyFromString(name, p.name);
    p.level = level;
    p.damaged = damaged ? 1 : 0;
    p.op = op;
}

// ---- HOST: the reads and the sends ----

std::unordered_map<std::wstring, bool> g_sentPipe;
std::unordered_map<std::wstring, float> g_sentLevel;
uint64_t g_nextRead = 0;

void Send(N::Session& s, const Payload& p, int slot) {
    if (slot >= 0) s.SendReliableToSlot(slot, N::ReliableKind::FloodState, &p, sizeof(p));
    else s.SendReliable(N::ReliableKind::FloodState, &p, sizeof(p));
}

void SendPipe(N::Session& s, void* pipe, const std::wstring& name, int slot) {
    bool damaged = false;
    if (!Damaged(pipe, damaged)) return;
    Payload p;
    Fill(p, name, N::kFloodOpPipe, 0.f, damaged);
    Send(s, p, slot);
    if (slot < 0) g_sentPipe[name] = damaged;
}

void SendLevel(N::Session& s, void* volume, const std::wstring& name, int slot) {
    float level = 0.f;
    if (!Level(volume, level)) return;
    Payload p;
    Fill(p, name, N::kFloodOpLevel, level, false);
    Send(s, p, slot);
    if (slot < 0) g_sentLevel[name] = level;
}

void HostRead(N::Session& s) {
    if (Now() < g_nextRead) return;
    g_nextRead = Now() + kReadMs;
    for (void* pipe : All(kPipeClass)) {
        bool damaged = false;
        const std::wstring name = NameOf(pipe);
        if (name.empty() || !Damaged(pipe, damaged)) continue;
        const auto it = g_sentPipe.find(name);
        if (it == g_sentPipe.end() ? damaged : it->second != damaged) {
            SendPipe(s, pipe, name, -1);
            UE_LOGI("flood_sync: HOST pipe '%ls' %s -- sent", name.c_str(), damaged ? "burst" : "mended");
        }
    }
    for (void* v : All(kVolumeClass)) {
        float level = 0.f;
        const std::wstring name = NameOf(v);
        if (name.empty() || !Level(v, level)) continue;
        const auto it = g_sentLevel.find(name);
        if (it == g_sentLevel.end() ? level >= kLevelEps : std::fabs(it->second - level) >= kLevelEps) SendLevel(s, v, name, -1);
    }
}

// ---- HOST: a client's repair ----

struct Rate {
    uint64_t second = 0;
    int n = 0;
};
Rate g_rate[N::kMaxPeers];

void Judge(N::Session& s, uint8_t slot, const Payload& p) {
    const std::wstring name = N::StringFromWireKey(p.name);
    if (!s.IsSlotWorldReady(slot)) return;
    void* const pipe = ByName(kPipeClass, name);
    if (!pipe) {
        UE_LOGI("flood_sync: slot %u's repair names no pipe ('%ls')", static_cast<unsigned>(slot), name.c_str());
        return;
    }
    const char* why = nullptr;
    bool damaged = false;
    if (coop::players::Registry::Get().LifeOf(slot) != coop::players::PuppetLife::Alive) why = "dead";
    if (!why) {
        const coop::element::IntentSubject subject = coop::element::IntentTarget::ForClientIntent(s, slot, kReachUU).Authorize(pipe);
        if (!subject) why = coop::element::OutcomeName(subject.outcome);
    }
    if (!why && !coop::hand_item::HeldClassIs(slot, kToolboxClass)) why = "no toolbox in the hand";
    if (!why && (!Damaged(pipe, damaged) || !damaged)) why = "the pipe is whole";
    if (why) {
        UE_LOGI("flood_sync: slot %u's repair of '%ls' refused: %s", static_cast<unsigned>(slot), name.c_str(), why);
        SendPipe(s, pipe, name, slot);
        return;
    }
    ++g_applying;
    SetPipe(pipe, false);
    --g_applying;
    UE_LOGI("flood_sync: slot %u mended pipe '%ls' on the host", static_cast<unsigned>(slot), name.c_str());
    SendPipe(s, pipe, name, -1);
}

// ---- CLIENT: the watches ----

void* g_fixPipe = nullptr;
bool g_fixWasDamaged = false;

sg::Verdict OnPre(const sg::Call& call) {
    const int row = call.tag - kTagBase;
    if (row < 0 || row >= kRowCount || g_applying || !coop::spawn_authority::IsActiveClientSession())
        return sg::Verdict::Run;
    // The rise and a pipe's burst are the host copy's.
    if (row != kPipeFix) return sg::Verdict::Cancel;
    g_fixPipe = call.object;
    g_fixWasDamaged = false;
    Damaged(call.object, g_fixWasDamaged);
    return sg::Verdict::Run;
}

void OnPost(const sg::Call& call) {
    if (call.tag - kTagBase != kPipeFix || call.object != g_fixPipe) return;
    g_fixPipe = nullptr;
    bool damaged = true;
    N::Session* s = Live();
    if (!s || s->role() != N::Role::Client || !g_fixWasDamaged || !Damaged(call.object, damaged) || damaged) return;
    const std::wstring name = NameOf(call.object);
    if (name.empty()) return;
    Payload p;
    Fill(p, name, N::kFloodOpFix, 0.f, false);
    s->SendReliable(N::ReliableKind::FloodIntent, &p, sizeof(p));
    UE_LOGI("flood_sync: this player mended pipe '%ls' -- the host mends it", name.c_str());
}

void Register() {
    for (int i = 0; i < kRowCount; ++i) {
        if (g_registered & (1u << i)) continue;
        if (sg::WatchName(*kRows[i], kTagBase + i, &OnPre, i == kPipeFix ? &OnPost : nullptr)) g_registered |= 1u << i;
    }
}

}  // namespace

void Install(N::Session* session) { g_session.store(session, std::memory_order_release); }

void Tick() {
    N::Session* s = Live();
    if (!s) return;
    if (g_registered != (1u << kRowCount) - 1) {
        static uint64_t nextTry = 0;
        if (Now() >= nextTry) {
            nextTry = Now() + 1000;
            Register();
            if (g_registered == (1u << kRowCount) - 1) UE_LOGI("flood_sync: the burst pipes' watches are live");
        }
    }
    if (s->role() == N::Role::Host) HostRead(*s);
}

void OnState(const Payload& p) {
    const std::wstring name = N::StringFromWireKey(p.name);
    ++g_applying;
    if (p.op == N::kFloodOpPipe) {
        void* const pipe = ByName(kPipeClass, name);
        bool damaged = false;
        const bool want = p.damaged != 0;
        if (pipe && Damaged(pipe, damaged) && damaged != want && SetPipe(pipe, want))
            UE_LOGI("flood_sync: CLIENT pipe '%ls' %s, as the host's", name.c_str(), want ? "burst" : "mended");
    } else if (p.op == N::kFloodOpLevel && std::isfinite(p.level)) {
        if (void* const v = ByName(kVolumeClass, name)) SetLevel(v, p.level);
    }
    --g_applying;
}

void OnIntent(int senderSlot, const Payload& p) {
    N::Session* s = Live();
    if (!s || s->role() != N::Role::Host || senderSlot <= 0 || senderSlot >= N::kMaxPeers || p.op != N::kFloodOpFix) return;
    Rate& r = g_rate[senderSlot];
    const uint64_t sec = Now() / 1000;
    if (r.second != sec) r = Rate{sec, 0};
    if (++r.n > kMaxIntentsPerSecond) return;
    Judge(*s, static_cast<uint8_t>(senderSlot), p);
}

void SendJoin(int slot) {
    N::Session* s = Live();
    if (!s || s->role() != N::Role::Host) return;
    for (void* pipe : All(kPipeClass)) {
        const std::wstring name = NameOf(pipe);
        if (!name.empty()) SendPipe(*s, pipe, name, slot);
    }
    for (void* v : All(kVolumeClass)) {
        const std::wstring name = NameOf(v);
        if (!name.empty()) SendLevel(*s, v, name, slot);
    }
}

void OnDisconnect() {
    g_sentPipe.clear();
    g_sentLevel.clear();
    for (Rate& r : g_rate) r = Rate{};
    g_fixPipe = nullptr;
}

}  // namespace coop::flood_sync
