// coop/player/hand_record_sync.cpp -- see coop/player/hand_record_sync.h.

#include "coop/player/hand_record_sync.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/items/save_record_wire.h"
#include "coop/net/blob_chunks.h"
#include "coop/net/intent_bucket.h"
#include "coop/net/net_clock.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/hand_history.h"
#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"
#include "coop/player/roster_ledger.h"
#include "ue_wrap/actors/container_inventory.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/class_reask.h"
#include "ue_wrap/core/class_resolve.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <array>
#include <string>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <vector>
#include <wchar.h>

namespace coop::hand_record_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace SR = ue_wrap::save_record;
namespace sg = ue_wrap::script_gate;
namespace CI = ue_wrap::container_inventory;
namespace CR = ue_wrap::class_resolve;
namespace W  = coop::save_record_wire;
namespace N  = coop::net;

// The game's own hold change, watched at the script gate: updateHold rebuilds the hand item from the hold slot (a pick,
// a drop, a quick-slot switch) and writes holding_actor before it returns, so the hold is what the post sees.
UE_FACT_FN(kRowUpdateHold, "hand_record_sync", L"mainPlayer_C", L"updateHold", Declared);
constexpr int kTag = 0x48524355;  // 'HRCU'

// A client's hand records the host judges: 4 at once, 2 a second. A record past it is not refused: the newest waits for
// the bucket to refill (a quick hotbar scroll ends on its final state), and a sender past it only replaces its own.
constexpr N::IntentBudget kBudget{4.0f, 2.0f};

// A held item's record is its leaf state. Every honest held-can record measured 232-234 B on the rig (the can, hand,
// joiner and forge arms); a record past the ceiling is refused whole before anything of it is decoded or relayed. A drive
// box holds 16 signal rows, and a row's bytes are signal_wire's 48 of head, 2 per char of its four texts (capped at 96,
// 48, 64 and 64 chars: 544 B) and a 4-byte length: 596 B at the caps, 9536 B for 16 rows, which a 4 KB ceiling refused
// (the sender cleared the hand and the box was lost). 12 KB holds the 16 capped rows and leaves 2.7 KB for the rest of
// a box's record, which carries no list.
constexpr size_t kMaxRecordBytes = 12288;
// The bounds on what a decoded record may carry, set from that measured shape plus margin (the stored line prints every
// group's count so the next run can tighten them): a held item writes a handful of short groups.
constexpr size_t kMaxGroups = 16;      // groups of one kind
constexpr size_t kMaxPerGroup = 64;    // elements of one group
constexpr size_t kMaxChars = 96;       // a name, a string, a class leaf
constexpr size_t kMaxSignals = 16;     // signal rows (a disc carrier's; a drive box holds 16, VOTV-MP/Multivoid#192)
constexpr size_t kMaxClassNames = 64;  // every class leaf of a record, all groups: each is a probe of the class index
// How long a class the sender held counts as held: the host applies a client's hand states at a pace (kHandBudget), so
// a record can arrive behind the hand it was read from. Covers that pace with margin.
constexpr uint64_t kStaleWindowMs = 3000;

coop::net::Session* g_session = nullptr;
coop::blob_chunks::Assembler g_asm;
uint32_t g_seq = 0;

bool IsHost() { return g_session && g_session->running() && g_session->role() == N::Role::Host; }
bool Connected() { return g_session && g_session->running() && g_session->connected(); }

uint8_t OwnSlot() { return coop::players::Registry::Get().LocalPeerId(); }

// One hand's record as this peer holds it. The host keeps the bytes it re-sends (its decoded copy, re-encoded), and
// the slot's mirror is written from `rec` once per record, and again when the mirror is made anew.
struct Stored {
    bool have = false;
    std::vector<uint8_t> blob;  // HOST only: [u8 slot][the record], what a joiner is sent
    SR::SaveRecord rec;
    ue_wrap::CachedObjRef appliedTo;  // the mirror it was last written into
    bool dirty = false;
    ue_wrap::CachedObjRef checked;  // the mirror whose class classMatch was read for
    bool classMatch = false;
    bool waitingClasses = false;  // a class the record names is not loaded yet; ask is the retry's window
    ue_wrap::class_reask::Latch ask;
};
coop::roster_ledger::PerSlotState<Stored> g_hand;

