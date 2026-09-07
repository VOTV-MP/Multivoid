// ue_wrap/core/pe_diag.cpp -- see ue_wrap/core/pe_diag.h.

#include "ue_wrap/core/pe_diag.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace ue_wrap::pe_diag {
namespace {

// The two pe_detour.cpp TU-locals the snapshots read, captured at arm time.
void* g_detourAddr = nullptr;
void* g_trampolineAddr = nullptr;

// ---- PE double-detour DIAGNOSTIC (probe; RULE 2 exempt) ------------------------
//
// Proves or refutes the followJmp-divert hypothesis for the boot crash WITHOUT needing the crash
// itself. The divert is STRUCTURAL: if UE4SS's PolyHook detours ProcessEvent AFTER our MinHook, its
// followJmp resolves our E9 and re-points its patch onto OUR DETOUR body, so our detour's prologue
// is overwritten in place -- observable on any NORMAL boot. Snapshots the whole hook chain (the PE
// prologue, our detour prologue, our trampoline) at install and again ~10 s later, past UE4SS init.
// Inert unless VOTVCOOP_PE_DIAG=1.

bool PeDiagEnabled() {
    char v[8] = {};
    const DWORD n = ::GetEnvironmentVariableA("VOTVCOOP_PE_DIAG", v, sizeof(v));
    return n > 0 && v[0] == '1';
}

std::string HexBytes(const void* p, size_t n) {
    std::string s;
    const auto* b = static_cast<const uint8_t*>(p);
    char buf[4];
    for (size_t i = 0; i < n; ++i) {
        // Read defensively: the pages are code (present), but guard anyway.
        std::snprintf(buf, sizeof(buf), "%02x ", b[i]);
        s += buf;
    }
    return s;
}

// Classify the first byte of a prologue: is it an inline-detour jmp (someone
// hooked it) or the real function body?
const char* JmpKind(const uint8_t* p) {
    if (p[0] == 0xE9) return "E9-rel32-jmp";
    if (p[0] == 0xFF && p[1] == 0x25) return "FF25-riprel-jmp";
    if (p[0] == 0xEB) return "EB-short-jmp";
    if ((p[0] == 0x48 || p[0] == 0x49) && (p[1] == 0xB8 || p[1] == 0xBB)) return "mov-imm64+jmp";
    return "not-a-jmp(real-body?)";
}

void LogHookChainSnapshot(const char* when) {
    auto* pe    = reinterpret_cast<uint8_t*>(reflection::ProcessEventAddr());
    auto* det   = reinterpret_cast<uint8_t*>(g_detourAddr);
    auto* tramp = reinterpret_cast<uint8_t*>(g_trampolineAddr);
    UE_LOGI("pe_diag[%-9s] PE      %p : %s | first=%s", when, (void*)pe,
            pe ? HexBytes(pe, 16).c_str() : "(null)", pe ? JmpKind(pe) : "?");
    UE_LOGI("pe_diag[%-9s] detour  %p : %s | first=%s", when, (void*)det,
            det ? HexBytes(det, 16).c_str() : "(null)", det ? JmpKind(det) : "?");
    // 48 bytes: covers the trampoline (0x14) + the MinHook relay opcode (0x14..0x19)
    // + the relay's abs64 target pointer (0x1A..0x21) -- the exact 8 bytes PolyHook's
    // followJmp overwrites when UE4SS hooks PE after us.
    UE_LOGI("pe_diag[%-9s] tramp   %p : %s", when, (void*)tramp,
            tramp ? HexBytes(tramp, 48).c_str() : "(null)");
    // Classify the relay. It lives inside the trampoline slot BEHIND MinHook's own jump-back stub
    //   (FF25 00000000 + abs64 -> PE+len(stolen)), which shares the legacy relay's encoding, so a
    //   blind first-match scan reads the jump-back and mislabels every boot. The abs64 payload is
    //                                  what discriminates: only the relay targets &detour. Locate
    //                                  it ONCE at the install snapshot, before anything has patched
    //                                  it, remember the offset, and classify THAT offset in every
    //                                  later snapshot: FF25 00000000 <&detour>      LEGACY-RELAY
    //                                  INTACT FF25 00000000 <other>        LEGACY-RELAY CORRUPT --
    //                                  PolyHook clobbered the pointer slot, the old double-detour
    //                                  crash
    //   48 B8 <&detour> FF E0        IMMUNE-RELAY INTACT (UE4SS has not armed its PE
    //                                  hook this session)
    //   48 B8 <other> FF E0          IMMUNE-RELAY PTR-MISMATCH
    //   anything else at the offset  POLYHOOK-COMPOSED -- UE4SS in-place hooked our
    //                                  relay, which is the fix working, made visible
    if (tramp) {
        const uint64_t wantDet = reinterpret_cast<uint64_t>(det);
        static int s_relayOff = -1;  // located at the install snapshot, once per session
        if (s_relayOff < 0) {
            for (int off = 0; off + 14 <= 48; ++off) {
                uint64_t p = 0;
                if (tramp[off] == 0xFF && tramp[off + 1] == 0x25 && tramp[off + 2] == 0 &&
                    tramp[off + 3] == 0 && tramp[off + 4] == 0 && tramp[off + 5] == 0) {
                    std::memcpy(&p, tramp + off + 6, 8);
                    if (p == wantDet) { s_relayOff = off; break; }
                } else if (tramp[off] == 0x48 && tramp[off + 1] == 0xB8 &&
                           tramp[off + 10] == 0xFF && tramp[off + 11] == 0xE0) {
                    std::memcpy(&p, tramp + off + 2, 8);
                    if (p == wantDet) { s_relayOff = off; break; }
                }
            }
        }
        const char* verdict = "UNKNOWN(relay not located at install)";
        if (s_relayOff >= 0) {
            const uint8_t* r = tramp + s_relayOff;
            uint64_t p = 0;
            if (r[0] == 0xFF && r[1] == 0x25 && r[2] == 0 && r[3] == 0 && r[4] == 0 &&
                r[5] == 0) {
                std::memcpy(&p, r + 6, 8);
                verdict = (p == wantDet) ? "LEGACY-RELAY INTACT"
                                         : "LEGACY-RELAY CORRUPT(double-detour hit)";
            } else if (r[0] == 0x48 && r[1] == 0xB8 && r[10] == 0xFF && r[11] == 0xE0) {
                std::memcpy(&p, r + 2, 8);
                verdict = (p == wantDet) ? "IMMUNE-RELAY INTACT(UE4SS not armed on it)"
                                         : "IMMUNE-RELAY PTR-MISMATCH";
            } else {
                verdict = "POLYHOOK-COMPOSED(immune relay in-place hooked -- fix working)";
            }
        }
        UE_LOGW("pe_diag[%-9s] RELAY: %s", when, verdict);
    }
    // WHO-FIRST is decided by what our trampoline HOLDS, not by PE's byte: if the
    // trampoline holds the real PE prologue (40 55 56 57 41 54) we hooked FIRST (PE
    // had real bytes); if it holds an ff25/e9 jmp, UE4SS hooked PE before us.
    const bool weFirst = tramp && tramp[0]==0x40 && tramp[1]==0x55 && tramp[2]==0x56;
    UE_LOGW("pe_diag[%-9s] WHO-FIRST: %s (trampoline holds %s)", when,
            weFirst ? "WE-FIRST (PE had real bytes at our install)"
                    : "UE4SS-FIRST (we relocated its jmp)",
            tramp ? JmpKind(tramp) : "?");
    ue_wrap::log::Flush();
}

DWORD WINAPI PeDiagDelayedThread(LPVOID) {
    ::Sleep(10000);  // past UE4SS init() -> setup_unreal() -> PE detour + slot-2 dispatch
    LogHookChainSnapshot("post-init");
    return 0;
}

}  // namespace

void ArmIfEnabled(void* detourAddr, void* trampolineAddr) {
    if (!PeDiagEnabled()) return;
    g_detourAddr = detourAddr;
    g_trampolineAddr = trampolineAddr;
    LogHookChainSnapshot("install");
    if (HANDLE t = ::CreateThread(nullptr, 0, PeDiagDelayedThread, nullptr, 0, nullptr)) {
        ::CloseHandle(t);
    }
}

}  // namespace ue_wrap::pe_diag
