// coop/dev/hand_record_drill_detail.h -- what the held item's record drill's files share: a can's state read from an
// actor, the lobby-chat marks, the forged record's builder, and the step state and verdict verbs the can arm
// (hand_record_drill_can.cpp) uses. See coop/dev/hand_record_drill.h.

#pragma once

#include "coop/session/rig_verdict.h"

#include <cstdint>
#include <string>

namespace coop::net { class Session; }

namespace coop::dev::hand_record_drill::detail {

inline constexpr const wchar_t* kCanClass = L"prop_cannedFood_0_C";
inline constexpr const wchar_t* kCanRow   = L"fcan_p";   // list_props' row of the can, its name
inline constexpr const wchar_t* kBoxClass = L"prop_orderBox_C";  // an order box: its getData writes a classes[] group
inline constexpr const wchar_t* kBoxRow   = L"orderbox";         // the box's own name
inline constexpr const wchar_t* kDiscClass = L"prop_floppyDisc_R_C";  // a disc: its loadData runs prop's init, physics on
inline constexpr const wchar_t* kDiscRow   = L"floppy_0";             // list_props' row of the red disc

// A can's two fields as an actor holds them; -1 when it has no such field.
struct Can {
    int opened = -1;
    int foods = -1;
    bool operator==(const Can& o) const { return opened == o.opened && foods == o.foods; }
};
Can ReadCan(void* actor);
std::string Fmt(const Can& c);                       // "opened=1 foods=3"
bool ParseCan(const std::string& text, Can& out);    // the two fields anywhere in `text`

// The actor's own function `fn`, with `player` when the function takes one. True when it ran.
bool CallOn(void* actor, const wchar_t* fn, void* player);

// The local hand's actor when it is a can (or, HeldBox, an order box with its key set), else null.
void* HeldCan();
void* HeldBox();

// What a forged record is wrong with: its class (`fireExt`, a class the hand does not hold), a class it names that
// resolves to none, no key, a size over the lane's ceiling, one string past the field bounds, a non-finite number.
enum class Forgery : uint8_t { Class, UnknownClass, Keyless, Oversize, Bounds, NonFinite };

// A forged record sent to the host as this client's, built from the can in its hand and sent through the blob layer as the
// lane would. True when the blob was sent.
bool SendForged(coop::net::Session& session, Forgery what);

// ---- Defined in hand_record_drill.cpp: the step state and the verbs every arm uses --------------------------------

inline constexpr const char* kHead = "[HANDREC-DRILL] ";
inline constexpr const char* kC    = "HREC c1 ";   // the client's lines
inline constexpr const char* kH    = "HREC h ";    // the host's lines

extern coop::net::Session* g_s;
extern int g_step;          // this peer's step in its half of the arm; 99 is done
extern std::wstring g_key;  // the can's Key
extern uint32_t g_c0;       // a counter at the step's start
extern uint32_t g_c1;
extern Can g_mark;          // the state a mark carried

void Next(int step);
bool Expired();
std::string ClientLine(const std::string& what);
std::string HostLine(const std::string& what);
void HostEnds(coop::rig_verdict::Outcome o, const char* why);   // HOST: the one verdict
void ClientReport(const char* word, const char* why);           // CLIENT: what stopped this half, told to the host
void Fail(const char* why);
void Abandon(const char* why);
bool ClientReady();
bool PutCanInHand();
bool PrepareHeldCan(void* can);
bool AtRest(void* actor);   // a copy that moved less than half a centimetre since the last check

// ---- Defined in hand_record_drill_can.cpp -------------------------------------------------------------------------

void ClientCan(void* player);
void HostCan();
void ResetCan();            // session end: the can arm's marks

// ---- Defined in hand_record_drill_display.cpp ---------------------------------------------------------------------

void ClientDisplay();
void HostDisplay();

}  // namespace coop::dev::hand_record_drill::detail