// HOST: a client's newest record, waiting for its pacing token (raw: nothing of it is decoded before the token), and
// then, decoded, for every class it names to resolve.
struct Parked {
    bool valid = false;
    std::vector<uint8_t> blob;
    bool decoded = false;
    SR::SaveRecord rec;
    bool waitingClasses = false;
    ue_wrap::class_reask::Latch ask;  // the class index's window for a class not drained yet
};
coop::roster_ledger::PerSlotState<Parked> g_parked;
coop::roster_ledger::PerSlotState<coop::net::IntentBucket> g_bucket;

// The owner's side: the hand changed (the hold, or its can's init) and its record is yet to be read and sent. The watch
// is the edge; the flag is read in Tick, after the game wrote the hold.
bool g_ownDirty = false;
bool g_wasConnected = false;
ue_wrap::CachedObjRef g_waitHand;  // the hand actor the announce latch was read for
std::wstring g_waitCls;
enum class Reg : uint8_t { Pending, Registered, Refused };
Reg g_reg = Reg::Pending;
bool g_settled = false;

constexpr int kRefusals = static_cast<int>(Refusal::kCount);
constexpr const char* kRefusalName[kRefusals] = {"oversize", "malformed", "wrong-slot", "non-finite", "forged class",
                                                 "class unknown", "keyless", "bounds"};
// HOST: why a client's record was refused, counted per sender; logged at the first and each power of two.
coop::roster_ledger::PerSlotState<std::array<uint32_t, kRefusals>> g_refused;
coop::roster_ledger::PerSlotState<uint32_t> g_stale;      // a record of a class the sender held a moment ago, dropped
coop::roster_ledger::PerSlotState<uint32_t> g_accepted;   // records the judge stored for this slot
coop::roster_ledger::PerSlotState<uint16_t> g_snapshotOwed;  // joiner slot -> the source slots whose blob its link refused
uint32_t g_relaySkipped = 0;                              // [dev] a send the relay red arm skipped
uint32_t g_snapshots = 0;                                 // a joiner's snapshot sent
coop::roster_ledger::PerSlotState<uint32_t> g_replaced;   // a parked record a newer one replaced
coop::roster_ledger::PerSlotState<uint32_t> g_written;    // records written into this slot's mirror here
coop::roster_ledger::PerSlotState<uint32_t> g_cleared;    // this slot's record cleared (the host) or taken as cleared (a peer)
uint32_t g_indicesNeutered = 0;                           // a container record that carried a GObjStack index

// `detail`, when given, says which bound; it is logged with the refusal, which a power-of-two count throttles.
void Refuse(uint8_t sender, Refusal why, const wchar_t* cls, const char* detail = "") {
    const uint32_t n = ++g_refused[sender][static_cast<int>(why)];
    // %.64ls: the class name is the sender's, up to the wire's 256 K chars.
    if ((n & (n - 1)) == 0)
        UE_LOGW("hand_record: refused (%s) slot=%u class='%.64ls' (#%u from this sender) %s",
                kRefusalName[static_cast<int>(why)], static_cast<unsigned>(sender), cls ? cls : L"", n, detail);
}

// Every number a record carries, finite: a non-finite one is refused whole, never clamped (hook_anchor's shape).
bool RecordFinite(const SR::SaveRecord& r) {
    for (float f : r.xform) if (!std::isfinite(f)) return false;
    for (const auto& grp : r.floats) for (float f : grp) if (!std::isfinite(f)) return false;
    for (const auto& grp : r.vectors) for (const auto& v : grp) for (float f : v) if (!std::isfinite(f)) return false;
    for (const auto& grp : r.rotators) for (const auto& v : grp) for (float f : v) if (!std::isfinite(f)) return false;
    for (const auto& grp : r.transforms) for (const auto& v : grp) for (float f : v) if (!std::isfinite(f)) return false;
    for (const auto& s : r.signals)
        for (float f : {s.size, s.decoded, s.downloadedAtQuality, s.locX, s.locY}) if (!std::isfinite(f)) return false;
    return true;
}

