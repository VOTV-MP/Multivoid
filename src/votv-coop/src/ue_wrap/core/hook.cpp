#include "ue_wrap/core/hook.h"

#include "ue_wrap/core/log.h"

#include <MinHook.h>
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>

namespace ue_wrap::hook {
namespace {

// The facade's armed state, not whether MinHook is initialised: Shutdown clears this and
// deliberately leaves MinHook initialised, because uninitialising frees trampolines the
// process is still calling through (see hook.h). One flag, not two: a second could disagree
// with this one, and then neither is authority. The ordering is load-bearing: Shutdown
// clears it before lifting patches, so Enable's post-enable re-read cannot miss a teardown
// that started mid-call.
std::atomic<bool> g_live{false};

// The retirement latch, one-way, never cleared. The live flag answers two questions that
// diverge after a teardown: whether MinHook initialised and whether a new patch may arm.
// Shutdown never uninitialises MinHook, so Init would answer the first with yes, and an
// install after the teardown would resurrect the facade and arm a fresh patch nothing would
// ever lift, since the shutdown latch never runs a second time; the live path is the capture
// hook thread, un-joined, which calls Install from outside the shutdown ordering. Two flags
// that cannot disagree harmfully: this is a monotonic latch, not a mirror of the live flag.
std::atomic<bool> g_retired{false};

const char* StatusName(MH_STATUS s) { return MH_StatusToString(s); }

// The follow-jmp-immune relay rewrite. On x64 MinHook always routes a patched target through
// a relay (an indirect jump through an absolute pointer) inside the 64-byte trampoline slot.
// A co-resident inline-hook engine that follows jmp chains (UE4SS ships one) hooking the
// same function after us takes our target's jump into this relay, sees the indirect branch,
// resolves its destination to the operand address (the relay's pointer slot) and writes its
// own patch there, clobbering the detour pointer; our relay then jumps through a garbage
// pointer to a non-canonical address, a general-protection fault surfaced as an access
// violation at -1 (proven from a full crash dump). The fix rewrites the relay's leading
// instruction to a non-branching form (mov rax, imm64; jmp rax): the follower stops on the
// mov, hooks the relay itself in place, and both detours chain. Absolute-jump semantics are
// identical; only the byte encoding the follower keys on changes. Safe because it runs
// before the enable: the target is still unpatched, so nothing executes the relay yet.
// Fail-closed: if the expected relay signature is not in the slot, leave it untouched
// (MinHook's layout changed; surface it rather than guess).
bool MakeRelayFollowJmpImmune(void* trampoline, void* detour) {
    if (!trampoline || !detour) return false;
    auto* base = static_cast<uint8_t*>(trampoline);
    const uint64_t want = reinterpret_cast<uint64_t>(detour);
    uint8_t* relay = nullptr;
    // The relay lives inside the 64-byte trampoline slot; scan for the classic MinHook relay
    // signature whose absolute target is our detour.
    for (int off = 0; off + 14 <= 64; ++off) {
        if (base[off] == 0xFF && base[off + 1] == 0x25 && base[off + 2] == 0x00 &&
            base[off + 3] == 0x00 && base[off + 4] == 0x00 && base[off + 5] == 0x00) {
            uint64_t p = 0;
            std::memcpy(&p, base + off + 6, sizeof(p));
            if (p == want) { relay = base + off; break; }
        }
    }
    if (!relay) {
        UE_LOGE("hook: immune-relay: FF25 relay not found in trampoline slot -- "
                "MinHook layout changed? leaving relay as-is (fail-closed)");
        return false;
    }
    DWORD oldProt = 0;
    if (!VirtualProtect(relay, 14, PAGE_EXECUTE_READWRITE, &oldProt)) {
        UE_LOGE("hook: immune-relay: VirtualProtect(RWX) failed on relay %p", relay);
        return false;
    }
    uint8_t buf[14];
    buf[0] = 0x48; buf[1] = 0xB8;                 // mov rax, imm64
    std::memcpy(buf + 2, &want, sizeof(want));    //   = &detour
    buf[10] = 0xFF; buf[11] = 0xE0;               // jmp rax
    buf[12] = 0x90; buf[13] = 0x90;               // pad to the 14-byte relay footprint
    std::memcpy(relay, buf, sizeof(buf));
    DWORD tmp = 0;
    VirtualProtect(relay, 14, oldProt, &tmp);
    FlushInstructionCache(GetCurrentProcess(), relay, 14);
    UE_LOGI("hook: immune-relay: relay @%p rewritten to MOV RAX,&detour/JMP RAX "
            "(followJmp-immune)", relay);
    return true;
}

}  // namespace

bool Init() {
    // Retirement outranks MinHook's own opinion: the initialise call reports already-initialised
    // forever, and that answer once undid a completed Shutdown.
    if (g_retired) return false;
    if (g_live) return true;
    const MH_STATUS s = MH_Initialize();
    // Already-initialised is success here, not an error: Shutdown clears the live flag without
    // uninitialising MinHook, so the two states legitimately disagree after a teardown, and
    // MinHook is the one telling the truth about its own heap.
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) {
        UE_LOGE("hook: MH_Initialize failed (%s)", StatusName(s));
        return false;
    }
    g_live = true;
    UE_LOGI("hook: MinHook initialized%s",
            s == MH_ERROR_ALREADY_INITIALIZED ? " (already; re-arming the facade)" : "");
    return true;
}

