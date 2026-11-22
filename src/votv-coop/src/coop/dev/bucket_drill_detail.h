// coop/dev/bucket_drill_detail.h -- what the bucket drill's files share (bucket_drill.cpp, _stage, _spots, _walk, _verbs, _concrete, _tick,
// _drop, _join, _alc, _watch). Dev only; see coop/dev/bucket_drill.h.

#pragma once

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/types.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace coop::net { class Session; }

namespace coop::dev::bucket_drill::detail {

inline constexpr int kBoundTicks = 3600;   // a step that waits this long for its event says INVALID and stops
inline constexpr int kQuietTicks = 30;     // a prop is at rest after this many quiet ticks in a row
inline constexpr float kShovedCm = 5.f;   // a dropped prop that leaves its place by more than this (level) was pushed by something
inline constexpr int kReadTicks  = 180;    // a settle window after an event, counted in ticks
inline constexpr const char* kPrefix = "bkd ";
inline constexpr uint8_t kSlot = 1;        // the one client the drill acts on

enum class Arm : uint8_t { Off, Soap, Dip, Mix, Trowel, Transfer, Pin, Visuals, Ctick, Drop, Join, Mop, Alc, Brew };
Arm ArmOf();
const char* ArmName(Arm a);   // the verdict's name: `rig: VERDICT bucket_<arm> ...`

// ---- the log, the chat, the verdict and the bound ----
void Fact(const char* fmt, ...);
void Say(const std::string& line);
bool Seen(const std::string& line);
bool Rest(const std::string& prefix, std::string* rest);   // the text after `prefix` of the last chat line that starts with it
bool ParseInts(const std::string& text, int n, int* out);  // n integers off the front of `text`; false when one is missing
std::string Ints(std::initializer_list<int> v);            // the integers joined by spaces
// True when the step's bound ran out (the step has said INVALID). A wait on the other peer's verdict-bearing line takes a longer bound
// than the other peer's own wait for its event, so the peer that acts says its result first.
bool Late(const char* why, int boundTicks = kBoundTicks);
void Invalid(const char* why);  // the drill could not measure
void Fail(const char* fmt, ...);   // it measured a failure: said where it is decided, nothing after it runs
void Pass(const char* why);
void Finish();                  // this peer has nothing more to say (the host says the verdict)
bool Ended();
void Go();                      // a step began: its bound starts again
void NameVerdict(const char* arm);   // the verdict's arm name, `bucket_<arm>`, when another drill runs the stage (hostile_drill_bucket)

// ---- reads ----
int          Centi(float v);                      // v * 100 rounded: a float told in whole hundredths
int          EidOf(void* actor);                  // the element id, 0 when none
void*        LiveProp(int eid);
std::wstring KeyOf(void* actor);
void*        ByKey(const std::wstring& key);      // the actor that holds the key now, null when none
void*        Local();                             // this peer's own player
void*        Puppet();                            // the host's puppet of the client, null on a client or before it stands
bool         Quiet(void* actor);                  // one read: the body's velocity is below the ground helper's bound
bool         Calm(void* actor, int& quiet);       // Quiet kQuietTicks ticks in a row, counted in `quiet`
bool         HandEmpty();                         // this peer's hand actor is gone
std::vector<void*> LiveOf(const wchar_t* cls);
void*        Nearest(const wchar_t* cls, void* to, float maxCm);   // the live instance of cls nearest to `to`, null past maxCm
float        Dist(void* a, void* b);              // -1 when a place could not be read
bool         HostMayStart(coop::net::Session& s);
bool         ClientMayStart();

// ---- the player's own verbs ----
bool Hold(void* player, void* item);                          // the hand key's `Hold Object` on the item
bool HandReady();                                             // the hand holds an actor, announced when this peer is a client
bool UseOn(void* player, void* target);                       // the target's playerUsedOn with the hand actor and its name
bool HandUse(void* player, void* item, void* target, bool rmb);   // the item's click with the player's hit written on target
bool DropHeld(void* player);                                  // simulateDrop, not into the inventory
// The camera turned round a point near the target's rim (its bounds centre raised by 80% of its half height) until the player's own trace
// names the target: 1 once it does, 0 working (or the arm said INVALID: the bounds could not be read), -1 once every pose of the fan was
// held without it (`why` names the camera, its distance and what the trace names; nothing is said INVALID).
int  Aim(void* player, void* target, std::string& why);
void AimReset();

// ---- the client's approach to a staged prop (bucket_drill_walk.cpp) ----
// Called each tick before a client's verb on `target` (a bucket or a bag standing in the world): 0 working (a walk, or the camera's
// turn), 1 the camera is near the target and turned to it, so the verb runs now, -1 the arm has said INVALID naming the target.
int  Approach(void* player, void* target);
// The aim at the approach's standpoint failed: the next standpoint is walked to (INVALID, named, once none is left).
void ApproachRetry(const std::string& why);
void ApproachReset();

// ---- the host's stage: the buckets filled on the water, carried and set down before the client's puppet ----
void* SpawnAt(const wchar_t* name, float x, float y, float z);
void* SpawnBefore(const wchar_t* name, void* who, float fwdCm, float rightCm);   // a prop of /Game/objects/<name> before `who`

struct Spec {
    int plains = 0;                  // water buckets set down (the first is a mop bucket when `mop`)
    bool mop = false;
    const wchar_t* items[4] = {};    // props spawned behind the puppet, by class name without `_C`
    int nItems = 0;
    bool nearHost = false;           // placed round the host's own player (no client yet: the join arm)
    float itemFwdCm = -150.f;        // the items stand this far before the anchor, behind it when negative (ItemClearOfSpots refuses a spec that stands one within kClearOfBucketCm of a place)
    float itemSpreadCm = 60.f;       // and this far from one another
};
// The game's own place of a held prop: hold it, enter the place mode, set the ghost at each of `spots` in turn until the game's
// test reads it free, drop; the new actor under the key is accepted once it has been calm for kQuietTicks ticks in a row. A new
// actor that leaves the chosen place by more than kShovedCm is said INVALID at once, naming what stands beside it. `item` is read on
// the first call, the job starts over for each prop. 0 working, 1 placed (`placed`, `at` the place it was dropped at), -1 stopped.
struct PlaceJob {
    int sub = 0;
    void* old = nullptr;
    void* placed = nullptr;
    std::wstring key;
    ue_wrap::FRotator rot;   // the placement ghost's rotation: level unless the caller sets it before the first call
    ue_wrap::FVector at{};   // the place the drop was made at
    int calm = 0;            // the new actor's quiet ticks in a row
};
int PlaceHeld(PlaceJob& j, void* item, const ue_wrap::FVector* spots, int nSpots);

// The places the stage offers the game's own place (bucket_drill_spots.cpp).
inline constexpr int kSpotCount = 47;              // the candidate table's size
inline constexpr float kClearOfBucketCm = 100.f;   // a place within this of a bucket already set down is not tried, nor an item spawned within it of a place: two buckets touch at 83-92 cm
struct OfferIn {
    void* anchor = nullptr;                // the client's puppet, or the host's own player for the join arm
    const ue_wrap::FVector* placed = nullptr;   // where each bucket already set down stands
    int nPlaced = 0;
};
struct Offer {
    ue_wrap::FVector spots[kSpotCount];   // the places offered, in the table's order
    int n = 0;                            // how many
    int crowded = 0, noFloor = 0;         // the candidates refused: near a bucket set down, no floor under the place
    const char* broken = nullptr;         // why the floor could not be judged (a trace failed); the stage says INVALID
};
// The candidates for `in.anchor`, filtered: not within kClearOfBucketCm (level) of a bucket set down, and with level geometry under the
// place. The game's own place test (cantPlacePrec) judges the rest, in PlaceHeld. False when a place could not be read.
bool  OfferSpots(const OfferIn& in, Offer& out);
bool  ItemClearOfSpots(float fwd, float side);      // an item stood there is clear of every candidate

constexpr int kMaxPlains = 4;
constexpr int kMaxItems = 4;
void  StageBegin(const Spec& spec);
bool  StageStep(coop::net::Session& s);          // true once every bucket stands filled and at rest and every item has an id
void* StageBucket(int i);                        // the bucket now standing under its key, null when none
void* StageItem(int i);
void  StageReset();
void  StageSetPlains(int n);                     // a bucket the preparation used up is no longer staged
std::wstring StageBucketKey(int i);
std::wstring StageItemKey(int i);
// "stage <plains> <items> <eids>": the bucket ids then the item ids, said once every one has an id and is at rest. False until then.
bool  StageSay();
// The client's copies, named by the host's stage line: true once every one stands here and is at rest.
bool  ClientBindStage(int nPlains, int nItems);
void* ClientBucket(int i);
void* ClientItem(int i);
int   ClientEid(int slot);                       // slots 0..kMaxPlains-1 the buckets, then the items
void  ClientReset();
void  ClientRebind(int slot, int eid);           // a later line names another element for the slot

// The host's own hold and use on a prop, as one step machine: hold, wait the hand, use, then a tail.
struct HostUse {
    int ph = 0;
    int tail = 0;
    ue_wrap::CachedObjRef item, target;
    bool rmb = false, soap = false;
};
// 0 working, 1 the use ran (the hand may still hold the item), -1 a step could not run (the job has said why).
int HostUseStep(HostUse& j);
// Drops what the host holds and waits the hand empty: true once empty. `ph` is the caller's phase counter for it.
bool HostDropStep(int& ph);

// ---- the arms (each: a host half and a client half, called every tick until the arm has said its end) ----
void SoapHost(coop::net::Session& s);   void SoapClient();
void DipHost(coop::net::Session& s);    void DipClient();
void TransferHost(coop::net::Session& s);   void TransferClient();
void MixHost(coop::net::Session& s);    void MixClient();
void TrowelHost(coop::net::Session& s); void TrowelClient();
void PinHost(coop::net::Session& s);    void PinClient();
void VisualsHost(coop::net::Session& s);   void VisualsClient();
void MopHost(coop::net::Session& s);    void MopClient();
void DropHost(coop::net::Session& s);   void DropClient();
void JoinHost(coop::net::Session& s);   void JoinClient();
void AlcHost(coop::net::Session& s);    void AlcClient();
void ResetArms();   // calls each file's reset
void ResetVerbs();  void ResetConcrete();  void ResetTickArms();  void ResetDrop();  void ResetJoin();  void ResetAlc();

// The tick and the reset (bucket_drill.cpp).
void TickImpl(coop::net::Session& s);
void Reset();

// The host's counters of the client's commits.
uint32_t CommitsTaken();
uint32_t Applies();
uint32_t Refusals();   // the host's answers to the client that were not grants

// One ask of the client judged by the host: the counters at its start, then the wait for the commit.
struct Round {
    uint32_t taken = 0, applies = 0, refusals = 0;
};
Round RoundBegin();
// 0 waiting (the bound runs), 1 the commit applied, -1 the drill has said FAIL (a refusal, or a commit that did not apply).
int RoundStep(const Round& r, const char* what);
// The client: the host's line `<prefix> <eid> <seq> ...` has come and the row of that seq has reached the copy of `eid`. `out`
// takes the line's first `n` integers (eid, seq, then the host's own readings).
bool RowPast(const char* prefix, int n, int* out, bool bounded = true);   // unbounded: the host's own bound ends the wait (the brew)
// The client: no request to the host is open (its replay ran and its commit left).
bool RecordClosed();
bool PendingIdOpen();   // a request to the host is open on this client

// The drill-owned ReceiveTick POST watch on the client's plain bucket copy (bucket_drill_watch.cpp).
// `gap0`: the drawn water's distance from the water at the entry of the first tick after the target row was applied (-1 before that tick).
struct TickSample { unsigned long long posts = 0; float height = 0, interp = 0, gap0 = -1.f; bool empty = false; bool good = false; };
bool  WatchStart();                    // register once; false when the gate took none
bool  WatchSettled();                  // every watch of the drill is live (a dead one has said INVALID)
// Samples from the next POST of `copy`. The visuals judge takes the copy's element id: its entries note each row applied to it, and its
// POSTs count from WatchTarget's row on.
void  WatchArm(void* copy, float wantHeight, bool judgeVisuals, uint32_t eid = 0);
void  WatchTarget(uint32_t seq);       // the visuals judge: the row of this seq (or a later one) is the one the draw must follow
TickSample WatchRead();
void  WatchReset();

// The drill-owned POST watch on prop_bucket_C deleteAlc: the gate runs a body's POST only when the body ran, so a call the lane cancelled
// leaves no POST. The alc arm's client arms it on its copy, calls deleteAlc, and reads the POSTs.
bool  AlcWatchSettled();               // registers on the first call; true once live (a dead one has said INVALID)
void  AlcWatchArm(void* copy);
unsigned long long AlcPosts();         // POSTs of the armed copy since AlcWatchArm

}  // namespace coop::dev::bucket_drill::detail
