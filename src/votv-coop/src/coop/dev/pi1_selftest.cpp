// coop/dev/pi1_selftest.cpp -- [dev] the four substrate primitives on the live game. See the header.
#include "coop/dev/pi1_selftest.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/session/net_pump.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/caller_locals.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_calls.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/engine/engine_pawn.h"
#include "ue_wrap/facts/function_row.h"

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <vector>

namespace coop::dev::pi1_selftest {
namespace {

namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;
namespace UH = ue_wrap::ufunction_hook;
namespace SC = ue_wrap::script_calls;
namespace CL = ue_wrap::caller_locals;
namespace E  = ue_wrap::engine;
namespace PL = coop::players;

UE_FACT_FN(kRowPi1GetPoints, "dev.pi1_selftest", L"lib_C", L"getPoints", Declared);
UE_FACT_FN(kRowPi1GetMainGamemode, "dev.pi1_selftest", L"lib_C", L"getMainGamemode", Declared);
UE_FACT_FN(kRowPi1GetGameMode, "dev.pi1_selftest", L"GameplayStatics", L"GetGameMode", Declared);
UE_FACT_FN(kRowPi1SetControlRotation, "dev.pi1_selftest", L"Controller", L"SetControlRotation", Declared);
UE_FACT_FN(kRowPi1MakeLookAt, "dev.pi1_selftest", L"mainPlayer_C", L"makeLookAt", Declared);

constexpr int kTagPoints = 0x50493141;  // 'PI1A'
constexpr int kTagGmg    = 0x50493142;  // 'PI1B'
constexpr int kMaxReads  = 8;
constexpr uint8_t kOpLocalVirtualFunction = 0x45;   // getPoints calls getMainGamemode by name, locally
const char* const kHold = "pi1_selftest";

enum class Stage : uint8_t { Wait, Settle, Done };
Stage g_stage = Stage::Wait;
bool  g_host = false;

// What the installs took, so a check can say which one a missing line belongs to.
struct Installed {
    bool watchPoints = false, watchGmg = false, post = false, pre = false;
};
Installed g_inst;

// Two owners for ScopeBitFor: the addresses are the identities.
int g_ownerA = 0;
int g_ownerB = 0;

void* g_local = nullptr;       // this peer's local player actor
void* g_libCdo = nullptr;      // lib_C's default object, which the library's static functions run on
void* g_fnPoints = nullptr;    // lib_C::getPoints
void* g_fnGmg = nullptr;       // lib_C::getMainGamemode
void* g_fnGameMode = nullptr;  // GameplayStatics::GetGameMode
void* g_fnSetRot = nullptr;    // Controller::SetControlRotation
int32_t g_newRotOff = -1;      // NewRotation's offset in SetControlRotation's parameters

// The callbacks' switches and records.
bool      g_collect = false;   // record the reads of the call the test is making
bool      g_mark = false;      // getPoints' PRE marks its scope
bool      g_refuse = false;    // the SetControlRotation pre hook refuses
uintptr_t g_bit = 0;
int       g_preSeen = 0;       // SetControlRotation calls the pre hook saw
ue_wrap::FRotator g_lastSeen;  // the NewRotation it read last

struct PreRead  { bool fromBody; uintptr_t bits; };
struct PostRead { void* caller; int32_t offset; uintptr_t bits; };
PreRead  g_pres[kMaxReads];
int      g_nPres = 0;
PostRead g_posts[kMaxReads];
int      g_nPosts = 0;

void Pass(const char* check) { UE_LOGI("[PI-1] PASS %s", check); }

void Fail(const char* check, const char* fmt, ...) {
    char why[240];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(why, sizeof why, fmt, ap);
    va_end(ap);
    UE_LOGI("[PI-1] FAIL %s %s", check, why);
}

unsigned long long Ull(uintptr_t v) { return static_cast<unsigned long long>(v); }

// One axis' difference, a full turn apart counting as equal.
float AngleDiff(float a, float b) {
    float d = std::fmod(a - b, 360.f);
    if (d > 180.f) d -= 360.f;
    if (d < -180.f) d += 360.f;
    return std::fabs(d);
}

bool SameRot(const ue_wrap::FRotator& a, const ue_wrap::FRotator& b, float tol = 1e-3f) {
    return AngleDiff(a.Pitch, b.Pitch) < tol && AngleDiff(a.Yaw, b.Yaw) < tol && AngleDiff(a.Roll, b.Roll) < tol;
}

// The tolerance the value the pre hook read is judged by.
constexpr float kReadTol = 0.01f;

// A value the selftest never passes, so a hook that read nothing does not match (a roll of -9999 is 81 degrees
// under the full-turn compare, and the player's roll is 0).
void ClearSeen() { g_lastSeen.Pitch = g_lastSeen.Yaw = g_lastSeen.Roll = -9999.f; }

// A library function of lib_C that takes the world context and writes an out value: called on the library's
// default object with this peer's local player as the context.
bool CallLib(void* fn) {
    ue_wrap::ParamFrame f(fn);
    if (!f.valid() || !f.Set<void*>(L"__WorldContext", g_local)) return false;
    return ue_wrap::Call(g_libCdo, f);
}

// ---- the callbacks ----

// A PRE on getPoints: marks the scope its body is about to get, then calls getMainGamemode itself, a
// dispatch nested inside the PRE.
sg::Verdict OnPointsPre(const sg::Call& c) {
    if (!g_collect) return sg::Verdict::Run;
    if (g_mark) sg::MarkScope(c, g_bit);
    CallLib(g_fnGmg);
    return sg::Verdict::Run;
}

// A PRE on getMainGamemode: what the scope of getPoints reads at its entry, and whether getPoints' body made
// the call.
sg::Verdict OnGmgPre(const sg::Call& c) {
    if (g_collect && g_nPres < kMaxReads)
        g_pres[g_nPres++] = PreRead{c.callerFunction == g_fnPoints, sg::ScopeBits(g_fnPoints)};
    return sg::Verdict::Run;
}

// The post hook on GetGameMode, which getMainGamemode calls: the call's end offset in its caller, and the
// scope of getPoints read from a native seam.
void OnGetGameModePost(void*, void*, void*) {
    const UH::CallerFrame frame = UH::CurrentCallerFrame();
    if (!g_collect || frame.function != g_fnGmg || g_nPosts >= kMaxReads) return;
    g_posts[g_nPosts++] = PostRead{frame.function, frame.codeOffset, sg::ScopeBits(g_fnPoints)};
}

// The pre hook on SetControlRotation: counts the call, reads NewRotation, and refuses while the flag is set.
sg::Verdict OnSetRotPre(void*, void*, const uint8_t* parms) {
    ++g_preSeen;
    if (g_newRotOff >= 0 && parms) std::memcpy(&g_lastSeen, parms + g_newRotOff, sizeof(g_lastSeen));
    return g_refuse ? sg::Verdict::Cancel : sg::Verdict::Run;
}

// ---- readiness and installs ----

bool Ready(coop::net::Session* session) {
    PL::Registry& reg = PL::Registry::Get();
    g_host = session->role() == coop::net::Role::Host;
    const bool up = g_host ? (reg.LocalPeerId() == PL::kPeerIdHost && reg.GetPlayerElement(PL::kPeerIdHost))
                           : coop::net_pump::HasAnnouncedWorldReady();
    return up && ue_wrap::object_index::ClassByName(L"lib_C") && reg.Local();
}

void Install() {
    sg::Acquire(kHold);
    g_inst.watchPoints = sg::WatchName(kRowPi1GetPoints, kTagPoints, &OnPointsPre, nullptr);
    g_inst.watchGmg = sg::WatchName(kRowPi1GetMainGamemode, kTagGmg, &OnGmgPre, nullptr);

    const ue_wrap::facts::Resolved gameMode = ue_wrap::facts::Resolve(kRowPi1GetGameMode);
    g_fnGameMode = gameMode.Fn();
    g_inst.post = UH::InstallPostHook(gameMode, &OnGetGameModePost, /*armed=*/false);

    const ue_wrap::facts::Resolved setRot = ue_wrap::facts::Resolve(kRowPi1SetControlRotation);
    g_fnSetRot = setRot.Fn();
    if (g_fnSetRot) {
        for (const auto& p : R::FunctionParams(g_fnSetRot)) {
            UE_LOGI("[PI-1] SetControlRotation parameter '%ls' flags 0x%llX offset %d size %d", p.name.c_str(),
                    static_cast<unsigned long long>(p.flags), p.offset, p.size);
            if (p.name == L"NewRotation") g_newRotOff = p.offset;
        }
    }
    g_inst.pre = UH::InstallPreHook(setRot, &OnSetRotPre, /*armed=*/false);
}

bool Settled() {
    return (!g_inst.watchPoints || sg::NameWatchSettled(kRowPi1GetPoints, kTagPoints)) &&
           (!g_inst.watchGmg || sg::NameWatchSettled(kRowPi1GetMainGamemode, kTagGmg));
}

// ---- the checks ----

bool PhaseOk(int phase, uintptr_t wantBody) {
    if (g_nPres != 2 || g_nPosts != 2) {
        Fail("scope-mark", "phase %d saw %d PRE reads and %d post reads, want 2 and 2", phase, g_nPres, g_nPosts);
        return false;
    }
    bool ok = true;
    if (g_pres[0].fromBody || !g_pres[1].fromBody) {
        Fail("scope-mark", "phase %d: the PRE reads are not [nested call, getPoints' body call]", phase);
        ok = false;
    }
    if (g_pres[0].bits != 0) {
        Fail("scope-mark", "phase %d: the nested call's PRE read 0x%llX, want 0", phase, Ull(g_pres[0].bits));
        ok = false;
    }
    if (g_pres[1].bits != wantBody) {
        Fail("scope-mark", "phase %d: the body call's PRE read 0x%llX, want 0x%llX", phase, Ull(g_pres[1].bits),
             Ull(wantBody));
        ok = false;
    }
    if (g_posts[0].bits != 0) {
        Fail("scope-mark", "phase %d: the nested call's post hook read 0x%llX, want 0", phase, Ull(g_posts[0].bits));
        ok = false;
    }
    if (g_posts[1].bits != wantBody) {
        Fail("scope-mark", "phase %d: the body call's post hook read 0x%llX, want 0x%llX", phase,
             Ull(g_posts[1].bits), Ull(wantBody));
        ok = false;
    }
    return ok;
}

// One getPoints call with the reads recorded; false when the call itself failed.
bool RunPhase(bool mark) {
    g_nPres = 0;
    g_nPosts = 0;
    g_mark = mark;
    g_collect = true;
    const bool called = CallLib(g_fnPoints);
    g_collect = false;
    g_mark = false;
    return called;
}

void CheckScopeMark() {
    if (!g_inst.watchPoints || !g_inst.watchGmg) {
        Fail("scope-mark", "a watch was refused");
        return;
    }
    if (!sg::NameWatchLive(kRowPi1GetPoints, kTagPoints) || !sg::NameWatchLive(kRowPi1GetMainGamemode, kTagGmg)) {
        Fail("scope-mark", "a watch settled dead");
        return;
    }
    if (!g_inst.post) {
        Fail("scope-mark", "the post hook install on GetGameMode was refused");
        return;
    }
    if (!g_fnPoints || !g_fnGmg || !g_libCdo) {
        Fail("scope-mark", "lib_C's getPoints or getMainGamemode did not resolve");
        return;
    }
    bool ok = true;
    g_bit = sg::ScopeBitFor(&g_ownerA);
    const uintptr_t again = sg::ScopeBitFor(&g_ownerA);
    const uintptr_t other = sg::ScopeBitFor(&g_ownerB);
    if (g_bit == 0) {
        Fail("scope-mark", "ScopeBitFor gave an owner no bit");
        return;
    }
    if (again != g_bit) {
        Fail("scope-mark", "ScopeBitFor gave one owner 0x%llX and then 0x%llX", Ull(g_bit), Ull(again));
        ok = false;
    }
    if (other == 0 || other == g_bit) {
        Fail("scope-mark", "ScopeBitFor gave a second owner 0x%llX beside the first's 0x%llX", Ull(other),
             Ull(g_bit));
        ok = false;
    }
    if (!RunPhase(/*mark=*/true)) {
        Fail("scope-mark", "phase 1: the getPoints call failed");
        return;
    }
    ok = PhaseOk(1, g_bit) && ok;
    if (!RunPhase(/*mark=*/false)) {
        Fail("scope-mark", "phase 2: the getPoints call failed");
        return;
    }
    ok = PhaseOk(2, 0) && ok;
    if (ok) Pass("scope-mark");
}

// How many of `fn`'s calls name `callee`, and whether each of them is listed once; -1 when `fn` does not decode.
int CountCallee(void* fn, void* callee, bool& eachOnce) {
    std::vector<SC::CallSite> sites;
    if (!SC::EnumerateCalls(fn, sites)) return -1;
    std::set<int32_t> ends;
    int n = 0;
    eachOnce = true;
    for (const SC::CallSite& s : sites) {
        if (!SC::SameCallee(s, callee)) continue;
        ++n;
        if (!ends.insert(s.end).second) eachOnce = false;
    }
    return n;
}

void CheckCallSites() {
    bool ok = true;
    std::vector<SC::CallSite> sites;
    if (!g_fnPoints || !g_fnGmg || !g_fnGameMode) {
        Fail("call-sites", "getPoints, getMainGamemode or GetGameMode did not resolve");
        return;
    }
    if (!SC::EnumerateCalls(g_fnPoints, sites) || sites.size() != 1) {
        Fail("call-sites", "getPoints lists %zu calls, want 1", sites.size());
        ok = false;
    } else {
        if (!SC::SameCallee(sites[0], g_fnGmg)) {
            Fail("call-sites", "getPoints' call is not getMainGamemode");
            ok = false;
        }
        if (sites[0].op != kOpLocalVirtualFunction) {
            Fail("call-sites", "getPoints' call is op 0x%02X, want 0x%02X", sites[0].op, kOpLocalVirtualFunction);
            ok = false;
        }
    }

    const SC::CallSite* gameModeCall = nullptr;
    if (!SC::EnumerateCalls(g_fnGmg, sites)) {
        Fail("call-sites", "getMainGamemode does not decode");
        ok = false;
    } else {
        for (const SC::CallSite& s : sites)
            if (SC::SameCallee(s, g_fnGameMode)) { gameModeCall = &s; break; }
        if (!gameModeCall) {
            Fail("call-sites", "getMainGamemode lists no GetGameMode call");
            ok = false;
        } else if (gameModeCall->start != 10 || gameModeCall->end != 29) {
            Fail("call-sites", "the GetGameMode call is [%d, %d), want [10, 29)", gameModeCall->start,
                 gameModeCall->end);
            ok = false;
        }
    }
    if (gameModeCall) {
        // The position arm makes and records its own getPoints call.
        if (!g_inst.post) {
            Fail("call-sites", "position: the post hook install on GetGameMode was refused");
            ok = false;
        } else if (!g_libCdo || !RunPhase(/*mark=*/false)) {
            Fail("call-sites", "position: the getPoints call failed");
            ok = false;
        } else {
            if (g_nPosts == 0) {
                Fail("call-sites", "position: no post hook read was recorded");
                ok = false;
            }
            for (int i = 0; i < g_nPosts; ++i)
                if (g_posts[i].offset != gameModeCall->end) {
                    Fail("call-sites", "position: the post hook stood at %d, the call ends at %d", g_posts[i].offset,
                         gameModeCall->end);
                    ok = false;
                }
        }
    }

    void* playerClass = ue_wrap::object_index::ClassByName(L"mainPlayer_C");
    void* uber = playerClass ? R::FindFunction(playerClass, L"ExecuteUbergraph_mainPlayer") : nullptr;
    void* teleport = playerClass ? R::FindFunction(playerClass, L"teleportWObackrooms") : nullptr;
    void* lookAt = ue_wrap::facts::Resolve(kRowPi1MakeLookAt).Fn();
    if (!uber || !teleport || !lookAt || !g_fnSetRot) {
        Fail("call-sites", "mainPlayer_C's ubergraph, makeLookAt, teleportWObackrooms or SetControlRotation did "
             "not resolve");
        ok = false;
    } else {
        struct Want { const char* name; void* fn; int count; };
        const Want wants[] = {{"the ubergraph", uber, 5}, {"makeLookAt", lookAt, 1}, {"teleportWObackrooms", teleport, 1}};
        for (const Want& w : wants) {
            bool once = true;
            const int n = CountCallee(w.fn, g_fnSetRot, once);
            if (n != w.count || !once) {
                Fail("call-sites", "%s lists %d SetControlRotation calls (each once: %d), want %d", w.name, n, once,
                     w.count);
                ok = false;
            }
        }
    }
    if (ok) Pass("call-sites");
}

// The local player's camera-lag component's world location, which makeLookAt's one other effect moves toward.
bool LagLocation(ue_wrap::FVector& out) {
    const int32_t off = R::FindPropertyOffset(R::ClassOf(g_local), L"lag");
    if (off < 0) return false;
    void* comp = *reinterpret_cast<void**>(static_cast<uint8_t*>(g_local) + off);
    if (!comp) return false;
    void* declaring = nullptr;
    void* fn = R::FindDispatchFunction(R::ClassOf(comp), L"K2_GetComponentLocation", &declaring);
    if (!fn) return false;
    ue_wrap::ParamFrame f(fn);
    if (!ue_wrap::Call(comp, f)) return false;
    out = f.Get<ue_wrap::FVector>(L"ReturnValue");
    return true;
}

// What makeLookAt's non-seated branch needs of the local player: the name of the condition that does not hold,
// else null. Its interpolation speed is worldDeltaSeconds, and a zero speed returns the target, so no delta is
// required.
const char* LookAtPrecondition() {
    void* sitting = nullptr;
    if (!E::ReadMainPlayerSittingOn(g_local, sitting)) return "sittingOn unreadable";
    if (sitting) return "the player is sitting";
    return nullptr;
}

// The pre hook on SetControlRotation, on both routes: a reflected call (the engine_pawn call, parameters read
// from the frame) and the bytecode route (makeLookAt calls it from its own script). The player's control
// rotation is read before and after each, and put back at the end.
void CheckConstRef(const ue_wrap::FRotator& rot0, void* controller) {
    if (!g_inst.pre) {
        Fail("const-ref-pre", "install refused");
        Fail("const-ref-bytecode", "no pre hook");
        return;
    }
    if (!controller || !g_fnSetRot) {
        Fail("const-ref-pre", "no controller for the local player");
        Fail("const-ref-bytecode", "no controller for the local player");
        return;
    }
    ue_wrap::FRotator off = rot0;
    off.Yaw += 90.f;
    UH::SetArmed(g_fnSetRot, &OnSetRotPre, true);

    bool preOk = true;
    g_refuse = true;
    g_preSeen = 0;
    ClearSeen();
    E::SetControlRotation(controller, off);
    const ue_wrap::FRotator refused = E::GetControlRotation(controller);
    UE_LOGI("[PI-1] const-ref-pre: the hook saw %d call(s), NewRotation read (%.1f, %.1f, %.1f)", g_preSeen,
            g_lastSeen.Pitch, g_lastSeen.Yaw, g_lastSeen.Roll);
    if (!SameRot(refused, rot0)) {
        Fail("const-ref-pre", "not refused: (%.1f, %.1f, %.1f) became (%.1f, %.1f, %.1f)", rot0.Pitch, rot0.Yaw,
             rot0.Roll, refused.Pitch, refused.Yaw, refused.Roll);
        preOk = false;
    }
    if (!SameRot(g_lastSeen, off, kReadTol)) {
        Fail("const-ref-pre", "value (%.2f, %.2f, %.2f) want (%.2f, %.2f, %.2f)", g_lastSeen.Pitch, g_lastSeen.Yaw,
             g_lastSeen.Roll, off.Pitch, off.Yaw, off.Roll);
        preOk = false;
    }
    g_refuse = false;
    E::SetControlRotation(controller, off);
    if (SameRot(E::GetControlRotation(controller), rot0)) {
        Fail("const-ref-pre", "passed nothing: the call with the flag cleared left the rotation as it was");
        preOk = false;
    }
    if (preOk) Pass("const-ref-pre");

    // The bytecode route: makeLookAt with its camera location at the lag component's current one.
    E::SetControlRotation(controller, rot0);
    ue_wrap::FVector lag;
    void* makeLookAt = ue_wrap::facts::Resolve(kRowPi1MakeLookAt).Fn();
    if (!makeLookAt || !LagLocation(lag)) {
        Fail("const-ref-bytecode", "makeLookAt or the lag component did not resolve");
        return;
    }
    const ue_wrap::FVector cam = E::GetCameraLocation();
    const float yaw = (rot0.Yaw + 90.f) * 3.14159265f / 180.f;
    const ue_wrap::FVector look{cam.X + 1000.f * std::cos(yaw), cam.Y + 1000.f * std::sin(yaw), cam.Z};
    ue_wrap::ParamFrame f(makeLookAt);
    if (!f.valid() || !f.Set<ue_wrap::FVector>(L"lookAtLocation", look) ||
        !f.Set<ue_wrap::FVector>(L"cameraLocation", lag)) {
        Fail("const-ref-bytecode", "makeLookAt's parameters did not set");
        return;
    }
    if (const char* why = LookAtPrecondition()) {
        Fail("const-ref-bytecode", "precondition %s", why);
        return;
    }
    // The rotation an unrefused call produces from rot0, which is what the refused call's hook must read.
    g_refuse = false;
    ClearSeen();
    const bool calledFree = ue_wrap::Call(g_local, f);
    const ue_wrap::FRotator expected = E::GetControlRotation(controller);
    E::SetControlRotation(controller, rot0);
    if (!calledFree) {
        Fail("const-ref-bytecode", "unseen: the unrefused makeLookAt call failed");
        return;
    }
    g_refuse = true;
    ClearSeen();
    const int before = g_preSeen;
    const bool called = ue_wrap::Call(g_local, f);
    g_refuse = false;
    const int seen = g_preSeen - before;
    const ue_wrap::FRotator after = E::GetControlRotation(controller);
    if (!called) Fail("const-ref-bytecode", "unseen: the makeLookAt call failed");
    else if (!SameRot(after, rot0))
        Fail("const-ref-bytecode", "changed: (%.1f, %.1f, %.1f) became (%.1f, %.1f, %.1f), the hook saw %d", rot0.Pitch,
             rot0.Yaw, rot0.Roll, after.Pitch, after.Yaw, after.Roll, seen);
    else if (seen < 1) Fail("const-ref-bytecode", "unseen: makeLookAt reached SetControlRotation %d time(s) through the hook", seen);
    else if (!SameRot(g_lastSeen, expected, kReadTol))
        Fail("const-ref-bytecode", "value (%.2f, %.2f, %.2f) want (%.2f, %.2f, %.2f)", g_lastSeen.Pitch, g_lastSeen.Yaw,
             g_lastSeen.Roll, expected.Pitch, expected.Yaw, expected.Roll);
    else {
        UE_LOGI("[PI-1] const-ref-bytecode: the hook saw %d call(s), NewRotation read (%.1f, %.1f, %.1f)", seen,
                g_lastSeen.Pitch, g_lastSeen.Yaw, g_lastSeen.Roll);
        Pass("const-ref-bytecode");
    }
}

void CheckCallerLocals() {
    if (!CL::SelftestPassed()) {
        Fail("caller-locals", "the boot selftest did not pass");
        return;
    }
    CL::HitLayout layout;
    if (!CL::ResolveHitLayout(layout) || layout.actor < 0 || layout.component < 0 ||
        layout.size < layout.actor + 8 || layout.size < layout.component + 8) {
        Fail("caller-locals", "the HitResult layout did not resolve (actor %d, component %d, size %d)", layout.actor,
             layout.component, layout.size);
        return;
    }
    UE_LOGI("[PI-1] caller-locals: HitResult size %d, Actor at %d, Component at %d", layout.size, layout.actor,
            layout.component);
    Pass("caller-locals");
}

void RunChecks() {
    PL::Registry& reg = PL::Registry::Get();
    g_local = reg.Local();
    g_libCdo = R::FindClassDefaultObject(L"lib_C");
    void* libClass = ue_wrap::object_index::ClassByName(L"lib_C");
    g_fnPoints = libClass ? R::FindFunction(libClass, L"getPoints") : nullptr;
    g_fnGmg = libClass ? R::FindFunction(libClass, L"getMainGamemode") : nullptr;

    void* controller = g_local ? E::GetController(g_local) : nullptr;
    const ue_wrap::FRotator rot0 = controller ? E::GetControlRotation(controller) : ue_wrap::FRotator{};

    UH::SetArmed(g_fnGameMode, &OnGetGameModePost, true);
    CheckScopeMark();
    CheckCallSites();
    CheckConstRef(rot0, controller);
    CheckCallerLocals();

    // Teardown: everything the run put in place comes out, and the player's rotation goes back.
    sg::UnwatchName(kRowPi1GetPoints, kTagPoints, &OnPointsPre, nullptr);
    sg::UnwatchName(kRowPi1GetMainGamemode, kTagGmg, &OnGmgPre, nullptr);
    UH::SetArmed(g_fnGameMode, &OnGetGameModePost, false);
    UH::SetArmed(g_fnSetRot, &OnSetRotPre, false);
    g_refuse = false;
    if (controller) E::SetControlRotation(controller, rot0);
    sg::Release(kHold);
    UE_LOGI("[PI-1] DONE %s", g_host ? "host" : "client");
}

}  // namespace

void Tick(coop::net::Session* session) {
    static const bool s_on = coop::config::ResolveFlag(::coop::config_registry::rows::pi1_selftest);
    if (!s_on || !session || g_stage == Stage::Done) return;
    if (g_stage == Stage::Wait) {
        if (!Ready(session)) return;
        Install();
        g_stage = Stage::Settle;
        return;
    }
    sg::ResolvePendingNames();
    if (!Settled()) return;
    g_stage = Stage::Done;
    RunChecks();
}

}  // namespace coop::dev::pi1_selftest
