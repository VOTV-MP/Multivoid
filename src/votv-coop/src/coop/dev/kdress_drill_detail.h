// coop/dev/kdress_drill_detail.h -- what the kdress drill's files share (kdress_drill.cpp, _host, _host_arms, _client,
// _client_arms). Dev only; see coop/dev/kdress_drill.h.

#pragma once

#include "ue_wrap/core/cached_obj_ref.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace coop::net { class Session; }

namespace coop::dev::kdress_drill::detail {

inline constexpr int kBoundTicks = 3600;   // a step that waits this long for its event says INVALID and stops
inline constexpr int kQuietTicks = 30;     // a spawned target is at rest after this many quiet ticks in a row
inline constexpr int kReadTicks  = 180;    // a settle window after an event, counted in ticks
inline constexpr const char* kPrefix = "kdr ";
inline constexpr const char* kTag = "kdress";   // the drill's name on its `rig: VERDICT` line

// The one row every arm wears and takes off: a type-0 accessory (glasses on the head) that a client can hold.
inline constexpr const wchar_t* kRow = L"kerfoacc_glasses";

inline constexpr float kCAheadCm = 150.f;    // Omega C stands this far before the client's puppet
inline constexpr float kJAheadCm = 250.f;    // an Omega the host dresses alone stands this far before its own player
inline constexpr float kNearCm   = 400.f;    // an item "beside" an Omega is within this many cm of it

enum class Arm : uint8_t { Off, PutOn, TakeOff, Pat, Born, Join };
Arm ArmOf();

// ---- the log, the chat, the verdict and the bound ----
void Fact(const char* fmt, ...);
void Say(const std::string& line);
bool Seen(const std::string& line);
bool Rest(const std::string& prefix, std::string* rest);   // the text after `prefix` of the last chat line that starts with it
bool ParseInts(const std::string& text, int n, int* out);  // n integers off the front of `text`; false when one is missing
bool Late(const char* why);                // true when the step's bound ran out (the step has said INVALID)
void Invalid(const char* why);             // the drill could not measure: `rig: VERDICT kdress INVALID`
void Fail(const char* fmt, ...);           // it measured a failure: said where it is decided, nothing after it runs
void Pass(const char* why);
void Finish();                             // this peer has nothing more to say (the host says the verdict)
bool Ended();
void Go();                                 // a step began: its bound starts again

// ---- reads of an Omega, the player and the items ----
int          OmegaEid(void* omega);        // the id the lane names it by on this copy, 0 when none
void*        OmegaOf(int eid);             // the live Omega the lane resolves `eid` to on this copy, or null
std::string  DripText(void* omega);        // its `drip` rows joined by commas; "-" when empty, "?" when unread
bool         DripHas(void* omega, const wchar_t* row);
bool         HandEmpty(void* player);      // the player's hold names no live actor
std::string  Narrow(const std::wstring& ascii);
std::wstring Widen(const std::string& ascii);
int          PropEid(void* actor);
void*        LiveProp(int eid);
std::vector<void*> LiveOf(const wchar_t* cls);
std::wstring ItemClass(const wchar_t* row);                 // the class the game spawns for a row, empty while its list is unfilled
int          NearCount(const wchar_t* cls, void* omega);    // the live copies of `cls` within kNearCm of `omega`
// The player's own use of the item in its hand on `omega`: the Omega's playerUsedOn, as the game's use key calls it.
bool         UseOn(void* player, void* omega);

// ---- targets (the host) ----
void* SpawnOmega(void* who, float fwdCm, float rightCm);   // through the host's own spawn path, the lane announces it
bool  AtRest(void* actor);                                 // still for kQuietTicks ticks in a row
bool  HostMayStart(coop::net::Session& s);
bool  ClientMayStart();
// The game has filled its prop list, so a row names a class (waits, bounded, otherwise).
bool  CatalogReady();
// The host's own player puts `kRow` on `omega` with its own hand, as it does with no client in the world.
bool  DressOmega(void* me, void* omega);

// ---- the tick and each peer's steps ----
void TickImpl(coop::net::Session& s);
void Reset();
void HostTick(coop::net::Session& s);
void ResetHost();
void ClientTick();
void ResetClient();

// Omega C, the target of the arms that stand one before the client's puppet, and its element id.
extern ue_wrap::CachedObjRef g_c;
extern int g_eidC;
extern int g_stepTicks;

// The host: waits the joiner, spawns C before its puppet, waits its element id and rest. True once C stands.
bool HostStandC(coop::net::Session& s);
// The client: waits the host's `targets` line and its copy of C bound and at rest. True once it does.
bool ClientBindC();

// The arms' halves.
void PutOnHost(coop::net::Session& s);
void TakeOffHost(coop::net::Session& s);
void PatHost(coop::net::Session& s);
void BornHost(coop::net::Session& s);
void JoinHost(coop::net::Session& s);
void PutOnClient();
void TakeOffClient();
void PatClient();
void BornClient();
void JoinClient();
void ResetHostArms();
void ResetClientArms();

}  // namespace coop::dev::kdress_drill::detail
