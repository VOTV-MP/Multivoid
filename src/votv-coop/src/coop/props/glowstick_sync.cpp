// coop/props/glowstick_sync.cpp -- see coop/props/glowstick_sync.h.

#include "coop/props/glowstick_sync.h"

#include "coop/net/session.h"
#include "coop/props/prop_save_data.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/core/types.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace coop::glowstick_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;
namespace SR = ue_wrap::save_record;
using ue_wrap::FVector;

UE_FACT_FN(kRowStickTo, "glowstick_sync", L"prop_glowstick_C", L"stickTo", Declared);
UE_FACT_FN(kRowRelease, "glowstick_sync", L"prop_glowstick_C", L"releaseGlowstick", Declared);
UE_FACT_FN(kRowLoad, "glowstick_sync", L"prop_glowstick_C", L"loadData", Declared);

constexpr int kTagBase = 0x474C5730;  // 'GLW0'
enum Row { kStick = 0, kRelease, kLoad, kRowCount };
const ue_wrap::facts::Function* const kRows[kRowCount] = {&kRowStickTo, &kRowRelease, &kRowLoad};

std::atomic<coop::net::Session*> g_session{nullptr};
unsigned g_registered = 0;
bool g_converging = false;                  // inside this lane's own releaseGlowstick / stickTo
std::vector<void*> g_dirty;                 // glowsticks a player's own act stuck or freed; published next pass
struct Before { bool sticked; FVector loc; };
std::unordered_map<void*, Before> g_before;  // a landing's loadData: the copy's state before it
// A CLIENT's own publishes, by key: the `sticked` of the latest. Until the host's re-publish of that one lands, a
// landing for the key is an echo of this player's own act, older or the same, and the copy already holds the stick.
std::unordered_map<std::wstring, bool> g_ownEcho;
uint32_t g_published = 0, g_converged = 0;

int Bool(void* o, const wchar_t* name) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!o || !R::FindBoolProperty(R::ClassOf(o), name, off, mask)) return -1;
    return (static_cast<const uint8_t*>(o)[off] & mask) ? 1 : 0;
}

bool Loc(void* o, FVector& out) {
    const int32_t off = o ? R::FindPropertyOffset(R::ClassOf(o), L"loc") : -1;
    if (off < 0) return false;
    std::memcpy(&out, static_cast<const uint8_t*>(o) + off, sizeof(out));
    return true;
}

bool CallNoArgs(void* o, const wchar_t* fn) {
    ue_wrap::ParamFrame f(R::FindDispatchFunctionCached(R::ClassOf(o), fn));
    return f.valid() && ue_wrap::Call(o, f);
}

bool SetBool(void* o, const wchar_t* name, bool v) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!o || !R::FindBoolProperty(R::ClassOf(o), name, off, mask)) return false;
    uint8_t& b = static_cast<uint8_t*>(o)[off];
    b = v ? static_cast<uint8_t>(b | mask) : static_cast<uint8_t>(b & ~mask);
    return true;
}

// The stick made from the saved point: stickTo's re-stick form (setFrame true) sets `sticked` and puts the hook at
// the point, then hookDelay constrains as the stick itself did. Its static branch (the hook to the glowstick, frames
// from the hook) is the one a peer can make: the struck component of a stick to a moving body is a pointer no record
// carries, so where this copy has none the stick is made static.
bool ReStick(void* o) {
    ue_wrap::ParamFrame f(R::FindDispatchFunctionCached(R::ClassOf(o), L"stickTo"));
    if (!f.valid() || !f.Set(L"setFrame", true) || !ue_wrap::Call(o, f)) return false;
    const int32_t co = R::FindPropertyOffset(R::ClassOf(o), L"stickComp");
    void* comp = nullptr;
    if (co >= 0) std::memcpy(&comp, static_cast<const uint8_t*>(o) + co, sizeof(comp));
    if (!comp || !R::IsLive(comp)) SetBool(o, L"stickStatic", true);
    return CallNoArgs(o, L"hookDelay");
}

std::wstring KeyOf(void* o) { return ue_wrap::prop::GetInteractableKeyString(o); }

