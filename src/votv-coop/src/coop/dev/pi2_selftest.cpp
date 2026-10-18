// coop/dev/pi2_selftest.cpp -- [dev] the call-locals, the puppet-count listener, the caller-locals boot
// check and the puppet-body query on the live game. See the header.
#include "coop/dev/pi2_selftest.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/session/net_pump.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/caller_locals.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_calls.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace coop::dev::pi2_selftest {
namespace {

namespace R  = ue_wrap::reflection;
namespace SC = ue_wrap::script_calls;
namespace CL = ue_wrap::caller_locals;
namespace PL = coop::players;

bool g_done = false;
bool g_host = false;

void Pass(const char* check) { UE_LOGI("[PI-2] PASS %s", check); }

void Fail(const char* check, const char* fmt, ...) {
    char why[240];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(why, sizeof why, fmt, ap);
    va_end(ap);
    UE_LOGI("[PI-2] FAIL %s %s", check, why);
}

// ---- call-locals ----

// How many of the class's ubergraph calls of one native take a local of exactly this name.
struct Want {
    const wchar_t* local;
    int            count;
};

struct ScanClass {
    const wchar_t* cls;          // the class, found by name or loaded by `path`
    const wchar_t* path;         // its /Game object path
    const wchar_t* ubergraph;    // the function whose calls are listed
    const wchar_t* nativeClass;  // the engine class declaring the native the calls name
    const wchar_t* nativeFn;
    Want           wants[2];     // a null local ends the list
};

constexpr ScanClass kClasses[] = {
    {L"birch_C", L"/Game/objects/birch.birch_C", L"ExecuteUbergraph_birch", L"PrimitiveComponent",
     L"GetOverlappingActors",
     {{L"CallFunc_GetOverlappingActors_OverlappingActors", 5}, {L"CallFunc_GetOverlappingActors_OverlappingActors_1", 3}}},
    {L"npc_angryErieFlesh_C", L"/Game/objects/npc/npc_angryErieFlesh.npc_angryErieFlesh_C",
     L"ExecuteUbergraph_npc_angryErieFlesh", L"KismetSystemLibrary", L"SphereOverlapActors",
     {{L"CallFunc_SphereOverlapActors_OutActors", 1}, {nullptr, 0}}},
    {L"prop_shower_C", L"/Game/objects/prop_shower.prop_shower_C", L"ExecuteUbergraph_prop_shower",
     L"KismetSystemLibrary", L"LineTraceSingleForObjects",
     {{L"CallFunc_LineTraceSingleForObjects_OutHit", 1}, {nullptr, 0}}},
};

// The class by name, else loaded by its path (once per class: this runs once per process).
void* ClassOrLoad(const ScanClass& sc) {
    if (void* cls = ue_wrap::object_index::ClassByName(sc.cls)) return cls;
    return ue_wrap::asset_load::LoadObjectByPath(sc.path);
}

bool CheckOneClass(const ScanClass& sc) {
    void* cls = ClassOrLoad(sc);
    if (!cls) {
        Fail("call-locals", "%ls unloadable", sc.cls);
        return false;
    }
    void* uber = R::FindFunction(cls, sc.ubergraph);
    void* nativeCls = R::FindClass(sc.nativeClass);
    void* native = nativeCls ? R::FindFunction(nativeCls, sc.nativeFn) : nullptr;
    if (!uber || !native) {
        Fail("call-locals", "%ls: %ls did not resolve", sc.cls, uber ? sc.nativeFn : sc.ubergraph);
        return false;
    }
    std::vector<SC::CallSite> sites;
    if (!SC::EnumerateCalls(uber, sites)) {
        Fail("call-locals", "%ls: %ls does not decode", sc.cls, sc.ubergraph);
        return false;
    }
    bool ok = true;
    for (const Want& w : sc.wants) {
        if (!w.local) break;
        int n = 0;
        for (const SC::CallSite& s : sites)
            if (SC::SameCallee(s, native) && SC::CallHasLocal(s, w.local)) ++n;
        UE_LOGI("[PI-2] call-locals: %ls has %d call(s) of %ls taking %ls", sc.cls, n, sc.nativeFn, w.local);
        if (n != w.count) {
            Fail("call-locals", "%ls: %d call(s) of %ls take %ls, want %d", sc.cls, n, sc.nativeFn, w.local, w.count);
            ok = false;
        }
    }
    return ok;
}

// Two sites built by hand, needing no engine state: a truncated one with no stored local must answer
// false (not known), and one whose stored local is an address no FProperty lives at must answer false
// under the fault guard instead of faulting. The fault logs one [Error] line, once per process.
bool CheckSyntheticSites() {
    bool ok = true;
    SC::CallSite truncated{};
    truncated.localsTruncated = true;
    if (SC::CallHasLocal(truncated, L"x")) {
        Fail("call-locals", "truncated-site arm: a truncated site with no stored local answered true");
        ok = false;
    }
    SC::CallSite bad{};
    bad.localCount = 1;
    bad.locals[0] = reinterpret_cast<void*>(uintptr_t{8});
    if (SC::CallHasLocal(bad, L"x")) {
        Fail("call-locals", "bad-pointer arm: a stored local at an unreadable address answered true");
        ok = false;
    }
    return ok;
}

void CheckCallLocals() {
    bool ok = CheckSyntheticSites();
    for (const ScanClass& sc : kClasses) ok = CheckOneClass(sc) && ok;
    if (ok) Pass("call-locals");
}

// ---- listener ----

int g_spyCalls = 0;
int g_spyLast = -1;

void Spy(int count) {
    ++g_spyCalls;
    g_spyLast = count;
}

void CheckListener() {
    PL::Registry& reg = PL::Registry::Get();
    constexpr uint8_t kSpareSlot = PL::kMaxPeers - 1;
    if (reg.LocalPeerId() == kSpareSlot || reg.Puppet(kSpareSlot) || reg.GetPlayerElement(kSpareSlot)) {
        UE_LOGI("[PI-2] ABANDONED listener slot in use");
        return;
    }
    // Never spawned: GetActor() is null, which is all the registry's count reads of it.
    static coop::RemotePlayer s_puppet;

    bool ok = true;
    g_spyCalls = 0;
    g_spyLast = -1;
    const PL::Registry::PuppetCountFn previous = reg.SetPuppetCountListener(&Spy);
    if (g_spyCalls != 1 || g_spyLast < 0) {
        Fail("listener", "set-time: %d call(s), last count %d, want one call", g_spyCalls, g_spyLast);
        ok = false;
    }
    const int base = g_spyLast;

    int calls = g_spyCalls;
    reg.RegisterPuppet(kSpareSlot, &s_puppet);
    if (g_spyCalls != calls + 1 || g_spyLast != base + 1) {
        Fail("listener", "register: %d call(s), last count %d, want one call with %d", g_spyCalls - calls, g_spyLast,
             base + 1);
        ok = false;
    }
    calls = g_spyCalls;
    reg.RegisterPuppet(kSpareSlot, &s_puppet);
    if (g_spyCalls != calls) {
        Fail("listener", "re-register of the same slot: %d call(s), want none", g_spyCalls - calls);
        ok = false;
    }
    calls = g_spyCalls;
    reg.ClearPuppet(kSpareSlot);
    if (g_spyCalls != calls + 1 || g_spyLast != base) {
        Fail("listener", "clear: %d call(s), last count %d, want one call with %d", g_spyCalls - calls, g_spyLast, base);
        ok = false;
    }
    calls = g_spyCalls;
    reg.UnregisterPuppet(kSpareSlot);
    if (g_spyCalls != calls) {
        Fail("listener", "unregister of a cleared slot: %d call(s), want none", g_spyCalls - calls);
        ok = false;
    }

    // Put the previous listener back; a null one is stored and not called.
    reg.SetPuppetCountListener(previous);
    if (ok) Pass("listener");
}

// ---- caller-locals ----

void CheckCallerLocals() {
    if (CL::SelftestPassed()) Pass("caller-locals");
    else Fail("caller-locals", "the boot selftest did not pass");
}

// ---- body ----

// The slot of the other peer whose puppet this machine holds: the first client's on the host, the host's
// on a client.
uint8_t PeerSlot() { return g_host ? uint8_t{1} : PL::kPeerIdHost; }

void CheckBody() {
    PL::Registry& reg = PL::Registry::Get();
    const uint8_t slot = PeerSlot();
    coop::RemotePlayer* puppet = reg.Puppet(slot);
    void* actor = puppet ? puppet->GetActor() : nullptr;
    if (!actor) {
        Fail("body", "no puppet actor in slot %u", static_cast<unsigned>(slot));
        return;
    }
    bool ok = true;
    if (!reg.IsPuppetBody(actor)) {
        Fail("body", "the puppet's own actor is not a puppet body");
        ok = false;
    }
    if (reg.IsPuppetBody(reg.Local())) {
        Fail("body", "this machine's own player reads as a puppet body");
        ok = false;
    }
    if (reg.IsPuppetBody(nullptr)) {
        Fail("body", "null reads as a puppet body");
        ok = false;
    }
    const uint8_t id = reg.PeerIdOfBody(actor);
    if (id != slot) {
        Fail("body", "PeerIdOfBody of the puppet is %u, want %u", static_cast<unsigned>(id), static_cast<unsigned>(slot));
        ok = false;
    }
    if (ok) Pass("body");
}

// ---- readiness ----

bool Ready(coop::net::Session* session) {
    PL::Registry& reg = PL::Registry::Get();
    g_host = session->role() == coop::net::Role::Host;
    const bool up = g_host ? (reg.LocalPeerId() == PL::kPeerIdHost && reg.GetPlayerElement(PL::kPeerIdHost))
                           : coop::net_pump::HasAnnouncedWorldReady();
    if (!up || !ue_wrap::object_index::ClassByName(L"lib_C") || !reg.Local()) return false;
    coop::RemotePlayer* peer = reg.Puppet(PeerSlot());
    return peer && peer->valid();
}

void RunChecks() {
    CheckCallLocals();
    CheckListener();
    CheckCallerLocals();
    CheckBody();
    UE_LOGI("[PI-2] DONE %s", g_host ? "host" : "client");
}

}  // namespace

void Tick(coop::net::Session* session) {
    static const bool s_on = coop::config::ResolveFlag(::coop::config_registry::rows::pi2_selftest);
    if (!s_on || !session || g_done) return;
    if (!Ready(session)) return;
    g_done = true;
    RunChecks();
}

}  // namespace coop::dev::pi2_selftest
