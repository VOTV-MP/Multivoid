// coop/dev/build_identity_drill.cpp -- the building identity drill's arming, its world edge and its steps; see
// coop/dev/build_identity_drill.h.

#include "coop/dev/build_identity_drill.h"

#include "build_identity_detail.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/net_clock.h"
#include "coop/net/session.h"
#include "coop/session/host_world_settle.h"
#include "coop/session/net_pump.h"
#include "coop/session/rig_verdict.h"
#include "coop/session/world_load_episode.h"

#include "harness/autotest/peer_signal.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/engine/world_identity.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace coop::dev::build_identity_drill {
namespace {

namespace PS = harness::autotest::peer_signal;
namespace BI = coop::dev::build_identity;
using coop::rig_verdict::Outcome;

constexpr const char* kTag = "build-identity";

enum class Role : uint8_t { Solo, Host, Client };
enum class Phase : uint8_t { WaitEdge, HostKeys1, HostKeys2, HostSettled2, Done };

constexpr uint64_t kClientKeysBoundMs = 300000;  // the host: the client's census is posted within this of its own edge
constexpr uint64_t kRejoinBoundMs = 420000;      // the host: the rejoined client's census, past its leaving, boot and join
constexpr uint64_t kPollMs = 250;                // the host reads its peer's signals this often

Phase       g_phase = Phase::WaitEdge;
bool        g_armed = false;
void*       g_censusWorld = nullptr;
uint64_t    g_phaseMs = 0;
uint64_t    g_polledMs = 0;
std::string g_hostText;  // the host's own census of the level pieces, what the client's is compared with

enum class Arm : uint8_t { Off, Green, KeyReadOff, SettleCountOff };

Arm ArmOf() {
    static const Arm a = [] {
        const std::string v = coop::config::ResolveEnum(::coop::config_registry::rows::build_identity_drill);
        if (v == "green") return Arm::Green;
        if (v == "key_read_off") return Arm::KeyReadOff;
        if (v == "settle_count_off") return Arm::SettleCountOff;
        return Arm::Off;
    }();
    return a;
}

// The rejoined client is a second process of the run: the rig sets the rejoin variable for it alone.
int LifeOf() {
    static const int life = coop::config::ReadEnv("VOTVCOOP_BUILD_IDENTITY_REJOIN") == "1" ? 2 : 1;
    return life;
}

Role RoleOf(const coop::net::Session& session) {
    if (session.running()) return session.role() == coop::net::Role::Host ? Role::Host : Role::Client;
    static const std::string configured = coop::config::ResolveEnum(::coop::config_registry::rows::net_role);
    return configured == "host" ? Role::Host : configured == "client" ? Role::Client : Role::Solo;
}

const char* RoleName(Role r) { return r == Role::Host ? "host" : r == Role::Client ? "client" : "solo"; }

bool EdgeReached(Role role) {
    if (role == Role::Host) return coop::host_world_settle::Settled();
    if (role == Role::Client) return coop::net_pump::HasAnnouncedWorldReady();
    return false;
}

void To(Phase p, uint64_t now) {
    g_phase = p;
    g_phaseMs = now;
}

// A verdict said, and the drill over: a FAIL runs no later step.
template <class... Args>
void End(Outcome o, const char* fmt, Args... args) {
    coop::rig_verdict::Say(kTag, o, fmt, args...);
    g_phase = Phase::Done;
}

// The tokens of a level census, "<class>=<key>" joined by ';'.
std::vector<std::string> Tokens(const std::string& text) {
    std::vector<std::string> out;
    size_t at = 0;
    while (at <= text.size()) {
        const size_t end = text.find(';', at);
        const size_t stop = end == std::string::npos ? text.size() : end;
        if (stop > at) out.push_back(text.substr(at, stop - at));
        if (end == std::string::npos) break;
        at = end + 1;
    }
    return out;
}

// True when the two censuses name the same pieces; else the FAIL is said at `step`, naming the first piece one side
// holds and the other does not.
bool SameKeys(const char* step, const char* what, const std::string& host, const std::string& client) {
    const std::vector<std::string> h = Tokens(host), c = Tokens(client);
    for (const std::string& t : h)
        if (std::find(c.begin(), c.end(), t) == c.end()) {
            End(Outcome::Fail, "%s: the host holds %s that %s does not", step, t.c_str(), what);
            return false;
        }
    for (const std::string& t : c)
        if (std::find(h.begin(), h.end(), t) == h.end()) {
            End(Outcome::Fail, "%s: %s holds %s that the host does not", step, what, t.c_str());
            return false;
        }
    return true;
}

// The edge of this peer's world: the census, i1, and then the role's half.
void AtEdge(Role role, uint64_t now) {
    std::vector<BI::Piece> pieces;
    BI::CollectPieces(pieces);
    const BI::LevelCensus level = BI::SayAndTotal(RoleName(role), pieces);
    if (pieces.empty()) {
        End(Outcome::Invalid, "no nail, wall or poster is live at the %s's world-ready edge: nothing to read", RoleName(role));
        return;
    }
    // The world is judged a New Game's before any key is: a save may hold a piece that is keyless on purpose.
    if (level.walls != 1 || level.posters != 5) {
        End(Outcome::Invalid, "the %s's world is not a New Game's: %d level wall(s) and %d level poster(s), expected 1 and 5",
            RoleName(role), level.walls, level.posters);
        return;
    }
    for (const BI::Piece& p : pieces) {
        if (p.key.empty() || p.key == L"None") {
            std::string where = p.cls + " at " + p.level;
            End(Outcome::Fail, "i1: the %s's %s reads no key", RoleName(role), where.c_str());
            return;
        }
    }
    BI::Info("i1 ok: %s's %zu piece(s) all read a key", RoleName(role), pieces.size());
    if (role == Role::Host) {
        g_hostText = level.text;
        To(Phase::HostKeys1, now);
        return;
    }
    // i4 judges the settle the episode reached on a stable population; a ceiling latch (a slow or stuck load), an
    // open session or none is no verdict on the count.
    if (!coop::world_load_episode::QuiescedByStability()) {
        End(Outcome::Invalid, "the client's load episode did not settle on a stable population (the ceiling latched, "
                              "or no session closed): nothing to read for i4");
        return;
    }
    const int unsettled = coop::world_load_episode::UnsettledBuildPieces();
    if (unsettled > 0) {
        End(Outcome::Fail, "i4: %d building piece(s) were counted unsettled when the client's load episode ended",
            unsettled);
        return;
    }
    const std::string life = std::to_string(LifeOf());
    PS::Mark(("bid-keys-" + life).c_str(), level.text);
    PS::Mark(("bid-settled-" + life).c_str());
    BI::Info("client life %s done: i1 and i4 hold, the level census is left for the host", life.c_str());
    g_phase = Phase::Done;
}

// The host's wait for its peer's census, polled; a bound says INVALID.
bool Fresh(const char* name, std::string& text, uint64_t now, uint64_t boundMs) {
    if (PS::Peek(name, text)) return true;
    if (now - g_phaseMs >= boundMs)
        End(Outcome::Invalid, "no '%s' from the client within %llu s of the host's wait", name,
            static_cast<unsigned long long>(boundMs / 1000));
    return false;
}

void HostStep(uint64_t now) {
    if (now - g_polledMs < kPollMs) return;
    g_polledMs = now;
    std::string text;
    switch (g_phase) {
    case Phase::HostKeys1:
        if (!Fresh("bid-keys-1", text, now, kClientKeysBoundMs)) return;
        if (!SameKeys("i2", "the client", g_hostText, text)) return;
        BI::Info("i2 ok: the level wall and the five posters carry the same keys on both peers");
        To(Phase::HostKeys2, now);
        return;
    case Phase::HostKeys2:
        if (!Fresh("bid-keys-2", text, now, kRejoinBoundMs)) return;
        if (!SameKeys("i3", "the rejoined client", g_hostText, text)) return;
        BI::Info("i3 ok: the rejoined client holds the same keys");
        To(Phase::HostSettled2, now);
        return;
    case Phase::HostSettled2:
        if (!Fresh("bid-settled-2", text, now, kClientKeysBoundMs)) return;
        End(Outcome::Pass, "the pieces' keys read on both peers, match across the rejoin, and settle the load");
        return;
    default:
        return;
    }
}

}  // namespace