sg::Verdict OnPre(const sg::Call& call) {
    if (call.tag != kTagBase + kLoad || !call.object || !SR::IsApplying()) return sg::Verdict::Run;
    Before b{Bool(call.object, L"sticked") == 1, {}};
    Loc(call.object, b.loc);
    g_before[call.object] = b;
    return sg::Verdict::Run;
}

void OnPost(const sg::Call& call) {
    void* const o = call.object;
    if (!o || !R::IsLive(o) || !g_session.load(std::memory_order_acquire)) return;
    const int row = call.tag - kTagBase;
    if (row == kStick || row == kRelease) {
        // A player's own act: not a record landing, not this lane's own converge.
        if (g_converging || SR::IsApplying()) return;
        for (void* d : g_dirty)
            if (d == o) return;
        g_dirty.push_back(o);
        return;
    }
    if (row != kLoad || !SR::IsApplying()) return;
    auto it = g_before.find(o);
    const Before was = it != g_before.end() ? it->second : Before{false, {}};
    if (it != g_before.end()) g_before.erase(it);
    const bool now = Bool(o, L"sticked") == 1;
    auto own = g_ownEcho.find(KeyOf(o));
    if (own != g_ownEcho.end()) {
        // The fields are the echo's; the constraint stays this player's own, and so does `sticked` until its echo.
        if (own->second == now) g_ownEcho.erase(own);
        else SetBool(o, L"sticked", own->second);
        if (was.sticked) {
            const int32_t off = R::FindPropertyOffset(R::ClassOf(o), L"loc");
            if (off >= 0) std::memcpy(static_cast<uint8_t*>(o) + off, &was.loc, sizeof(was.loc));
        }
        return;
    }
    FVector loc{};
    Loc(o, loc);
    const float dx = loc.X - was.loc.X, dy = loc.Y - was.loc.Y, dz = loc.Z - was.loc.Z;
    const bool moved = dx * dx + dy * dy + dz * dz > 1.f;
    if (!now && !was.sticked) return;
    if (now && was.sticked && !moved) return;
    g_converging = true;
    bool released = false, stuck = false;
    if (was.sticked) released = CallNoArgs(o, L"releaseGlowstick");
    if (now) stuck = ReStick(o);
    g_converging = false;
    ++g_converged;
    UE_LOGI("glowstick_sync: '%ls' %s from its record (released %d, stuck %d) at (%.0f,%.0f,%.0f)", KeyOf(o).c_str(),
            now ? "stuck" : "freed", released ? 1 : 0, stuck ? 1 : 0, loc.X, loc.Y, loc.Z);
}

void Register() {
    for (int i = 0; i < kRowCount; ++i) {
        if (g_registered & (1u << i)) continue;
        if (sg::WatchName(*kRows[i], kTagBase + i, i == kLoad ? &OnPre : nullptr, &OnPost)) g_registered |= 1u << i;
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    Register();
}

void Tick() {
    coop::net::Session* const s = g_session.load(std::memory_order_acquire);
    if (!s || g_dirty.empty()) return;
    std::vector<void*> dirty;
    dirty.swap(g_dirty);
    for (void* o : dirty) {
        if (!o || !R::IsLive(o)) continue;
        const std::wstring key = KeyOf(o);
        if (key.empty() || key == L"None") continue;
        const bool ok = coop::prop_save_data::Publish(s, o, key);
        if (ok && s->role() != coop::net::Role::Host) g_ownEcho[key] = Bool(o, L"sticked") == 1;
        ++g_published;
        UE_LOGI("glowstick_sync: this player %s '%ls' -- its record goes %s (sent %d)",
                Bool(o, L"sticked") == 1 ? "stuck" : "freed", key.c_str(),
                s->role() == coop::net::Role::Host ? "to every peer" : "to the host", ok ? 1 : 0);
    }
}

void OnDisconnect() {
    if (g_published || g_converged)
        UE_LOGI("glowstick_sync: this session published %u glowstick record(s) and made %u stick(s) from records",
                g_published, g_converged);
    g_dirty.clear();
    g_before.clear();
    g_ownEcho.clear();
    g_published = g_converged = 0;
    g_converging = false;
}

}  // namespace coop::glowstick_sync