// Which of a record's counts or lengths is past the lane's bound, as text for the refusal's line; empty when all are
// within: a client's refused hand has its reason said, not only its class.
template <class G>
std::string GroupsFault(const G& groups, const char* kind) {
    char b[96];
    if (groups.size() > kMaxGroups) {
        std::snprintf(b, sizeof(b), "%zu %s groups (bound %zu)", groups.size(), kind, kMaxGroups);
        return b;
    }
    for (const auto& grp : groups)
        if (grp.size() > kMaxPerGroup) {
            std::snprintf(b, sizeof(b), "a %s group of %zu (bound %zu)", kind, grp.size(), kMaxPerGroup);
            return b;
        }
    return {};
}

std::string TextsFault(const std::vector<std::vector<std::wstring>>& groups, const char* kind) {
    std::string f = GroupsFault(groups, kind);
    if (!f.empty()) return f;
    for (const auto& grp : groups)
        for (const auto& s : grp)
            if (s.size() > kMaxChars) {
                char b[96];
                std::snprintf(b, sizeof(b), "a %s of %zu chars (bound %zu)", kind, s.size(), kMaxChars);
                return b;
            }
    return {};
}

}  // namespace

std::string BoundsFault(const SR::SaveRecord& r) {
    if (r.className.size() > kMaxChars || r.key.size() > kMaxChars) return "a class or key past its length";
    for (const std::string& f : {GroupsFault(r.bools, "bool"), GroupsFault(r.floats, "float"), GroupsFault(r.ints, "int"),
                                 GroupsFault(r.vectors, "vector"), GroupsFault(r.rotators, "rotator"),
                                 GroupsFault(r.transforms, "transform"), GroupsFault(r.bytes, "byte"),
                                 TextsFault(r.strings, "string"), TextsFault(r.classes, "class"),
                                 TextsFault(r.names, "name")})
        if (!f.empty()) return f;
    size_t classNames = 0;
    for (const auto& grp : r.classes) classNames += grp.size();
    char b[96];
    if (classNames > kMaxClassNames) {
        std::snprintf(b, sizeof(b), "%zu class names (bound %zu)", classNames, kMaxClassNames);
        return b;
    }
    if (r.signals.size() > kMaxSignals) {
        std::snprintf(b, sizeof(b), "%zu signals (bound %zu)", r.signals.size(), kMaxSignals);
        return b;
    }
    for (const auto& s : r.signals)
        if (s.name.size() > kMaxChars || s.id.size() > kMaxChars || s.object.size() > kMaxChars || s.signal.size() > kMaxChars)
            return "a signal's text past its length";
    return {};
}

