// coop/interactables/signal_catch_sync.cpp -- see coop/interactables/signal_catch_sync.h.

#include "coop/interactables/signal_catch_sync.h"

#include "coop/interactables/console_state_sync.h"
#include "coop/interactables/desk_input_sync.h"
#include "coop/comms/peer_action_feed.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"

#include "ue_wrap/desk/console_desk.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/desk/space_renderer.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace coop::signal_catch_sync {
namespace {

namespace CD = ue_wrap::console_desk;
namespace SR = ue_wrap::space_renderer;
namespace sg = ue_wrap::script_gate;

using Clock = std::chrono::steady_clock;

std::atomic<coop::net::Session*> g_session{nullptr};

constexpr auto kPoll = std::chrono::milliseconds(1000);
// The recent-catch TTL: a same-identity re-catch needs a full ping cycle, measured at six
// seconds or more, plus the native satellites-moving re-enter block, so five seconds can
// never swallow a legitimate re-catch.
constexpr auto kRecentTTL = std::chrono::seconds(5);

struct Identity { float x = 0, y = 0, z = 0, frequency = 0; };

bool IdentityEq(const Identity& a, const Identity& b) {
    // Exact float equality: every non-catcher copy is wire-byte identical (never fuzzy-match
    // identities).
    return a.x == b.x && a.y == b.y && a.z == b.z && a.frequency == b.frequency;
}

Clock::time_point g_nextPoll{};
bool g_announced = false;
uint64_t g_localCatches = 0;
uint64_t g_attributedCatches = 0;

// The catch detector's baseline: the full identity of the desk's signal data at the last poll
// (the tuple and the name, the change-edge signature).
Identity g_prevId{};
std::wstring g_prevSigName;
bool g_havePrevSig = false;
// The desk instance the baselines were read from. A mid-session level reload spawns a fresh
// desk whose runtime-only signal data starts at None; a module-static baseline surviving that
// would hold the old world's signal against the new desk's, so the baselines follow the
// instance. The same instance-tracking shape as the console state's suppression latch.
void* g_deskInst = nullptr;

// Recently caught identities (the catcher at send, the host and receivers at apply): filters
// stale in-flight sky-signal snapshot rows and detector re-fires.
struct Recent { Identity id; Clock::time_point at; };
std::vector<Recent> g_recent;

// Re-baseline the detectors against the current desk instance without firing anything (the
// fresh-instance prime and the explicit re-prime after a replay).
void PrimeBaselinesFromDesk() {
    CD::CoordSignal sig;
    if (CD::ReadCoordSignal(sig)) {
        g_prevId = { sig.x, sig.y, sig.z, sig.frequency };
        g_prevSigName = sig.objectName;
        g_havePrevSig = true;
    } else {
        g_prevId = {};
        g_prevSigName.clear();
        g_havePrevSig = false;
    }
}

// True once per desk-instance change (boot and a mid-session level reload): re-primes the
// baselines so stale state from the previous world can never read as a player action on the
// fresh one.
bool CheckDeskInstance() {
    void* inst = CD::Instance();
    if (inst == g_deskInst) return false;
    g_deskInst = inst;
    PrimeBaselinesFromDesk();
    return true;
}

// The desk's setData is a save's restore, which the gamemode's load runs whenever it loads a save, as every join does.
// It writes the signal data the save holds, which is no one's catch, so the detector takes it as its baseline, as it
// takes our wire appliers' writes. Unprimed, a joiner's restore of the save's caught signal reads as the joiner's own
// catch, and the host's replay of that catch resets the host's download and slews every dish.
constexpr const wchar_t* kDeskClass = L"analogDScreenTest_C";
constexpr const wchar_t* kRestoreName = L"setData";  // one pointer: the gate knows a watch by its literals
constexpr int kTagRestore = 0x53435230;  // 'SCR0'
bool g_restoreWatched = false;
bool g_restoreLive = false;
bool g_restoreRefused = false;  // refused or settled dead: said once, and the attempts end

void OnRestorePost(const sg::Call&) {
    PrimeBaselinesFromDesk();
    UE_LOGI("signal_catch: the desk's signal data restored from a save ('%ls') -- the baseline, not a catch",
            g_havePrevSig ? g_prevSigName.c_str() : L"unread");
}

void WatchRestore() {
    if (g_restoreLive || g_restoreRefused) return;
    if (!g_restoreWatched) {
        g_restoreWatched = sg::WatchClassName(kDeskClass, kRestoreName, kTagRestore, nullptr, &OnRestorePost);
        if (!g_restoreWatched) {
            g_restoreRefused = true;
            UE_LOGE("signal_catch: the gate took no watch on the desk's setData -- a joiner's restore reads as its "
                    "own catch");
            return;
        }
    }
    sg::ResolvePendingNames();
    if (sg::ClassNameWatchLive(kDeskClass, kRestoreName, kTagRestore)) {
        g_restoreLive = true;
        UE_LOGI("signal_catch: the desk's restore from a save is watched");
    } else if (sg::ClassNameWatchSettled(kDeskClass, kRestoreName, kTagRestore)) {
        g_restoreRefused = true;
        UE_LOGE("signal_catch: the watch on the desk's setData settled dead -- a joiner's restore reads as its own "
                "catch");
    }
}

bool IsNoneName(const std::wstring& n) { return n.empty() || n == L"None"; }

void RegisterRecent(const Identity& id) {
    g_recent.push_back({ id, Clock::now() });
}

bool IsRecent(const Identity& id) {
    for (const auto& r : g_recent)
        if (IdentityEq(r.id, id)) return true;
    return false;
}

void PruneRecent() {
    const auto now = Clock::now();
    for (auto it = g_recent.begin(); it != g_recent.end();) {
        if (now - it->at > kRecentTTL) it = g_recent.erase(it);
        else ++it;
    }
}

void FillRowFromCoordSignal(const CD::CoordSignal& sig, coop::net::WireSkySignal& w) {
    std::memset(&w, 0, sizeof(w));
    w.x = sig.x; w.y = sig.y; w.z = sig.z;
    w.type = sig.type;
    w.strength = sig.strength;
    w.frequency = sig.frequency;
    w.frequencySpread = sig.frequencySpread;
    w.polarity = sig.polarity;
    w.polaritySpread = sig.polaritySpread;
    // Alpha, lifetimes and direction stay zero: the row is being deleted; receivers only consume
    // the struct content.
    size_t n = 0;
    for (; n < sig.objectName.size() && n < sizeof(w.objectName); ++n)
        w.objectName[n] = static_cast<char>(sig.objectName[n]);  // rolled names are ASCII
    w.nameLen = static_cast<uint8_t>(n);
}

// The wire row's ASCII name widened for feed and log lines.
std::wstring WireName(const coop::net::WireSkySignal& w) {
    const size_t n = w.nameLen <= sizeof(w.objectName) ? w.nameLen : sizeof(w.objectName);
    return std::wstring(w.objectName, w.objectName + n);
}

CD::CoordSignal CoordSignalFromRow(const coop::net::WireSkySignal& w) {
    CD::CoordSignal sig;
    sig.x = w.x; sig.y = w.y; sig.z = w.z;
    sig.type = w.type;
    sig.strength = w.strength;
    sig.frequency = w.frequency;
    sig.frequencySpread = w.frequencySpread;
    sig.polarity = w.polarity;
    sig.polaritySpread = w.polaritySpread;
    const size_t n = w.nameLen <= sizeof(w.objectName) ? w.nameLen : sizeof(w.objectName);
    sig.objectName.assign(w.objectName, w.objectName + n);
    return sig;
}

// HOST: the author of the catch its desk made for a client and the signal it caught (AttributeCatch); 0xFF when none,
// kNoAuthor when that client left before the detector found it, whose catch is then relayed as nobody's.
constexpr uint8_t kNoAuthor = 0xFE;
uint8_t  g_catchAuthor = 0xFF;
Identity g_authorId{};

// HOST: catches a client sent, which only the host authors, dropped unanswered and said at most every kSayDropsMs.
constexpr auto kSayDrops = std::chrono::seconds(10);
uint64_t g_droppedFromClients = 0;
Clock::time_point g_nextSayDrops{};

void SayDropped(uint8_t senderSlot, uint8_t kind) {
    ++g_droppedFromClients;
    const auto now = Clock::now();
    if (now < g_nextSayDrops) return;
    g_nextSayDrops = now + kSayDrops;
    UE_LOGW("signal_catch: kind=%u arrived AT the host from slot %u -- the host's to author, dropped (%llu so far)",
            static_cast<unsigned>(kind), static_cast<unsigned>(senderSlot),
            static_cast<unsigned long long>(g_droppedFromClients));
}

// HOST: a catch made for `author` goes to every ready client, the author's own included, stamped as the author's.
void SendAs(coop::net::Session* s, const coop::net::SkySignalCatchPayload& p, uint8_t author) {
    for (int slot = 1; slot < static_cast<int>(coop::net::kMaxPeers); ++slot) {
        if (!s->IsSlotReady(slot)) continue;
        s->SendReliableToSlot(slot, coop::net::ReliableKind::SkySignalCatch, &p, sizeof(p), author);
    }
}

// A client's replay of the game's consume chain's identity half. The dish theatre is the host's
// (its native slews stream as poses via the dish sync; a client never slews from the wire) and
// the arm rides the host-authored dish-arm lane.
void ApplyReplay(const coop::net::SkySignalCatchPayload& p) {
    const CD::CoordSignal sig = CoordSignalFromRow(p.row);
    CD::WriteCoordSignal(sig);
    SR::RemoveSignalByIdentity(sig.x, sig.y, sig.z, sig.frequency);
    // The ping success's own writes to the download machine, its two literals, and nothing more.
    if (!CD::WriteCatchReset())
        UE_LOGW("signal_catch: the catch's writes to the download machine did not resolve here ('%ls')",
                sig.objectName.c_str());
    // No ping-success beep replay here: the pinging peer's organic ping sound crosses on the desk
    // sound-effects lane. Poses arrive on the dish pose stream; the arm on the dish-arm lane.
    UE_LOGI("signal_catch: catch identity applied ('%ls') -- host owns the theater", sig.objectName.c_str());
    g_prevId = { sig.x, sig.y, sig.z, sig.frequency };  // prime the FULL tuple
    g_prevSigName = sig.objectName;
    g_havePrevSig = true;
    coop::desk_input_sync::PrimeBaselines();
}

bool BuildCatchPayload(const CD::CoordSignal& sig, uint8_t kind,
                       coop::net::SkySignalCatchPayload& p) {
    std::memset(&p, 0, sizeof(p));
    p.kind = kind;
    FillRowFromCoordSignal(sig, p.row);
    return true;
}

// The catch detector body, shared by the 1 Hz tick and the snapshot-arrival race
// check. The signature is the signal data's identity (tuple or name) change edge: no sky-row
// corroboration, no dish edge. The field's writers are the native ping-success chain (a catch,
// the host's alone), the native "Signal data deleted" chain (a clear), the desk's setData (a
// save's restore) and our wire appliers; the last two prime the baselines.
void RunDetectors(coop::net::Session* s, const CD::CoordSignal& sig, bool haveSig) {
    if (s && s->connected() && haveSig && g_havePrevSig) {
        // A clear (the game's signal-deleted reset) is not this lane's: the host's reset reaches every peer on the
        // download arm lane, and the coordinate clear is inside the game's own reset there.
        // Catch: the identity change edge to non-None. No claim gate: the successful ping's own
        // completion releases the desk FSM-hold within the same second as the edge, so a
        // claim-gated 1 Hz detector lost the race and the roll-forward below ate the catch
        // permanently. The unprimed change edge itself proves local authorship: every writer of
        // the field that is not a verdict of this machine's primes these baselines.
        if (!IsNoneName(sig.objectName)) {
            const Identity id{ sig.x, sig.y, sig.z, sig.frequency };
            const bool changed = !IdentityEq(id, g_prevId) ||
                                 sig.objectName != g_prevSigName;
            if (changed && !IsRecent(id)) {
                if (s->role() != coop::net::Role::Host) {
                    // A client rolls no verdict (desk_ping_sync refuses its gatherSignal), so no catch is its own
                    // to relay: the host's catch lane holds the desk's signal, and this change has a writer the
                    // detector does not know.
                    UE_LOGW("signal_catch: CLIENT's desk signal changed to '%ls' unprimed -- not a catch of this "
                            "machine's, not relayed", sig.objectName.c_str());
                } else {
                    coop::net::SkySignalCatchPayload p{};
                    BuildCatchPayload(sig, 0, p);
                    RegisterRecent(id);
                    if (g_catchAuthor == kNoAuthor && IdentityEq(id, g_authorId)) {
                        // The client the verdict was rolled for left before this poll: the catch crosses as kind 2,
                        // which a client applies as a catch and announces to nobody, and the host announces none.
                        g_catchAuthor = 0xFF;
                        p.kind = 2;
                        SendAs(s, p, /*author*/ 0);
                        UE_LOGI("signal_catch: the host's verdict caught '%ls' for a client that left -- relayed "
                                "unannounced", sig.objectName.c_str());
                    } else if (g_catchAuthor != 0xFF && IdentityEq(id, g_authorId)) {
                        // The host's desk rolled this verdict for a client's ping: the catch is that client's, on
                        // every feed and on the wire.
                        const uint8_t author = g_catchAuthor;
                        g_catchAuthor = 0xFF;
                        ++g_attributedCatches;
                        SendAs(s, p, author);
                        UE_LOGI("signal_catch: the host's verdict for slot %u caught '%ls' -- relayed as slot %u's",
                                static_cast<unsigned>(author), sig.objectName.c_str(),
                                static_cast<unsigned>(author));
                        coop::peer_action_feed::Announce(author, coop::peer_action_feed::Action::CaughtSignal,
                                                         sig.objectName);
                    } else {
                        SendAs(s, p, /*author*/ 0);  // the host's own catch, to every client
                        ++g_localCatches;
                        UE_LOGI("signal_catch: local catch detected ('%ls' at %.0f,%.0f,%.0f) -- relayed",
                                sig.objectName.c_str(), sig.x, sig.y, sig.z);
                        // The catcher's own activity-feed line (the same line everyone sees, no "You");
                        // receivers announce at their receive sites.
                        coop::peer_action_feed::Announce(
                            coop::players::Registry::Get().LocalPeerId(),
                            coop::peer_action_feed::Action::CaughtSignal, sig.objectName);
                    }
                }
            }
        }
    }
    // Roll the previous-poll state forward.
    if (haveSig) {
        g_prevId = { sig.x, sig.y, sig.z, sig.frequency };
        g_prevSigName = sig.objectName;
        g_havePrevSig = true;
    }
}

bool PayloadFinite(const coop::net::SkySignalCatchPayload& p) {
    const float vals[] = { p.row.x, p.row.y, p.row.z, p.row.strength, p.row.frequency,
                           p.row.frequencySpread, p.row.polarity, p.row.polaritySpread };
    for (float v : vals)
        if (!std::isfinite(v)) return false;
    return true;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    WatchRestore();
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;
    const auto now = Clock::now();
    if (now < g_nextPoll) return;
    g_nextPoll = now + kPoll;

    const bool cdUp = CD::EnsureResolved() && CD::Instance();
    if (!cdUp) return;
    if (!g_announced) {
        g_announced = true;
        UE_LOGI("signal_catch: installed (desk surface resolved; L4 tuple detector)");
    }
    if (CheckDeskInstance()) return;  // fresh desk: baselines primed, detect next poll

    CD::CoordSignal sig;
    const bool haveSig = CD::ReadCoordSignal(sig);
    RunDetectors(s, sig, haveSig);
    PruneRecent();
}

void OnReliable(const coop::net::SkySignalCatchPayload& p, uint8_t senderSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    if ((p.kind != 0 && p.kind != 2) || !PayloadFinite(p)) return;
    if (!CD::EnsureResolved() || !CD::Instance()) return;

    const Identity id{ p.row.x, p.row.y, p.row.z, p.row.frequency };
    if (s->role() == coop::net::Role::Host) {
        // Both kinds are the host's to author: kind 2 is its connect seed or a catch whose pinger left, and a
        // client rolls no verdict (desk_ping_sync), so it never catches. A reset rides download_arm_sync.
        SayDropped(senderSlot, p.kind);
        return;
    }
    // A client is transport-trusted (we only receive from the host; the sender slot carries the
    // logical catcher via the relay stamp).
    RegisterRecent(id);
    ApplyReplay(p);
    // kind 2, a connect seed or a catch whose pinger left, is announced by nobody: no feed line.
    if (p.kind == 0)
        coop::peer_action_feed::Announce(senderSlot, coop::peer_action_feed::Action::CaughtSignal,
                                         WireName(p.row));
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    if (!CD::EnsureResolved() || !CD::Instance()) return;
    CD::CoordSignal sig;
    if (!CD::ReadCoordSignal(sig) || IsNoneName(sig.objectName)) return;
    coop::net::SkySignalCatchPayload p{};
    // kind 2 is the connect state seed: applied exactly like a catch but never announced to the
    // activity feed (a joiner must not see a stale host-attributed "caught signal" line).
    BuildCatchPayload(sig, 2, p);
    s->SendReliableToSlot(peerSlot, coop::net::ReliableKind::SkySignalCatch, &p, sizeof(p));
    UE_LOGI("signal_catch: connect catch seed (kind=2) -> slot %d ('%ls')", peerSlot, sig.objectName.c_str());
}

void NoteIncomingSnapshot(std::vector<SR::SignalRow>& rows) {
    auto* s = g_session.load(std::memory_order_acquire);
    // Run the catch detector now (fresh reads) so an in-flight local catch outranks the stale
    // snapshot row about to apply; any peer with an unprimed edge is the catch authority.
    if (s && CD::EnsureResolved() && CD::Instance() && !CheckDeskInstance()) {
        CD::CoordSignal sig;
        const bool haveSig = CD::ReadCoordSignal(sig);
        RunDetectors(s, sig, haveSig);
    }
    // Strip recently caught identities: a snapshot sent before the host processed the catch must
    // not resurrect the row.
    for (auto it = rows.begin(); it != rows.end();) {
        if (IsRecent({ it->x, it->y, it->z, it->frequency })) {
            UE_LOGI("signal_catch: filtered recently-caught row (%.0f, %.0f, %.0f) "
                    "from an incoming snapshot", it->x, it->y, it->z);
            it = rows.erase(it);
        } else {
            ++it;
        }
    }
}

uint64_t LocalCatchesRelayed() { return g_localCatches; }

uint64_t AttributedCatchesRelayed() { return g_attributedCatches; }

void AttributeCatch(float x, float y, float z, float frequency, uint8_t slot) {
    g_catchAuthor = slot;
    g_authorId = { x, y, z, frequency };
}

void OnPeerLeft(uint8_t slot) {
    if (g_catchAuthor == slot) g_catchAuthor = kNoAuthor;
}

void OnDisconnect() {
    g_prevId = {};
    g_prevSigName.clear();
    g_havePrevSig = false;
    g_deskInst = nullptr;
    g_recent.clear();
    g_catchAuthor = 0xFF;
}

}  // namespace coop::signal_catch_sync
