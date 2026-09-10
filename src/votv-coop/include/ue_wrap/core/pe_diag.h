// ue_wrap/core/pe_diag.h -- the PE double-detour DIAGNOSTIC (probe; RULE 2 exempt).
//
// Proves or refutes the followJmp-divert hypothesis for the boot crash WITHOUT needing the
// ~20% crash. The divert is STRUCTURAL: if UE4SS's PolyHook detours ProcessEvent AFTER our
// MinHook, its followJmp resolves our E9 and re-points its patch onto OUR DETOUR body -- so
// the snapshot pair (install / ~10 s post-init) makes the fix's presence or the baseline
// corruption visible on any NORMAL boot. Inert unless VOTVCOOP_PE_DIAG=1.
//
// Extracted from pe_detour.cpp when that file went over the 800-line soft cap: this block was
// banner-commented, off the hot path, and consumed only at Install().

#pragma once

namespace ue_wrap::pe_diag {

// Arm if VOTVCOOP_PE_DIAG=1: log the install-time hook-chain snapshot (PE
// prologue / our detour prologue / our trampoline + the relay verdict +
// WHO-FIRST) and schedule the post-init snapshot ~10 s later. No-op otherwise.
// The two pointers are pe_detour.cpp's ProcessEventDetour and its MinHook
// trampoline, passed in because both are TU-local there; both are written once
// at hook install and never change afterwards (Uninstall disables, never
// frees), so arm-time value capture is equivalent to the pre-extraction
// read-at-snapshot-time.
void ArmIfEnabled(void* detourAddr, void* trampolineAddr);

}  // namespace ue_wrap::pe_diag