namespace {

bool Decode(const std::vector<uint8_t>& blob, uint8_t& slot, SR::SaveRecord& rec) {
    size_t o = 0;
    return W::RdU8(blob, o, slot) && W::DeSave(blob, o, rec) && o == blob.size();
}

std::vector<uint8_t> Encode(uint8_t slot, const SR::SaveRecord& rec) {
    std::vector<uint8_t> blob;
    W::AppU8(blob, slot);
    W::SerSave(blob, rec);
    return blob;
}

// A send is all-or-nothing: false when the link refused it. The dev relay red drops silently (nothing is owed for it).
bool SendTo(const std::vector<uint8_t>& blob, int toSlot) {
    // [dev] the relay drill's red arm (hand_record_relay_off): the host stores what it is sent and sends nobody anything.
    static const bool relayOff = coop::config::ResolveFlag(::coop::config_registry::rows::hand_record_relay_off);
    if (relayOff && IsHost()) {
        if ((++g_relaySkipped & (g_relaySkipped - 1)) == 0)
            UE_LOGW("hand_record: send to slot %d skipped (hand_record_relay_off) (#%u)", toSlot, g_relaySkipped);
        return true;
    }
    return coop::blob_chunks::SendBlobToSlot(g_session, toSlot, N::ReliableKind::HandRecord, ++g_seq, blob);
}

// HOST: `blob` to every world-ready client but the record's owner (it has the item, and its own slot is never mirrored
// here). A slot still loading is skipped: its snapshot at the world-ready edge carries every stored record.
void EmitToPeers(const std::vector<uint8_t>& blob, uint8_t ownerSlot) {
    for (int x = 1; x < N::kMaxPeers; ++x) {
        if (x == ownerSlot || !g_session->IsSlotWorldReady(x)) continue;
        SendTo(blob, x);
    }
}

// This peer's copy of `slot`'s record; the mirror is written from it.
void Keep(uint8_t slot, SR::SaveRecord&& rec) {
    Stored& h = g_hand[slot];
    h = Stored{};
    h.have = true;
    h.rec = std::move(rec);
    h.dirty = true;
}

// HOST: the host's copy of `slot`'s record, decoded: a held item is no container and carries no record of its own to
// anyone else's world (the transform is the hand's, the key is the sender's own, no part of what the mirror shows), so
// the copy drops them and every peer is sent a re-encode of it, never the sender's bytes (MTA:
// CPlayerPuresyncPacket.cpp:218-268 parses, stores and re-sends from its own state).
void StoreAndEmit(uint8_t slot, SR::SaveRecord&& rec) {
    rec.xform = {};
    rec.key.clear();
    rec.nested.clear();
    rec.nestedCarried = false;
    // A container's GObjStack index names a slot of the sender's array: neutered, as every record lane does.
    if (CI::IsContainerRecord(rec)) {
        if (CI::CarriesInventoryIndex(rec)) {
            ++g_indicesNeutered;
            UE_LOGW("hand_record: slot %u's %.64ls record carried a GObjStack index -- neutered (#%u)",
                    static_cast<unsigned>(slot), rec.className.c_str(), g_indicesNeutered);
        }
        CI::ClearInventoryIndex(rec);
    }
    std::vector<uint8_t> blob = Encode(slot, rec);
    size_t names = 0;
    for (const auto& g : rec.classes) names += g.size();
    UE_LOGI("hand_record: slot %u's %.64ls record stored (%zu B; groups bools %zu floats %zu ints %zu strings %zu classes "
            "%zu (%zu names) names %zu signals %zu)",
            static_cast<unsigned>(slot), rec.className.c_str(), blob.size(), rec.bools.size(), rec.floats.size(),
            rec.ints.size(), rec.strings.size(), rec.classes.size(), names, rec.names.size(), rec.signals.size());
    ++g_accepted[slot];
    Keep(slot, std::move(rec));
    Stored& h = g_hand[slot];
    h.blob = blob;
    EmitToPeers(blob, slot);
}

// HOST: a client's decoded record against the hand its owner announced and the host applied.
void Judge(uint8_t slot, SR::SaveRecord&& rec) {
    if (!coop::hand_item::HeldClassIs(slot, rec.className.c_str())) {
        // A class the sender held a moment ago is an honest race (its record was read before the hand moved on, and the
        // host applies hand states at a pace): dropped, with its own counter. MTA treats the same case, old weapon
        // data, as benign (CPlayerPuresyncPacket.cpp:232-238).
        if (coop::hand_history::HeldWithin(slot, rec.className.c_str(), kStaleWindowMs, coop::net::NowMs())) {
            ++g_stale[slot];
            return;
        }
        Refuse(slot, Refusal::Forged, rec.className.c_str());
        return;
    }
    StoreAndEmit(slot, std::move(rec));
}

// The record's shape, after its token: decode, the sender's own slot, a key, finite numbers, the bounds. A refusal is
// counted under its own reason.
bool DecodeAndCheck(uint8_t sender, Parked& p) {
    uint8_t slot = 0;
    if (!Decode(p.blob, slot, p.rec)) { Refuse(sender, Refusal::Malformed, L""); return false; }
    p.blob = {};
    const wchar_t* cls = p.rec.className.c_str();
    if (slot != sender) { Refuse(sender, Refusal::WrongSlot, cls); return false; }
    // A held item is a keyed prop: a record without its key is a can mid-construction or a forgery.
    if (p.rec.key.empty() || p.rec.key == L"None") { Refuse(sender, Refusal::Keyless, cls); return false; }
    if (!RecordFinite(p.rec)) { Refuse(sender, Refusal::NonFinite, cls); return false; }
    if (const std::string why = BoundsFault(p.rec); !why.empty()) {
        Refuse(sender, Refusal::Bounds, cls, why.c_str());
        return false;
    }
    return true;
}

// Whether every class a record names resolves, by the class index alone (a host never loads for a client's name).
enum class Names : uint8_t { Found, Missing, Gone };
Names ResolveNames(const SR::SaveRecord& r) {
    Names out = Names::Found;
    for (const auto& group : r.classes)
        for (const auto& leaf : group) {
            if (leaf.empty()) continue;  // a null class is no name
            const CR::ClassState st = CR::ResolveClass(leaf.c_str(), CR::AnyFamily(), CR::Load::LookupOnly).state;
            if (st == CR::ClassState::Refused) return Names::Gone;
            if (st != CR::ClassState::Found) out = Names::Missing;
        }
    return out;
}

// HOST: the slot's parked record: once the hand state it follows is applied (the host applies a client's hand states at
// a pace, so the one applied may be older than the one that arrived: prop_nametag waits the same way) and the sender's
// bucket holds a token, it is decoded and checked; a class it names that the index has not drained yet waits under the
// index's window (never refused for being late), and one that resolves to no class when the window ends is refused.
void ServeParked(uint8_t slot) {
    Parked& p = g_parked[slot];
    if (!p.valid) return;
    if (coop::hand_item::HandStateOwed(slot)) return;
    if (!p.decoded) {
        if (!g_bucket[slot].Take(kBudget, coop::net::NowMs())) return;
        if (!DecodeAndCheck(slot, p)) { p = Parked{}; return; }
        p.decoded = true;
    }
    if (!p.rec.classes.empty()) {
        if (p.waitingClasses && !p.ask.Due()) {
            if (p.ask.Expired()) {
                Refuse(slot, Refusal::ClassUnknown, p.rec.className.c_str());
                p = Parked{};
            }
            return;
        }
        const Names n = ResolveNames(p.rec);
        if (n == Names::Gone) { Refuse(slot, Refusal::ClassUnknown, p.rec.className.c_str()); p = Parked{}; return; }
        if (n == Names::Missing) { p.waitingClasses = true; return; }
    }
    SR::SaveRecord rec = std::move(p.rec);
    p = Parked{};
    Judge(slot, std::move(rec));
}

// HOST: `slot`'s stored record goes, and every peer but the owner is sent an EMPTY record (no class) for it, which clears
// its copy: neither the next item of that class nor a joiner is shown the old item's state. Only a slot that had a
// record sends one.
void ClearAndEmpty(uint8_t slot, const char* why) {
    if (!g_hand[slot].have) return;
    UE_LOGI("hand_record: slot %u's %.64ls record cleared (%s)", static_cast<unsigned>(slot),
            g_hand[slot].rec.className.c_str(), why);
    g_hand[slot] = Stored{};
    ++g_cleared[slot];
    // [dev] the clear drill's red arm (hand_record_clear_off): the host clears its copy and tells nobody.
    static const bool clearOff = coop::config::ResolveFlag(::coop::config_registry::rows::hand_record_clear_off);
    if (clearOff) {
        UE_LOGW("hand_record: the empty record for slot %u is not sent (hand_record_clear_off)", static_cast<unsigned>(slot));
        return;
    }
    EmitToPeers(Encode(slot, SR::SaveRecord{}), slot);
}

// HOST: the host's own hand holds no item with a record of its own.
void ClearOwn(const char* why) {
    if (IsHost()) ClearAndEmpty(OwnSlot(), why);
}

// The owner's hand: its record read through the item's own getData and sent, once the hand lane has announced the item
// (HandItem goes first on the one ordered lane, so the host judges the record against the hand it describes). Only a
// can's own state change and a hold change are announced here: another class's state changed in the hand (an
// extinguisher's charge) is not, and its mirror shows the record of the hold. MTA streams this state in puresync
// (CPlayerPuresyncPacket.cpp:371-460); the game announces a hold and a can's change only, so this lane is event-driven
// and a joiner is replayed.
void TickOwner() {
    if (!g_ownDirty) return;
    void* const hand = coop::hand_item::LocalHandActor();
    if (!hand || !R::IsLive(hand)) {
        g_ownDirty = false;
        ClearOwn("the hand is empty");
        return;
    }
    void* const cls = R::ClassOf(hand);
    if (!SR::IsPropClass(cls) || !SR::OverridesGetData(cls)) {
        g_ownDirty = false;  // an item with no record of its own
        ClearOwn("the hand holds an item with no record");
        return;
    }
    // A keyless item (a can whose init runs before its key is set) has no identity yet: its record is not read, and the
    // can's next init (the edge that marks this lane dirty) comes with the key. The host refuses a keyless record too.
    const std::wstring key = ue_wrap::prop::GetInteractableKeyString(hand);
    if (key.empty() || key == L"None") {
        g_ownDirty = false;
        ClearOwn("the hand holds a keyless item");
        static bool s_said = false;
        if (!s_said) UE_LOGW("hand_record: the hand's %ls has no key yet -- its record is not read", R::ClassNameOf(hand).c_str());
        s_said = true;
        return;
    }
    // A poll of the hand lane's latch while the announce is yet to come: the hand lane's edge tables are the host's
    // applied states alone (hand_item.h SubscribeHandApplied) and the owner's own announce has no subscriber, so the
    // only observable is the latch. It follows in the frame (hand_record_sync::Tick runs before local_streams::Tick), and
    // stays unannounced while a joining client's streams wait for the load tail; each read is a few pointer reads.
    if (!g_waitHand.Is(hand)) {
        g_waitHand.Set(hand);
        g_waitCls = R::ClassNameOf(hand);
    }
    if (!coop::hand_item::OwnAnnounced(g_waitCls.c_str())) return;
    g_ownDirty = false;
    SR::SaveRecord rec;
    if (!SR::CaptureRecord(hand, rec)) {
        static bool s_said = false;
        if (!s_said) UE_LOGW("hand_record: the hand's %ls record could not be read", g_waitCls.c_str());
        s_said = true;
        ClearOwn("the record could not be read");
        return;
    }
    // Where the hand holds it moves every frame and is no part of what the mirror shows (the receiver keeps its own
    // base fields): it is left out.
    rec.xform = {};
    const std::vector<uint8_t> blob = Encode(OwnSlot(), rec);
    if (blob.size() > kMaxRecordBytes) {
        static bool s_said = false;
        if (!s_said) UE_LOGW("hand_record: the hand's %ls record is %zu B, over the lane's ceiling -- not sent",
                             g_waitCls.c_str(), blob.size());
        s_said = true;
        ClearOwn("the record is over the ceiling");
        return;
    }
    UE_LOGI("hand_record: this player's hand %ls sent (%zu B)", g_waitCls.c_str(), blob.size());
    if (IsHost()) StoreAndEmit(OwnSlot(), std::move(rec));
    else if (!SendTo(blob, 0)) UE_LOGW("hand_record: the send of the hand's record was refused -- dropped");
}

// HOST: the hand lane applied a state of `slot`, or settled an owed one: a record of an item the hand no longer holds
// goes, and the parked record is judged against the hand now.
void OnHandApplied(uint8_t slot) {
    if (slot >= N::kMaxPeers) return;
    const Stored& h = g_hand[slot];
    if (h.have && !coop::hand_item::HoldsItem(slot)) ClearAndEmpty(slot, "the hand is empty");
    else if (h.have && !coop::hand_item::HeldClassIs(slot, h.rec.className.c_str())) ClearAndEmpty(slot, "the class changed");
    ServeParked(slot);
}

// The game's updateHold ran: if it was this player's, the hand changed.
void OnUpdateHoldPost(const sg::Call& c) {
    if (c.object && c.object == coop::players::Registry::Get().Local() && Connected()) g_ownDirty = true;
}

// Every stored record into its slot's current mirror: once per record, and again when the mirror is made anew. The hand
// lane exports no event for a mirror made anew, so each slot that holds a record compares its mirror's pointer per frame
// (a flag and a slot-validated compare); the mirror's class is read once per distinct mirror, never per frame.
void TickMirrors() {
    const uint8_t own = OwnSlot();
    const bool host = IsHost();
    for (uint8_t s = 0; s < N::kMaxPeers; ++s) {
        Stored& h = g_hand[s];
        if (!h.have || s == own) continue;
        void* const mirror = coop::hand_item::MirrorActorForSlot(s);
        if (!mirror) continue;
        if (!h.dirty && h.appliedTo.Is(mirror)) continue;
        if (!h.checked.Is(mirror)) {
            h.checked.Set(mirror);
            h.classMatch = R::ClassNameOf(mirror) == h.rec.className;
        }
        if (!h.classMatch) continue;  // the hand moved on: its next record follows
        // A client loads a class the record names inside the per-tick budget: past it the write waits, asked again on the
        // next ticks while the class set moves (class_reask::Latch), and finds every class loaded when it runs. The host
        // loads none for a client's name.
        if (!host) {
            if (h.waitingClasses && !h.ask.Due()) continue;
            h.waitingClasses = !SR::ClassesReady(h.rec, SR::ClassLoad::Budgeted);
            if (h.waitingClasses) continue;
        }
        const SR::ClassLoad load = host ? SR::ClassLoad::LookupOnly : SR::ClassLoad::Immediate;
        const bool ok = SR::ApplyRecord(mirror, h.rec, load);
        // A prop's loadData runs its init, which turns its physics back on: the mirror falls out of the hand between the
        // view's placings and hangs in the air when it is put back. The write leaves it a display again.
        static const bool redoOff = coop::config::ResolveFlag(::coop::config_registry::rows::hand_display_redo_off);  // [dev] red arm
        if (!redoOff) coop::hand_item::MakeDisplay(mirror);
        h.appliedTo.Set(mirror);
        h.dirty = false;
        ++g_written[s];
        UE_LOGI("hand_record: slot %u's hand %.64ls written into its mirror here (ok=%d)", s, h.rec.className.c_str(),
                ok ? 1 : 0);
    }
}

// HOST: every stored record but the joiner's own, to that slot; the sources whose blob its link refused are owed to it.
void SendSnapshot(int slot, uint16_t sources) {
    uint16_t owed = 0;
    for (uint8_t s = 0; s < N::kMaxPeers; ++s) {
        if (!(sources & (1u << s)) || !g_hand[s].have || s == slot) continue;
        if (!SendTo(g_hand[s].blob, slot)) owed |= static_cast<uint16_t>(1u << s);
    }
    g_snapshotOwed[slot] = owed;
}

}  // namespace