void Tick(coop::net::Session& session) {
    if (ArmOf() == Arm::Off || g_phase == Phase::Done) return;
    void* world = ue_wrap::world_identity::CurrentWorld();
    if (!world || ue_wrap::world_identity::CurrentWorldKind() != ue_wrap::world_identity::WorldKind::Gameplay) return;
    const Role role = RoleOf(session);
    const uint64_t now = coop::net::NowMs();
    if (!g_armed) {
        g_armed = true;
        if (!PS::Begin(kTag)) {  // says its own INVALID line
            g_phase = Phase::Done;
            return;
        }
        if (ArmOf() == Arm::KeyReadOff) ue_wrap::prop::SetBuildPieceKeyRead(false);
        g_phaseMs = now;
        BI::Info("armed role=%s arm=%s life=%d", RoleName(role),
                 ArmOf() == Arm::Green ? "green" : ArmOf() == Arm::KeyReadOff ? "key_read_off" : "settle_count_off",
                 LifeOf());
    }
    if (role == Role::Solo) {
        End(Outcome::Invalid, "a host and a client run this drill: this process has no role");
        return;
    }
    if (g_phase == Phase::WaitEdge) {
        if (world == g_censusWorld || !EdgeReached(role)) return;
        g_censusWorld = world;
        AtEdge(role, now);
        return;
    }
    if (role == Role::Host) HostStep(now);
}

}  // namespace coop::dev::build_identity_drill
