// coop/dev/clean_drill_detail.h -- co-located private header of the clean drill (clean_drill*.cpp): the decal reads, the peer
// marks and the verdict that its arms share. Not a public header.
#pragma once

#include "coop/session/rig_verdict.h"
#include "ue_wrap/core/types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace coop::net { class Session; }

namespace coop::dev::clean_drill::detail {

using ue_wrap::FVector;

enum class Arm { Off, Clean, Leaves, LeavesJoin };
Arm ArmOf();

// The peers of the drill, by slot: the host and the first two clients (mp.py --clients 2).
inline constexpr int kPeers = 3;
// A phase that ends on no event says INVALID after this: a time bound never decides a pass or a fail.
inline constexpr uint64_t kBoundMs = 300000;

// ---- decals ---------------------------------------------------------------------------------------------------------
// Every live instance of exactly `cls`, the object index drained first.
std::vector<void*> All(const wchar_t* cls);
float Dist2(const FVector& a, const FVector& b);
// The live decal of `cls` standing at `at` (within 1 cm), or null.
void* DecalAt(const std::wstring& cls, const FVector& at);
bool BoolOf(void* obj, const wchar_t* name);
float ProcessOf(void* decal);   // -1 when unread
bool CallNoArgs(void* obj, const wchar_t* fn);
// grime_C::clean(sponge, Sub, noSound): with no sponge, the rain's call form, and silent. True when it ran.
bool Clean(void* decal, void* sponge, float sub);
std::string Narrow(const std::wstring& w);
// `text` cut at `sep` (an empty piece kept), and a float read whole (false when the piece holds anything else).
std::vector<std::string> Split(const std::string& text, char sep);
bool ToFloat(const std::string& piece, float& out);
// The save's bucket the drill's targets are picked around (VOTVCOOP_CLEAN_BUCKET names its key).
bool FindAnchor(FVector& out);

// ---- the peers ------------------------------------------------------------------------------------------------------
int MySlot();
const char* SlotName(int slot);
bool IsHost();
// Whether this peer is the one that acts in the clean arm (VOTVCOOP_CLEAN_ACTOR: `client`, the first client, by default,
// or `host`).
bool IsActor();
// This peer's world is up and past its join (the host: with one client at least).
bool WorldUp(coop::net::Session& s);

// A run's start. Once this peer's world is up it opens the peers' folder (an INVALID verdict if it is unset) and starts the
// first phase's clock; then, while `ready` (the arm's own readiness) holds, it marks this peer up on every tick until it has
// read every other peer's mark, so a peer that began later still reads the last one. 1 = every peer is up, 0 = not yet,
// -1 = the run is over.
int Arrive(coop::net::Session& s, const char* tag, const char* syncName, bool ready, bool barrier = true);
// How many peers an arm waits for in its barrier: the host and the clients up to this count (the default is kPeers).
void SetPeers(int n);
// A mark of this peer, `name_<slot>`, and another peer's.
void MarkMine(const char* name, const std::string& text);
bool PeekOf(const char* name, int slot, std::string& text);
// A one-writer mark with no slot suffix, and its read.
void MarkShared(const char* name, const std::string& text);
bool PeekShared(const char* name, std::string& text);

// The verdict, said once and marked for every other peer: `kind` P, F or I. Over() is true once this peer decided or read
// another's end; a drill stops its steps then. EndKind() is the first letter of the end that was said or read.
void Decide(coop::rig_verdict::Outcome o, const char* fmt, ...);
bool Over();
char EndKind();
// Starts the clock of a phase; Overdue() says INVALID (and returns true) when it ran past kBoundMs.
void Enter(const char* phase);
bool Overdue();

// ---- the arms ---------------------------------------------------------------------------------------------------------
void TickLeaves(coop::net::Session& s);
void TickLeavesJoin(coop::net::Session& s);

}  // namespace coop::dev::clean_drill::detail