size_t EncodedBytes(const SR::SaveRecord& r) {
    SR::SaveRecord copy = r;
    copy.xform = {};   // as the sender sends it: where the hand holds it is no part of the record
    return Encode(0, copy).size();
}

size_t RecordCeilingBytes() { return kMaxRecordBytes; }

void Install(coop::net::Session* session) {
    g_session = session;
    if (!coop::hand_item::SubscribeHandApplied(&OnHandApplied))
        UE_LOGE("hand_record: the hand lane's subscriber table is full -- a record waiting for its hand state is "
                "judged at the next tick instead");
    if (g_reg != Reg::Pending) return;
    g_reg = sg::WatchName(kRowUpdateHold, kTag, nullptr, &OnUpdateHoldPost) ? Reg::Registered : Reg::Refused;
}

SlotCounts CountsFor(uint8_t slot) {
    if (slot >= N::kMaxPeers) return {};
    return {g_written[slot], g_cleared[slot], g_hand[slot].have, g_accepted[slot]};
}

uint32_t RefusedCount(uint8_t slot, Refusal why) {
    return slot < N::kMaxPeers ? g_refused[slot][static_cast<int>(why)] : 0;
}

uint32_t StaleDropped(uint8_t slot) { return slot < N::kMaxPeers ? g_stale[slot] : 0; }

