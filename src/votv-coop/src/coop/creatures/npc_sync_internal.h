// coop/creatures/npc_sync_internal.h -- IMPLEMENTATION-PRIVATE shared seam between
// npc_sync.cpp and npc_sync_install.cpp ONLY (s28 cut 2026-07-19).
//
// NOT a public header (lives under src/, not include/): it declares the symbols the install
// TU (npc_sync_install.cpp -- the staged reflection resolve + observer/interceptor
// registration) and the seam TU (npc_sync.cpp -- the ProcessEvent callbacks + runtime state)
// must share but that are NOT part of the public npc_sync.h surface. The five shared globals
// are DEFINED in npc_sync_install.cpp (the writer); the hot-path readers here are the
// interceptor/POST callbacks and the allowlist walk. npc_mirror / npc_pose_host /
// npc_world_enum stay accessor-only consumers of the public header -- do NOT include this.
//
// Same family pattern as coop/props/remote_prop_internal.h.

#pragma once

#include <atomic>
#include <cstdint>

#include "ue_wrap/core/sdk_profile.h"  // kNpcAllowlistSize (binds the array length)

namespace coop::npc_sync {

// -- Shared install-resolved state (written once by Install; read on hot paths) --------------

// NPC UClass* allowlist (resolved from P::name::kNpcAllowlist at install). Read by the
// subclass-aware allowlist walk (IsClassOrDerivedFromAnyAllowlisted) on every interceptor fire.
extern void* g_npcAllowlist[ue_wrap::profile::name::kNpcAllowlistSize];

// BeginDeferredSpawnFromClass param offsets (resolved once at Install). Read per interceptor /
// POST fire on the ProcessEvent-dispatching thread (may be a parallel-anim worker).
extern int32_t g_npcSpawnActorClassParamOff;
extern int32_t g_npcSpawnReturnParamOff;
extern int32_t g_npcSpawnXformParamOff;

// True after Install permanently gave up registering a lifecycle observer (table full). The
// interceptor's host branch gates on it so a partial-lifecycle install never leaks Elements.
extern std::atomic<bool> g_npcSyncDisabledThisProcess;

// -- ProcessEvent callbacks (defined in npc_sync.cpp; registered by Install cross-TU) --------

void NpcSpawn_POST(void* self, void* function, void* params);
void NpcDestroy_PRE(void* self, void* function, void* params);
bool NpcSuppress_Interceptor(void* self, void* params);

}  // namespace coop::npc_sync
