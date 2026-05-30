// coop/prop_element_tracker.h -- per-actor lifecycle bookkeeping for keyed
// interactables (Aprop_C + chipPile/clump/trashBitsPile families).
//
// Three independent maintained sets, all keyed by AActor*:
//
//   1. ProcessedInit set
//      Dedupes the "Init POST observer fires twice via a BP Super call"
//      pattern. Entries added on Init POST broadcast, removed on
//      K2_DestroyActor PRE. Cap 16384.
//
//   2. KnownKeyedProps set
//      The maintained live-actor set. Seeded ONCE at Install via a single
//      GUObjectArray walk; thereafter maintained by Init POST (insert) +
//      K2_DestroyActor PRE (evict). Replaces the per-reconnect GUObjectArray
//      walk that the H2-redux work retired on 2026-05-28.
//
//   3. PropElement shadow (the element::Prop owner -- PR-FOUNDATION-3 Inc3)
//      Each entry in KnownKeyedProps gets a parallel coop::element::Prop
//      Element. The Element is OWNED by the shared singleton
//      coop::element::MirrorManager<Prop>::Instance() (the SAME manager
//      remote_prop uses for wire mirrors) -- the bespoke g_propElementsById
//      owner map is retired (RULE 2). This module keeps just ONE bespoke map:
//      g_actorToPropElementId (actor* -> local eid), the reverse lookup the
//      destroy gate + the Init-POST broadcast elementId stamp need. Lifetime
//      mirrors npc_sync: MarkPropElement AllocAndInstall's the Element into
//      the manager (m_mirror=false -> dtor FreeId) + records the reverse
//      entry; UnmarkKnownKeyedProp erases the reverse entry then Take's the
//      Element out of the manager + lets the dtor run outside the lock
//      (ABBA-safe; FreeId acquires element::Registry::m_mutex).
//
// Extracted from prop_lifecycle.cpp 2026-05-29 (M-1 follow-up to prop_synth_key);
// owner-map migrated to MirrorManager<Prop> 2026-05-30 (PR-FOUNDATION-3 Inc3).
// Behavior preserved: same idempotency / cap / overflow-log semantics, same
// in-lock role-read for the host/peer-range allocator decision in
// MarkPropElement, same leaf-mutex lock ordering.
//
// Session pointer:
//   prop_element_tracker holds its OWN cached coop::net::Session* (separate
//   from prop_lifecycle's). prop_lifecycle::SetSession + Install +
//   InstallInventory call this module's SetSession too. This keeps the
//   in-lock role read (audit fix 2026-05-29 prior session: capture role
//   inside the same locked block as the idempotency check; reading it after
//   the lock release would race SetSession / role-change).

#pragma once

#include "coop/element/element.h"

#include <cstddef>
#include <string>

namespace coop::net { class Session; }

namespace coop::prop_element_tracker {

// Cache the session pointer. Mirror of prop_lifecycle::SetSession --
// prop_lifecycle's Install / SetSession / InstallInventory call this too
// so the in-lock role read in MarkPropElement / SeedKnownKeyedProps has
// a live pointer to query.
void SetSession(coop::net::Session* session);

// ---- ProcessedInit dedupe ------------------------------------------------
void MarkProcessedInit(void* actor);
bool HasProcessedInit(void* actor);
void UnmarkProcessedInit(void* actor);

// OnDisconnect helper: clear the set, return the count we dropped + reset
// the overflow-logged latch. Called by prop_lifecycle::OnDisconnect.
size_t ClearProcessedInit();

// ---- KnownKeyedProps maintained set --------------------------------------
void MarkKnownKeyedProp(void* actor);

// Drains BOTH the known set AND the Prop Element shadow (one operation
// because they share the actor-keyed lifecycle). Drain-then-destruct
// pattern: extract shadow under lock, release lock, then let the
// unique_ptr destructor run. ABBA-safe vs element::Registry::m_mutex.
void UnmarkKnownKeyedProp(void* actor);

// ---- Prop Element shadow -------------------------------------------------
// Create a Prop Element for `actor`. Idempotent: no-op if already tracked.
// Reads its OWN cached session pointer inside the lock to determine host
// vs peer allocation range. Names + class name optional (empty wstring
// skips the field set).
void MarkPropElement(void* actor, const std::wstring& key, const std::wstring& cls);

// Look up the host-allocated ElementId for `actor`. Returns kInvalidId if
// not tracked.
coop::element::ElementId GetPropElementIdForActor(void* actor);

// ---- One-shot seed -------------------------------------------------------
// GUObjectArray walk that populates KnownKeyedProps + creates Prop Element
// shadows for every live keyed-interactable. Internal latch; safe to
// call multiple times. Two-phase: phase 1 walks without lock, phase 2
// takes the mutex for bulk insert (avoids holding the mutex for the full
// ~150k object walk).
void SeedKnownKeyedProps();

}  // namespace coop::prop_element_tracker