bool Settled(uint8_t slot) {
    if (slot >= N::kMaxPeers) return false;
    const Stored& h = g_hand[slot];
    void* const mirror = coop::hand_item::MirrorActorForSlot(slot);
    return h.have && !g_parked[slot].valid && !h.dirty && mirror && h.appliedTo.Is(mirror);
}

bool StoredClassIs(uint8_t slot, const wchar_t* cls) {
    return slot < N::kMaxPeers && cls && g_hand[slot].have && ::_wcsicmp(g_hand[slot].rec.className.c_str(), cls) == 0;
}

LaneCounts LaneCountsNow() { return {g_ownDirty, g_snapshots}; }

void NoteHeldItemChanged(void* actor) {
    if (actor && Connected() && actor == coop::hand_item::LocalHandActor()) g_ownDirty = true;
}

void Tick() {
    if (!g_settled && g_reg != Reg::Pending) {
        if (g_reg == Reg::Registered) sg::ResolvePendingNames();
        if (g_reg == Reg::Registered && sg::NameWatchLive(kRowUpdateHold, kTag)) {
            g_settled = true;
            UE_LOGI("hand_record: the watch on updateHold is live");
        } else if (g_reg == Reg::Refused || sg::NameWatchSettled(kRowUpdateHold, kTag)) {
            g_settled = true;
            UE_LOGE("hand_record: the watch on updateHold is dead -- a held item's record is not sent");
        }
    }
    if (!Connected()) {
        g_wasConnected = false;
        return;
    }
    // The session's start is an event of the hand too: an item held when it began has had no hold change since.
    if (!g_wasConnected) {
        g_wasConnected = true;
        g_ownDirty = true;
    }
    TickOwner();
    TickMirrors();
    // The bucket's refill and a class index's move announce nothing: a parked record is asked again each tick (an empty
    // slot costs a flag read), and a snapshot the link refused goes again once its slot is world-ready.
    if (IsHost())
        for (uint8_t s = 1; s < N::kMaxPeers; ++s) {
            ServeParked(s);
            if (g_snapshotOwed[s] && g_session->IsSlotWorldReady(s)) SendSnapshot(s, g_snapshotOwed[s]);
        }
    static uint64_t s_sweepMs = 0;
    if (!g_asm.Idle() && coop::net::NowMs() >= s_sweepMs) {
        s_sweepMs = coop::net::NowMs() + 1000;
        g_asm.Sweep(std::chrono::steady_clock::now(), std::chrono::seconds(10));
    }
}