bool Install(void* target, void* detour, void** trampoline, bool followJmpImmune) {
    if (!g_live && !Init()) return false;
    if (!target || !detour || !trampoline) {
        UE_LOGE("hook: Install called with null target/detour/trampoline");
        return false;
    }
    MH_STATUS s = MH_CreateHook(target, detour, trampoline);
    if (s != MH_OK) {
        UE_LOGE("hook: MH_CreateHook(%p) failed (%s)", target, StatusName(s));
        return false;
    }
    // Make the relay follow-jmp-immune while the target is still unpatched (before the enable,
    // so thread-safe). `*trampoline` is the slot base; the relay lives inside it. Best effort: a
    // failure is logged and non-fatal (the classic relay still works absent a co-resident
    // jmp-following hook engine).
    if (followJmpImmune) {
        MakeRelayFollowJmpImmune(*trampoline, detour);
    }
    s = MH_EnableHook(target);
    if (s != MH_OK) {
        UE_LOGE("hook: MH_EnableHook(%p) failed (%s)", target, StatusName(s));
        // The one legitimate hook removal in this process, and the gate
        // (.github/ci/minhook_free_gate.ps1) allowlists exactly this line. Removing frees the
        // trampoline, a use-after-free anywhere the hook is live, but the enable just failed, so
        // the target was never patched and no thread can be inside the trampoline or holding a
        // pointer into it. Leaving a created-but-disabled hook behind would leak the slot instead.
        MH_RemoveHook(target);
        return false;
    }
    // Compare after act, the same contract Enable documents below, and the reason this exists:
    // the entry guard is check-then-act, Install is reachable from threads that never
    // synchronise with the game thread, and an arm that lands after Shutdown's blanket disable
    // would survive with no second teardown to lift it. Teardown wins in every interleaving.
    if (g_retired) {
        MH_DisableHook(target);
        UE_LOGW("hook: install of %p raced Shutdown -- lifted again (teardown wins)", target);
        return false;
    }
    UE_LOGI("hook: installed on %p (trampoline %p)", target, *trampoline);
    return true;
}

bool Disable(void* target) {
    if (!g_live || !target) return false;
    const MH_STATUS s = MH_DisableHook(target);
    if (s != MH_OK) {
        UE_LOGW("hook: MH_DisableHook(%p) (%s)", target, StatusName(s));
        return false;
    }
    UE_LOGI("hook: disabled %p (trampoline slot retained on purpose)", target);
    return true;
}

bool Enable(void* target) {
    if (!g_live || !target) return false;
    const MH_STATUS s = MH_EnableHook(target);
    if (s != MH_OK) {
        UE_LOGW("hook: MH_EnableHook(%p) re-arm (%s)", target, StatusName(s));
        return false;
    }
    // Compare after act. The guard above is check-then-act on its own: this is reachable from
    // the render thread (the capture re-arm) while the game thread is inside Shutdown, so a
    // teardown can begin between the guard and the enable and we would re-arm a patch Shutdown
    // had just lifted. Shutdown sets the latch before its blanket disable, so re-reading it here
    // catches every interleaving: either we see the latch and lift our own patch, or Shutdown's
    // blanket runs after our enable and lifts it; both orders end disabled, as they do in
    // Install. Lock-free on purpose: Shutdown is reachable from process detach under the loader
    // lock, where a mutex owned by a thread Windows already killed never unlocks.
    if (g_retired) {
        MH_DisableHook(target);
        UE_LOGW("hook: re-arm of %p raced Shutdown -- lifted again (teardown wins)", target);
        return false;
    }
    UE_LOGI("hook: re-enabled %p", target);
    return true;
}

void Shutdown() {
    // The latch goes first and unconditionally: before the early return, so a Shutdown that
    // arrives before anything was installed still retires the facade, and before the blanket
    // disable, so a concurrent arm re-reads it.
    g_retired = true;
    if (!g_live.exchange(false)) return;   // one-way; also the double-Shutdown guard
    // Latching retirement first is the whole ordering contract (see Enable and Install): an arm
    // that slips past its own entry guard re-reads the latch after MinHook returns and undoes
    // itself. Lift every patch, free nothing: removing a hook or uninitialising both free
    // trampoline slots, and MinHook writes a free-list pointer over a slot's first bytes as it
    // does so, over the stolen prologue a thread may be about to return through; measured, this
    // runs seconds before process detach, so a dying process does not close that window, and
    // the OS reclaims the slots at exit. A known residual, pre-existing: the blanket disable
    // reaches MinHook's thread freeze, a toolhelp snapshot plus thread suspends, which on the
    // process-detach path runs under the loader lock, a documented deadlock risk. Never
    // observed; the graceful-close path does not exercise it, since the window procedure latches
    // shutdown first and the detach call is the idempotent no-op.
    MH_DisableHook(MH_ALL_HOOKS);
    UE_LOGI("hook: all patches lifted (trampolines retained -- MinHook stays initialized)");
}

}  // namespace ue_wrap::hook
