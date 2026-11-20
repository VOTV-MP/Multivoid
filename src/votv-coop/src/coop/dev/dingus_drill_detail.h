// coop/dev/dingus_drill_detail.h -- what the dingus drill's files share (dingus_drill.cpp, _host, _client). Dev only; see
// coop/dev/dingus_drill.h.

#pragma once

#include "ue_wrap/core/cached_obj_ref.h"

#include <cstdint>
#include <memory>
#include <string>

namespace coop::director { class AimedGrab; }
namespace coop::net { class Session; }

namespace coop::dev::dingus_drill::detail {

inline constexpr int kBoundTicks = 3600;   // a step that waits this long for its event says INVALID and stops
inline constexpr int kQuietTicks = 30;     // a spawned target is at rest after this many quiet ticks in a row
inline constexpr int kHandTicks  = 180;    // the hand takes a held dingus within this many ticks of the verb's answer
inline constexpr const char* kPrefix = "dgd ";
inline constexpr const char* kTag = "dingus";   // the drill's name on its `rig: VERDICT` line

enum class Arm : uint8_t { Off, Find, Pocket, Loaded, Join };
Arm ArmOf();

// ---- the log, the chat, the verdict and the bound ----
void Fact(const char* fmt, ...);
void Say(const std::string& line);
bool Seen(const std::string& line);
bool Rest(const std::string& prefix, std::string* rest);   // the text after `prefix` of the last chat line that starts with it
bool ParseInts(const std::string& text, int n, int* out);   // n integers off the front of `text`; false when one is missing
bool Late(const char* why);                                // true when the step's bound ran out (the step has said INVALID)
void Invalid(const char* why);
void Fail(const char* fmt, ...);
void Pass(const char* why);
bool Ended();                                              // a verdict was said, or this peer is finished
void Go();                                                 // a step began: its bound starts again
void Finish();                                             // this peer has nothing more to do (the host says the verdict)

// ---- reads of a copy ----
std::wstring KeyOf(void* d);
int  EidOf(void* actor);
void* LiveProp(int eid);
// What a copy reads: Quiet is isUsed set and the music off, Loud any other pair of reads, Unread a read the instrument could not
// take (-1): the drill ends INVALID on it, never FAIL.
enum class Reading : uint8_t { Quiet, Loud, Unread };
Reading ReadingOf(int found, int music);
std::string Narrow(const std::wstring& ascii);
std::wstring Widen(const std::string& ascii);
void LogPlaced();                                          // the live copies of each of the four classes in this world

// ---- targets ----
void* SpawnDingus(void* who, float fwdCm, float rightCm, const wchar_t* cls);
bool  Quiet(void* const* actors, int n);                      // every one live and at rest for kQuietTicks ticks in a row
bool  HostMayStart(coop::net::Session& s);
bool  ClientMayStart();

// ---- the lane's two reads, taken at its own edges (dingus_sync::dev::Observer) ----
// A copy's key, element id and what it read as the lane finished with it: on the host after the drain of its birth, on a
// client after its bind line.
struct Rec {
    std::wstring key;
    int eid = 0, found = -1, music = -1;
    Reading Read() const { return ReadingOf(found, music); }
};
void ArmObserver();
// The first record of `key` (any key when it is empty) whose element id is neither `exceptEid` (the original copy's) nor
// `alsoExceptEid` (0: none); FindBound with `onlyEid` set gives the one of that id.
const Rec* FindBorn(const std::wstring& key, int exceptEid, int alsoExceptEid = 0);
const Rec* FindBound(const std::wstring& key, int exceptEid, int onlyEid);

// ---- one grab, as the use key does it, then the let-go ----
// Grabbed once the let-go has emptied the player's grabbing actor, so the next verb finds a free hand.
class GrabRun {
public:
    enum class R { Working, Grabbed, Failed };
    GrabRun();
    void Begin(void* player, void* prop);
    R Tick();
    const char* Why() const { return why_; }
    bool NoAim() const;
    ~GrabRun();
private:
    std::unique_ptr<coop::director::AimedGrab> grab_;
    void* player_ = nullptr;
    bool letGo_ = false;
    const char* why_ = "";
};

// The player's pocket verb on a loose dingus (`Hold Object`, no grab), then the drop that does not go to the inventory.
class PocketRun {
public:
    enum class R { Working, Dropped, Failed };
    void Begin(void* player, void* target, bool client);
    R Tick();
    const char* Why() const { return why_; }
private:
    ue_wrap::CachedObjRef player_, target_;
    bool client_ = false;
    int stage_ = 0, ticks_ = 0;
    const char* why_ = "";
};

// ---- the tick and each peer's steps ----
void HostTick(coop::net::Session& s);
void ClientTick();
void ResetHost();
void ResetClient();
void ArmFirstTick();   // the chat history and the lane's reads, started with the first tick
void ResetAll();

}  // namespace coop::dev::dingus_drill::detail