void SendJoinSnapshotForSlot(int slot) {
    if (!IsHost() || slot <= 0) return;
    SendSnapshot(slot, 0xFFFF);
    ++g_snapshots;
    // The host's own hand, when it is not stored yet, is read and sent at the next tick: to the joiner alone, since a
    // stored record is what the loop above sent and every other peer has it.
    if (!g_hand[OwnSlot()].have) g_ownDirty = true;
}

void OnChunk(const coop::net::BlobChunkPayload& c, uint8_t senderSlot) {
    if (senderSlot >= N::kMaxPeers) return;
    std::vector<uint8_t> blob;
    if (!g_asm.OnChunk(c, senderSlot, blob)) return;
    uint8_t slot = 0;
    SR::SaveRecord rec;
    if (IsHost()) {
        // A client's own hand only. The ceiling first, then the newest record per sender is parked as bytes: nothing of
        // it is decoded before its pacing token, so a sender's cost to the host is the lane's rate, not its wire rate.
        if (senderSlot == 0) return;
        if (blob.size() > kMaxRecordBytes) {
            Refuse(senderSlot, Refusal::Oversize, L"");
            return;
        }
        Parked& p = g_parked[senderSlot];
        if (p.valid) {
            const uint32_t n = ++g_replaced[senderSlot];
            if ((n & (n - 1)) == 0)
                UE_LOGI("hand_record: slot %u's parked record replaced by a newer one (#%u)",
                        static_cast<unsigned>(senderSlot), n);
        }
        p = Parked{};
        p.valid = true;
        p.blob = std::move(blob);
        ServeParked(senderSlot);
        return;
    }
    if (senderSlot != 0) return;  // a client takes hand records from the host alone
    if (!Decode(blob, slot, rec) || slot >= N::kMaxPeers) {
        UE_LOGW("hand_record: a malformed hand record from the host -- dropped");
        return;
    }
    // An EMPTY record (no class): the host cleared that slot's record. The mirror shows the default once HandItem has
    // made it anew for the next item, which it does for an emptied hand or another class.
    if (rec.className.empty()) {
        if (g_hand[slot].have) {
            UE_LOGI("hand_record: slot %u's record cleared by the host", static_cast<unsigned>(slot));
            ++g_cleared[slot];
        }
        g_hand[slot] = Stored{};
        return;
    }
    Keep(slot, std::move(rec));
}

void OnPeerLeft(uint8_t slot) {
    if (slot >= N::kMaxPeers) return;
    g_asm.ClearSlot(slot);
}

void OnDisconnect() {
    for (uint8_t s = 0; s < N::kMaxPeers; ++s) {
        g_hand[s] = Stored{};
        g_parked[s] = Parked{};
        g_bucket[s] = coop::net::IntentBucket{};
        g_snapshotOwed[s] = 0;
    }
    g_asm.Clear();
    g_ownDirty = false;
    g_wasConnected = false;
    g_waitHand.Set(nullptr);
}

}  // namespace coop::hand_record_sync
