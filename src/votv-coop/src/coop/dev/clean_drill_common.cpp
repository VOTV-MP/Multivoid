// coop/dev/clean_drill_common.cpp -- the decal reads, the peer marks and the verdict that the clean drill's arms share; see
// coop/dev/clean_drill.h.

#include "clean_drill_detail.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/roster.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"
#include "harness/autotest/peer_signal.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/devices/grime.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace coop::dev::clean_drill::detail {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace OI = ue_wrap::object_index;
namespace G  = ue_wrap::grime;
namespace PS = harness::autotest::peer_signal;

std::string g_tag;
bool g_begun = false;
bool g_ended = false;
char g_endKind = 0;
const char* g_phase = "start";
uint64_t g_phaseAt = 0;
int g_peers = kPeers;

uint64_t Now() { return ::GetTickCount64(); }

std::string EndName() { return g_tag + "_end"; }

}  // namespace

Arm ArmOf() {
    static const Arm a = [] {
        const std::string v = coop::config::ResolveEnum(::coop::config_registry::rows::clean_drill);
        return v == "clean" ? Arm::Clean : v == "leaves" ? Arm::Leaves : v == "leaves_join" ? Arm::LeavesJoin : Arm::Off;
    }();
    return a;
}

std::vector<void*> All(const wchar_t* cls) {
    std::vector<void*> out;
    OI::DrainAll();
    void* const c = OI::ClassByName(cls);
    if (c)
        OI::ForEachInstance(c, [](void* ctx, void* obj, int32_t) {
            if (obj && R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__"))
                static_cast<std::vector<void*>*>(ctx)->push_back(obj);
        }, &out);
    return out;
}

float Dist2(const FVector& a, const FVector& b) {
    const float x = a.X - b.X, y = a.Y - b.Y, z = a.Z - b.Z;
    return x * x + y * y + z * z;
}

void* DecalAt(const std::wstring& cls, const FVector& at) {
    for (void* d : All(cls.c_str())) {
        FVector p{};
        if (E::TryGetActorLocation(d, p) && Dist2(p, at) < 1.f) return d;
    }
    return nullptr;
}

bool BoolOf(void* obj, const wchar_t* name) {
    int32_t off = -1;
    uint8_t mask = 0;
    return obj && R::FindBoolProperty(R::ClassOf(obj), name, off, mask) && off >= 0 && mask != 0 &&
           (static_cast<const uint8_t*>(obj)[off] & mask) != 0;
}

float ProcessOf(void* decal) {
    float p = -1.f;
    return decal && G::ReadProcess(decal, p) ? p : -1.f;
}

bool CallNoArgs(void* obj, const wchar_t* fn) {
    void* const f = obj ? R::FindDispatchFunctionCached(R::ClassOf(obj), fn) : nullptr;
    ue_wrap::ParamFrame frame(f);
    return f && frame.valid() && ue_wrap::Call(obj, frame);
}

bool Clean(void* decal, void* sponge, float sub) {
    void* const fn = decal ? R::FindDispatchFunctionCached(R::ClassOf(decal), L"clean") : nullptr;
    ue_wrap::ParamFrame f(fn);
    return fn && f.valid() && f.Set<void*>(L"sponge", sponge) && f.Set<float>(L"Sub", sub) &&
           f.Set<bool>(L"noSound", sponge == nullptr) && ue_wrap::Call(decal, f);
}

std::string Narrow(const std::wstring& w) {
    std::string s;
    for (const wchar_t c : w) s.push_back(c < 128 ? static_cast<char>(c) : '?');
    return s;
}

std::vector<std::string> Split(const std::string& text, char sep) {
    std::vector<std::string> out;
    size_t from = 0;
    for (;;) {
        const size_t at = text.find(sep, from);
        out.push_back(text.substr(from, at == std::string::npos ? std::string::npos : at - from));
        if (at == std::string::npos) return out;
        from = at + 1;
    }
}

bool ToFloat(const std::string& piece, float& out) {
    char* end = nullptr;
    const float v = std::strtof(piece.c_str(), &end);
    if (piece.empty() || end != piece.c_str() + piece.size()) return false;
    out = v;
    return true;
}

bool FindAnchor(FVector& out) {
    const std::string env = coop::config::ReadEnv("VOTVCOOP_CLEAN_BUCKET");
    const std::string want = env.empty() ? std::string("upAbhZM6weqmeddpJE6vHg") : env;
    for (void* b : All(L"prop_bucket_C")) {
        const std::wstring k = ue_wrap::prop::GetInteractableKeyString(b);
        if (Narrow(k) == want && E::TryGetActorLocation(b, out)) return true;
    }
    return false;
}

int MySlot() { return static_cast<int>(coop::players::Registry::Get().LocalPeerId()); }
bool IsHost() { return coop::roster::LocalIsHost(); }
const char* SlotName(int slot) { return slot == 0 ? "host" : slot == 1 ? "first client" : "second client"; }

bool IsActor() {
    static const bool host = coop::config::ReadEnv("VOTVCOOP_CLEAN_ACTOR") == "host";
    return host ? IsHost() : (!IsHost() && MySlot() == 1);
}

bool WorldUp(coop::net::Session& s) {
    if (!G::EnsureResolved() || OI::Backlog() != 0 || !coop::players::Registry::Get().Local()) return false;
    if (IsHost()) {
        for (int slot = 1; slot < coop::net::kMaxPeers; ++slot)
            if (s.IsSlotWorldReady(slot)) return true;
        return false;
    }
    return coop::net_pump::HasAnnouncedWorldReady() &&
           coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle;
}

void MarkMine(const char* name, const std::string& text) {
    PS::Mark((std::string(name) + "_" + std::to_string(MySlot())).c_str(), text);
}

bool PeekOf(const char* name, int slot, std::string& text) {
    return PS::Peek((std::string(name) + "_" + std::to_string(slot)).c_str(), text);
}

void MarkShared(const char* name, const std::string& text) { PS::Mark(name, text); }
bool PeekShared(const char* name, std::string& text) { return PS::Peek(name, text); }

bool Over() {
    if (g_ended) return true;
    std::string text;
    if (g_begun && PS::Peek(EndName().c_str(), text) && !text.empty()) {
        g_ended = true;
        g_endKind = text[0];
        UE_LOGI("[CLN] %s: the run was ended by another peer: %s", g_tag.c_str(), text.c_str());
    }
    return g_ended;
}

char EndKind() { return g_endKind; }

void Decide(coop::rig_verdict::Outcome o, const char* fmt, ...) {
    if (g_ended) return;
    char text[352] = {};
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    g_ended = true;
    g_endKind = o == coop::rig_verdict::Outcome::Pass ? 'P' : o == coop::rig_verdict::Outcome::Fail ? 'F' : 'I';
    if (g_begun) PS::Mark(EndName().c_str(), std::string(1, g_endKind) + " " + text);
    coop::rig_verdict::Say(g_tag.c_str(), o, "%s", text);
}

void Enter(const char* phase) {
    g_phase = phase;
    g_phaseAt = Now();
}

bool Overdue() {
    if (g_ended || Now() - g_phaseAt <= kBoundMs) return false;
    Decide(coop::rig_verdict::Outcome::Invalid, "%s on the %s: no event within %llu s", g_phase, SlotName(MySlot()),
           static_cast<unsigned long long>(kBoundMs / 1000));
    return true;
}

void SetPeers(int n) { g_peers = n; }

int Arrive(coop::net::Session& s, const char* tag, const char* syncName, bool ready, bool barrier) {
    if (g_ended) return -1;
    if (!g_begun) {
        if (!WorldUp(s)) return 0;
        g_tag = tag;
        if (!PS::Begin(tag)) {   // says its own INVALID line
            g_ended = true;
            g_endKind = 'I';
            return -1;
        }
        g_begun = true;
        Enter("the lane's readiness and the other peers");
        UE_LOGI("[CLN] %s: armed on the %s (slot %d)", tag, SlotName(MySlot()), MySlot());
    }
    if (Over() || Overdue()) return -1;
    if (!ready) return 0;
    if (!barrier) return 1;
    // Marked on every tick until every other peer's mark is read: a peer that began its drill later reads only a mark
    // written after its own start, so the last one written must postdate it.
    MarkMine(syncName, "up");
    std::string text;
    bool all = true;
    for (int slot = 0; slot < g_peers; ++slot)
        if (slot != MySlot() && !PeekOf(syncName, slot, text)) all = false;
    return all ? 1 : 0;
}

}  // namespace coop::dev::clean_drill::detail
