// coop/dev/cord_drill_detail.h -- what the cord drill's files share: its bounds, the cast (makers and sockets as this
// peer holds them), the acting player's hand, and the chat edges. Impl-private: nothing outside coop/dev includes it.
// Game thread.

#pragma once

#include "coop/dev/director/aim_fan.h"
#include "coop/dev/director/director.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/types.h"

#include <cstdint>
#include <memory>
#include <string>

namespace coop::net { class Session; }

namespace coop::dev::cord_drill::detail {

// ---- bounds: failure bounds of the instrument, never a wait for a milestone ----

// An arm not closed this long after its bound starts: the peer that waited prints FAIL. The bound starts at the arm's
// opening edge, or, in an arm whose acting peer walks, when that peer has arrived (a walk is its own phase).
inline constexpr uint64_t kArmFailMs = 60000;
// A walk (the first stand, the cast's arrival, an arm's own walk), which the director's own deadline bounds (+ a margin).
inline constexpr int      kWalkDeadlineS = 300;
inline constexpr uint64_t kStageFailMs = (static_cast<uint64_t>(kWalkDeadlineS) + 30) * 1000;

// ---- the geometry of the recipe (pr50cordm drill10) ----

inline constexpr float kStandReachCm = 40.f;   // the director's reach to a stand point
inline constexpr float kStandOutCm = 100.f;    // the stand point: this far out along the socket's plug arrow;
                                               // the use trace ends 200 uu from the camera (armLength), and at
                                               // 150 the camera stood 219 uu from the socket (run cord3)
inline constexpr float kPeerSideCm = 45.f;    // the two peers stand this far to either side of a socket's stand point
                                               // (the host to maker A's side, the client to B's), so neither walks
                                               // into the other's body (run cord8: the client's walk never arrived)
inline constexpr float kKneeCm = 50.f;         // a stand's maker lines start this far above its nav point
inline constexpr float kStandSlipCm = 50.f;    // the stand point's nav projection may lie this far from it
inline constexpr float kTraceReachCm = 180.f;  // the eye to the socket, inside the use trace's 200 (armLength)
inline constexpr float kSideCm = 80.f;         // a maker's spot: this far to the side of the stand point
inline constexpr float kClusterCm = 350.f;     // sockets 2 and 3 stand within this of socket 1's stand point:
                                               // a plugged cord breaks at 460 uu from its root (300 x 1.2 + 100,
                                               // cord.cpp), the makers stand 80 from socket 1's stand; at 200
                                               // no cluster of reachable sockets remained (run cord6)
inline constexpr float kLoneCm = 0.f;         // a cast socket has no other socket within this: off, nearly every
                                               // wall socket is one of a pair 10 uu apart (run cord10: lone=12 at
                                               // 25); the aim tells the pair apart instead (kTwinCm)
inline constexpr float kTwinCm = 40.f;         // the aim releases nearer the target than its twin within this
inline constexpr float kTwinBiasCm = 3.f;      // the aim point sits this far past the target, away from the twin
inline constexpr float kMakerRiseCm = 700.f;   // arms 6 and 7: the scripted move of one peer's copy of maker A

inline constexpr int kMakers = 2;   // A, B
inline constexpr int kSocks = 3;    // 1, 2, 3
inline constexpr int kArms = 7;

// What both peers' copies must show when arm n has closed: the socket each maker is in (0 none), and what the
// `sees` line says. The joiner's expectation is arm 4's end: A in 1, B unplugged.
struct Expect {
    int a, b;
    const char* sees;
};
inline constexpr Expect kExpect[kArms + 1] = {
    {0, 0, ""},        {1, 0, "A in 1"},       {1, 2, "B in 2"},      {1, 0, "B unplugged"},
    {1, 0, "A in 1"},  {1, 2, "B in 2"},       {1, 2, "A in 1"},      {0, 2, "A unplugged"},
};
inline constexpr Expect kJoinerExpect = {1, 0, "A in 1, B unplugged"};

// ---- the cast (cord_drill_cast.cpp) ----

struct Cast {
    ue_wrap::CachedObjRef maker[kMakers];
    ue_wrap::CachedObjRef sock[kSocks];
    ue_wrap::FVector makerAt[kMakers]{};   // the host's spawn spots
    std::wstring makerKey[kMakers], sockKey[kSocks];
};
Cast& TheCast();
void ResetCast();

void* Maker(int m);          // live corded prop or null
void* CordOfMaker(int m);    // live cord or null
void* Sock(int k);           // live socket or null
// The socket maker m is in as 1..3, 0 unplugged, 9 a socket outside the cast, -1 when the maker is not here.
int LinkOf(int m);
bool Matches(const Expect& e);
// "A in 1, B unplugged" -- this peer's copies, for a line.
std::string LinkText();

// A component of an actor by its property name, null when it is not there (reflection; the recipe's reads).
void* FieldOf(void* obj, const wchar_t* name);
// A free socket's world location and its plug arrow's forward; false when either does not read.
bool SocketGeometry(void* socket, ue_wrap::FVector& at, ue_wrap::FVector& fwd);
// Which peer stands: the host on the +perpendicular side of a socket's plug arrow (maker A's spot), the client on the
// other. Both peers compute their stand points from the socket's own place and the nav mesh, so they agree.
enum class Peer : uint8_t { Host, Client };
// The nominal stand: out along the plug arrow, nav-projected (within kStandSlipCm of the arrow's point); false when none.
bool NominalStand(void* player, const ue_wrap::FVector& at, const ue_wrap::FVector& fwd, ue_wrap::FVector& nav);
// A peer's stand: the nominal offset kPeerSideCm sideways, nav-projected again. `stand` is the walk's target (at the
// socket's height), `nav` the projected point a reach test stands an eye on; false when the projection slips.
bool PeerStand(void* player, const ue_wrap::FVector& at, const ue_wrap::FVector& fwd, const ue_wrap::FVector& nominal,
               Peer who, ue_wrap::FVector& stand, ue_wrap::FVector& nav);
// The walk target of a peer at a free socket; false when the socket's place or a projection does not read.
bool StandOf(void* player, void* socket, Peer who, ue_wrap::FVector& stand);

// ---- the acting player's hand (cord_drill_hand.cpp) ----

// One piece of cord work, a step per tick: walk to a stand by the director; press through the cord's own
// actionOptionIndex (the function the E press ends in); hold, or aim at a socket's Sphere (sock >= 0) or into the air
// (sock < 0) and release with the use key's stub.
class Hand {
public:
    enum class Mode : uint8_t { WalkOnly, Plug, Grab };
    enum class St : uint8_t { Idle, Walk, Press, Held, Aim, Release, Done, Stuck };
    struct Plan {
        Mode mode = Mode::WalkOnly;
        const char* who = "";            // the peer's name, for the lines
        void* cord = nullptr;            // Plug, Grab
        ue_wrap::FVector stand{};        // walked to when `walk`
        bool walk = false;
        int sock = -1;                   // Plug: the socket 0..2
    };
    void Start(const Plan& plan);
    St Tick(void* player);
    St state() const { return st_; }
    // From Held: aim at socket `sock` (0..2) or into the air (-1), and release.
    void Resume(int sock);
    // The walk, when one is in flight, ends.
    void Cancel();
    const char* stuckWhat() const { return stuckText_.c_str(); }

private:
    void Next(St s);
    St Stick(const char* at, const char* what, const std::string& saw = std::string());
    St TickAim(void* player);
    Plan plan_{};
    St st_ = St::Idle;
    std::shared_ptr<coop::director::BackgroundWalk> walk_;
    coop::director::AimFan fan_;
    int airTicks_ = 0;
    std::string stuckText_;   // why the hand stuck, with what it saw when that was an aim
    void* twin_ = nullptr;    // the target socket's nearest other socket within kTwinCm, read once per aim
    bool twinRead_ = false;
    float bestCm_ = -1.f;     // the aim's smallest head-to-socket Sphere distance so far, -1 before the first read
};

// ---- the chat edges (cord_drill.cpp) ----

// A line both peers watch for, latched on sight (a live row retires after a few more lines). Heard is true once the
// row was seen live; Rest is the text after a prefix, from the newest live row that carried it.
bool Heard(const std::string& line);
bool Rest(const std::string& prefix, std::string* rest);
void Say(const std::string& line);
// The lines' names, so both roles say and watch the same words.
std::string ArmLine(int n);            // host: arm n opens
std::string HostSeenLine(int n);       // host: its copies show arm n's links
std::string ClientSeenLine(int n);     // client: the same
std::string WalkedLine(int n);         // client: its walk in arm n has ended
// A narrow key for a line and back (keys are ASCII).
std::string Narrow(const std::wstring& w);
std::wstring Wide(const std::string& s);
// The text after `tag` up to the next space, from a line's rest.
std::string Token(const std::string& rest, const char* tag);

// A failure line, once; `arm` names the phase. Prints this peer's links.
void FailLine(const char* peer, const char* arm, const char* why);

uint64_t NowMs();

// ---- the roles (cord_drill_host.cpp, cord_drill_client.cpp) ----

void HostTick(coop::net::Session* s);
// The session's end on the host. True when the host is past its seven arms and the client has left: the cast and the
// phase stay for the rejoin (the host's session lives on); otherwise the drill starts over.
bool HostOnDisconnect();
void ClientTick(coop::net::Session* s);
void ClientReset();

}  // namespace coop::dev::cord_drill::detail
