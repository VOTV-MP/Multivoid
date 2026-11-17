// coop/props/glowstick_sync.cpp -- see coop/props/glowstick_sync.h.

#include "coop/props/glowstick_sync.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/session.h"
#include "coop/props/glowstick_stick_judge.h"
#include "coop/props/glowstick_stick_record.h"
#include "coop/props/glowstick_struck.h"
#include "coop/props/prop_key_index.h"
#include "coop/props/prop_save_data.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/core/types.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace coop::glowstick_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;
namespace SR = ue_wrap::save_record;
namespace SK = coop::glowstick_stick_record;
using ue_wrap::FVector;

UE_FACT_FN(kRowStickTo, "glowstick_sync", L"prop_glowstick_C", L"stickTo", Declared);
UE_FACT_FN(kRowRelease, "glowstick_sync", L"prop_glowstick_C", L"releaseGlowstick", Declared);
UE_FACT_FN(kRowLoad, "glowstick_sync", L"prop_glowstick_C", L"loadData", Declared);
UE_FACT_FN(kRowSetFrame, "glowstick_sync", L"prop_glowstick_C", L"setFrame", Declared);

constexpr int kTagBase = 0x474C5730;  // 'GLW0'
enum Row { kStick = 0, kRelease, kLoad, kSetFrame, kRowCount };
const ue_wrap::facts::Function* const kRows[kRowCount] = {&kRowStickTo, &kRowRelease, &kRowLoad, &kRowSetFrame};

std::atomic<coop::net::Session*> g_session{nullptr};
unsigned g_registered = 0;
bool g_converging = false;                  // inside this lane's own releaseGlowstick / stickTo
std::vector<ue_wrap::CachedObjRef> g_dirty;                 // glowsticks a player's own act stuck or freed; published next pass
// Glowsticks a player's own stickTo struck and whose constraint the game completes one frame later (hookDelay's
// Delay(0), then setFrame writes the frames the record carries): published when that setFrame ends, not before.
std::vector<ue_wrap::CachedObjRef> g_pending;
struct Before { bool sticked = false; bool stickStatic = false; FVector loc{}; std::wstring key; };
std::unordered_map<void*, Before> g_before;  // a landing's loadData: the copy's state before it
// A CLIENT's own sticks in flight, by key: the stick fields of each record it sent and the host has not yet handed
// back. A landing that equals one is the host's echo of that act, and the author's copy is already right: the
// landing is cancelled and the entry is consumed. Anything else is the host's truth: every older entry of the key
// goes and the landing applies. The newest few only, oldest out (a send that is never echoed is cleared by the next
// record that is not an echo).
constexpr size_t kInFlightPerKey = 4;
std::unordered_map<std::wstring, std::vector<SK::StickFields>> g_inFlight;
uint32_t g_published = 0, g_converged = 0, g_heldFree = 0, g_echoes = 0, g_sealedStatic = 0, g_bound = 0;
uint32_t g_keyless = 0;
uint32_t g_outsideIndex = 0;
uint32_t g_landed = 0, g_made = 0;   // landings that said stuck; sticks the game's own restore made

int Bool(void* o, const wchar_t* name) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!o || !R::FindBoolProperty(R::ClassOf(o), name, off, mask)) return -1;
    return (static_cast<const uint8_t*>(o)[off] & mask) ? 1 : 0;
}

bool Loc(void* o, FVector& out) {
    const int32_t off = o ? R::FindPropertyOffset(R::ClassOf(o), L"loc") : -1;
    if (off < 0) return false;
    std::memcpy(&out, static_cast<const uint8_t*>(o) + off, sizeof(out));
    return true;
}

bool CallNoArgs(void* o, const wchar_t* fn) {
    ue_wrap::ParamFrame f(R::FindDispatchFunctionCached(R::ClassOf(o), fn));
    return f.valid() && ue_wrap::Call(o, f);
}

bool SetBool(void* o, const wchar_t* name, bool v) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!o || !R::FindBoolProperty(R::ClassOf(o), name, off, mask)) return false;
    uint8_t& b = static_cast<uint8_t*>(o)[off];
    b = v ? static_cast<uint8_t>(b | mask) : static_cast<uint8_t>(b & ~mask);
    return true;
}

std::wstring KeyOf(void* o) { return ue_wrap::prop::GetInteractableKeyString(o); }

// The key the stick names (attachToKey); empty when the game wrote None.
std::wstring AttachKeyOf(void* o) {
    const int32_t off = R::FindPropertyOffset(R::ClassOf(o), L"attachToKey");
    if (off < 0) return {};
    std::wstring k = SR::ReadFNameAt(o, off);
    return k == L"None" ? std::wstring() : k;
}

void WriteAttachKey(void* o, const std::wstring& key) {
    const int32_t off = R::FindPropertyOffset(R::ClassOf(o), L"attachToKey");
    if (off >= 0) SR::WriteFNameField(static_cast<uint8_t*>(o) + off, key);
}

Before StateOf(void* o) {
    Before b;
    b.sticked = Bool(o, L"sticked") == 1;
    b.stickStatic = Bool(o, L"stickStatic") == 1;
    Loc(o, b.loc);
    b.key = AttachKeyOf(o);
    return b;
}

// The actor that owns the component a stick struck: UActorComponent::GetOwner, through reflection.
void* OwnerOf(void* comp) {
    ue_wrap::ParamFrame f(R::FindDispatchFunctionCached(R::ClassOf(comp), L"GetOwner"));
    return f.valid() && ue_wrap::Call(comp, f) ? f.Get<void*>(L"ReturnValue") : nullptr;
}

// What a stick struck, written as every peer will read it. The game names the struck prop only when the struck
// actor implements int_objects, and otherwise leaves the old name, so a record could send a peer to a prop the
// stick never touched; and `stickComp`, the struck component, is a pointer no record carries. So the lane reads the
// struck component's owner once, at the stick's end: a keyed prop is named by its key and the peers attach to it; any
// other surface is no key and the stick is static there.
void SealStrike(void* o) {
    const int32_t co = R::FindPropertyOffset(R::ClassOf(o), L"stickComp");
    void* comp = nullptr;
    if (co >= 0) std::memcpy(&comp, static_cast<const uint8_t*>(o) + co, sizeof(comp));
    void* const owner = comp && R::IsLive(comp) ? OwnerOf(comp) : nullptr;
    const std::wstring key = owner ? KeyOf(owner) : std::wstring();
    if (owner && !key.empty() && key != L"None") {
        if (coop::prop_key_index::Find(key) == owner) {
            if (AttachKeyOf(o) != key) WriteAttachKey(o, key);
            if (Bool(o, L"stickStatic") != 1) glowstick_struck::Attach(o, key);
            return;
        }
        if (Bool(o, L"stickStatic") == 1) {   // a keyed actor the stick does not follow: static is its truth, as for any surface
            if (!AttachKeyOf(o).empty()) WriteAttachKey(o, std::wstring());
            return;
        }
        // A simulating keyed actor outside the prop index (a vehicle, a drone, anything keyed but no prop): the record keeps the key the
        // game wrote and is NOT made static, which would hang every other peer's glowstick from the point where the body was
        // while this peer's follows it. The host's judge finds no prop under the key and refuses the stick, counted, and its
        // record un-sticks this copy (glowstick_stick_judge, struck-key-not-live).
        ++g_outsideIndex;
        UE_LOGW("glowstick_sync: a stick struck '%ls' (key '%ls'), keyed but outside the prop index -- sent as the game wrote it, "
                "the host will refuse it (%u so far)", R::ClassNameOf(owner).c_str(), key.c_str(), g_outsideIndex);
        return;
    }
    if (!AttachKeyOf(o).empty()) WriteAttachKey(o, std::wstring());
    if (Bool(o, L"stickStatic") != 1) {
        SetBool(o, L"stickStatic", true);
        ++g_sealedStatic;
    }
}

// The stick made on this copy from the record it now holds: the game's own restore, the one call its load makes on
// every actor (processKeys: a static stick re-hangs the hook at the point, a stick to a keyed prop constrains to that
// prop's root by its key, with the frames the record carries). It returns at once when `sticked` is false.
bool Restore(void* o) { return CallNoArgs(o, L"processKeys"); }

bool IsHost() {
    coop::net::Session* const s = g_session.load(std::memory_order_acquire);
    return s && s->role() == coop::net::Role::Host;
}

bool IsGlowstick(void* o) {
    void* base = ue_wrap::object_index::ClassByName(L"prop_glowstick_C");
    void* cls = o ? R::ClassOf(o) : nullptr;
    return base && cls && R::IsDescendantOfAny(cls, &base, 1);
}

// The landing record's stick fields, read off the loadData frame (the record the apply built, the receiver's own base
// half spliced in; the stick fields are the sender's).
bool LandingStick(const sg::Call& call, SK::StickFields& out) {
    static R::ParamOffset s_dataOff{L"data"};
    const int32_t off = s_dataOff.Of(call.function);
    if (off < 0 || !call.locals) return false;
    SR::SaveRecord rec;
    SR::ReadSaveRecord(call.locals + off, rec, /*withImage=*/false);
    return SK::ReadStick(rec, out) == SK::Read::Ok;
}

// The author's echo test: true when the landing equals a stick this client sent (the entry is consumed); otherwise
// the host's record is the truth and the key's older entries go.
bool ConsumesEcho(void* o, const sg::Call& call) {
    if (IsHost() || g_inFlight.empty()) return false;
    const auto list = g_inFlight.find(KeyOf(o));
    if (list == g_inFlight.end()) return false;
    SK::StickFields landing;
    if (!LandingStick(call, landing)) {
        g_inFlight.erase(list);
        return false;
    }
    for (auto e = list->second.begin(); e != list->second.end(); ++e) {
        if (!SK::Same(*e, landing)) continue;
        list->second.erase(e);
        if (list->second.empty()) g_inFlight.erase(list);
        return true;
    }
    g_inFlight.erase(list);
    return false;
}

sg::Verdict OnPre(const sg::Call& call) {
    if (call.tag != kTagBase + kLoad || !call.object || !SR::IsApplying()) return sg::Verdict::Run;
    if (ConsumesEcho(call.object, call)) {
        ++g_echoes;
        return sg::Verdict::Cancel;   // this player's own stick comes back as it left: the copy is right already
    }
    g_before[call.object] = StateOf(call.object);
    return sg::Verdict::Run;
}

bool Has(const std::vector<ue_wrap::CachedObjRef>& list, void* o) {
    for (const ue_wrap::CachedObjRef& d : list)
        if (d.Is(o)) return true;
    return false;
}

void Pend(std::vector<ue_wrap::CachedObjRef>& list, void* o) {
    if (Has(list, o)) return;
    list.emplace_back();
    list.back().Set(o);
}

void Unpend(std::vector<ue_wrap::CachedObjRef>& list, void* o) {
    for (size_t i = 0; i < list.size(); ++i)
        if (list[i].Is(o)) {
            list.erase(list.begin() + static_cast<long>(i));
            return;
        }
}

// A record landed on this copy (the host's truth, or a client's stick the host took): make or break the constraint
// the record says, as the game's own restore does, once.
void Landed(void* o, const Before& was) {
    const Before now = StateOf(o);
    if (!now.sticked && !was.sticked) return;
    const float dx = now.loc.X - was.loc.X, dy = now.loc.Y - was.loc.Y, dz = now.loc.Z - was.loc.Z;
    const bool moved = dx * dx + dy * dy + dz * dz > 1.f || now.stickStatic != was.stickStatic || now.key != was.key;
    if (now.sticked && was.sticked && !moved) return;   // the same stick: a wait or an attachment it has stands
    glowstick_struck::Forget(o);   // a stick that moves ends the wait and the attachment it had
    if (now.sticked) ++g_landed;
    // The drill's reds: a landing that makes no stick, and a restore that ignores the struck prop's key.
    static const bool s_restoreOff = coop::config::ResolveFlag(coop::config_registry::rows::glowstick_restore_off);
    static const bool s_keyOff = coop::config::ResolveFlag(coop::config_registry::rows::glowstick_attach_key_off);
    if (now.sticked && s_restoreOff) {
        UE_LOGI("glowstick_sync: '%ls' lands stuck and no stick is made (glowstick_restore_off)", KeyOf(o).c_str());
        return;
    }
    bool attached = now.sticked && !now.stickStatic && !now.key.empty();
    if (attached && s_keyOff) {
        WriteAttachKey(o, std::wstring());
        SetBool(o, L"stickStatic", true);
        attached = false;
    }
    g_converging = true;
    bool released = false, stuck = false;
    // A stick that is already made is released first: processKeys returns at once on a copy whose `sticked` is false,
    // and the release is what clears the old constraint and the hook, so the record's `sticked` is written back
    // between the two.
    if (was.sticked) released = CallNoArgs(o, L"releaseGlowstick");
    const bool waits = attached && !glowstick_struck::IsBound(now.key);
    if (now.sticked && !waits) {
        if (released) SetBool(o, L"sticked", true);
        stuck = Restore(o);
        if (stuck) ++g_made;
        if (attached) glowstick_struck::Attach(o, now.key);
    }
    g_converging = false;
    // The struck prop is not bound here yet: the game's restore would show its "INVALID" hint and make no
    // constraint, so the stick waits for the prop (glowstick_struck) and is made when the key binds.
    if (waits) glowstick_struck::Wait(o, now.key);
    ++g_converged;
    UE_LOGI("glowstick_sync: '%ls' %s from its record (released %d, stuck %d) at (%.0f,%.0f,%.0f), %s%ls%s", KeyOf(o).c_str(),
            now.sticked ? "stuck" : "freed", released ? 1 : 0, stuck ? 1 : 0, now.loc.X, now.loc.Y, now.loc.Z,
            now.stickStatic ? "static" : "attached to ", now.stickStatic ? L"" : now.key.c_str(),
            waits ? " -- waits for that prop to bind" : "");
}

// A struck prop bound: the stick a waiting copy holds in its record is made, once.
void OnKeyBound(void* o) {
    if (!R::IsLive(o) || Bool(o, L"sticked") < 0) return;
    const Before now = StateOf(o);
    g_converging = true;
    SetBool(o, L"sticked", true);
    const bool stuck = Restore(o);
    if (stuck) ++g_made;
    g_converging = false;
    if (!now.stickStatic && !now.key.empty()) glowstick_struck::Attach(o, now.key);
    ++g_bound;
    UE_LOGI("glowstick_sync: '%ls' -- the prop '%ls' it waited for is bound, its stick is made (%d)", KeyOf(o).c_str(),
            now.key.c_str(), stuck ? 1 : 0);
}

void OnPost(const sg::Call& call) {
    void* const o = call.object;
    const int row = call.tag - kTagBase;
    // A landing's state before it is taken first: an early return below must not leave it behind for a recycled address.
    Before was;
    bool haveWas = false;
    if (row == kLoad && o) {
        const auto it = g_before.find(o);
        if (it != g_before.end()) {
            was = std::move(it->second);
            g_before.erase(it);
            haveWas = true;
        }
    }
    if (!o || !R::IsLive(o) || !g_session.load(std::memory_order_acquire)) return;
    if (g_converging) return;   // this lane's own release or restore
    if (row == kStick) {
        // A player's own stickTo. The constraint is finished one frame later: the record waits for setFrame. A record parked
        // for the glowstick (the host's grab-time free record, parked while this player held it) is older than this stick and
        // would land on it once the hold has ended: it goes, by what it is and not by a clock.
        if (!SR::IsApplying()) {
            Pend(g_pending, o);
            coop::prop_save_data::DropParkedFor(KeyOf(o));
        }
        return;
    }
    if (row == kSetFrame) {
        // The stick's end (stickTo's hookDelay, one frame after the strike). The landing's own restore runs inside this
        // lane's converge and returned above; the game's load restore finds nothing pending.
        if (SR::IsApplying() || !Has(g_pending, o)) return;
        Unpend(g_pending, o);
        SealStrike(o);
        Pend(g_dirty, o);
        return;
    }
    if (row == kRelease) {
        // A release is a grab's: the grabber's own game frees its copy, every other peer frees theirs at the hold's
        // begin (OnHoldBegin), and the host publishes. A client sends no release. A stick intent parked for this glowstick
        // belongs to the hold before this one and dies with it.
        if (SR::IsApplying()) return;
        Unpend(g_pending, o);
        glowstick_struck::Forget(o);
        coop::prop_save_data::DropParkedFor(KeyOf(o));
        if (IsHost()) Pend(g_dirty, o);
        return;
    }
    if (row != kLoad || !SR::IsApplying() || !haveWas) return;
    Landed(o, was);
}

void Register() {
    for (int i = 0; i < kRowCount; ++i) {
        if (g_registered & (1u << i)) continue;
        if (sg::WatchName(*kRows[i], kTagBase + i, i == kLoad ? &OnPre : nullptr, &OnPost)) g_registered |= 1u << i;
    }
}

// The four watches name their bodies by FName, which resolve on the game thread: this lane drives that resolve itself
// and says once whether every watch is live or one is dead (a body the class does not declare), as the coffee lane does.
void SettleWatch() {
    static bool s_settled = false;
    if (s_settled) return;
    sg::ResolvePendingNames();
    if (g_registered != (1u << kRowCount) - 1) return;
    bool live = true;
    for (int i = 0; i < kRowCount; ++i) live = live && sg::NameWatchLive(*kRows[i], kTagBase + i);
    if (live) {
        s_settled = true;
        UE_LOGI("glowstick_sync: the stick's four watches are live");
    } else if (sg::PendingNameCount() == 0) {
        s_settled = true;
        UE_LOGE("glowstick_sync: a watch is dead (resolved into a full table, or the class declares no such body) -- a "
                "glowstick's stick crosses only where its own watches ran");
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    coop::glowstick_stick_judge::Install(session);
    Register();
}

void Tick() {
    coop::net::Session* const s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    SettleWatch();
    glowstick_struck::TickWaits(&OnKeyBound);
    // A stick whose glowstick died before its setFrame never completes: it is dropped, not kept for the world's life.
    for (size_t i = 0; i < g_pending.size();) {
        if (g_pending[i].Alive()) ++i;
        else g_pending.erase(g_pending.begin() + static_cast<long>(i));
    }
    if (g_dirty.empty()) return;
    std::vector<ue_wrap::CachedObjRef> dirty;
    dirty.swap(g_dirty);
    for (const ue_wrap::CachedObjRef& d : dirty) {
        void* const o = d.Get();
        if (!o) continue;
        const std::wstring key = KeyOf(o);
        if (key.empty() || key == L"None") {
            ++g_keyless;
            UE_LOGW("glowstick_sync: a stick of a glowstick with no key (%p) is not published (%u so far) -- the peers cannot name it",
                    o, g_keyless);
            continue;
        }
        const bool client = s->role() != coop::net::Role::Host;
        // The drill's reds: a client's stick that is not sent, and one whose point is a kilometre from where it struck.
        static const bool s_intentOff = coop::config::ResolveFlag(coop::config_registry::rows::glowstick_intent_off);
        static const bool s_farLoc = coop::config::ResolveFlag(coop::config_registry::rows::glowstick_far_loc);
        if (client && s_intentOff) {
            ++g_published;   // the lane took the stick up and withheld it: the drill's event that the stick ended
            UE_LOGI("glowstick_sync: this player's stick of '%ls' is not sent (glowstick_intent_off)", key.c_str());
            continue;
        }
        if (client && s_farLoc) {
            FVector far{};
            const int32_t off = R::FindPropertyOffset(R::ClassOf(o), L"loc");
            if (off >= 0 && Loc(o, far)) {
                far.X += 100000.f;
                std::memcpy(static_cast<uint8_t*>(o) + off, &far, sizeof(far));
            }
        }
        // A client keeps what it sends: a getData taken just before the publish's own, with nothing between them that
        // changes the glowstick, so the bytes that leave and the entry that waits for their echo are one record.
        SK::StickFields sent;
        SR::SaveRecord rec;
        const bool keep = client && SR::CaptureRecord(o, rec) &&
                          SK::ReadStick(rec, sent) == SK::Read::Ok;
        const bool ok = coop::prop_save_data::Publish(s, o, key);
        if (ok && keep) {
            std::vector<SK::StickFields>& list = g_inFlight[key];
            if (list.size() >= kInFlightPerKey) list.erase(list.begin());
            list.push_back(std::move(sent));
        }
        ++g_published;
        UE_LOGI("glowstick_sync: this player %s '%ls' -- its record goes %s (sent %d)",
                Bool(o, L"sticked") == 1 ? "stuck" : "freed", key.c_str(),
                s->role() == coop::net::Role::Host ? "to every peer" : "to the host", ok ? 1 : 0);
    }
}

void OnHoldBegin(void* prop) {
    coop::net::Session* const s = g_session.load(std::memory_order_acquire);
if (!s || !IsGlowstick(prop) || !R::IsLive(prop)) return;    // A stick intent parked for this glowstick belongs to the hold before this one (the host parks a client's intent while    // that client's drive stands): a new hold of it ends that stick, so the old intent must not land after this hold.    coop::prop_save_data::DropParkedFor(KeyOf(prop));    if (Bool(prop, L"sticked") != 1) return;
    static const bool s_off = coop::config::ResolveFlag(coop::config_registry::rows::glowstick_grab_release_off);
    if (s_off) {
        UE_LOGI("glowstick_sync: a hold of '%ls' began here and its stick is not released (glowstick_grab_release_off)",
                KeyOf(prop).c_str());
        return;
    }
    g_converging = true;
    const bool released = CallNoArgs(prop, L"releaseGlowstick");
    g_converging = false;
    ++g_heldFree;
    glowstick_struck::Forget(prop);
    const std::wstring key = KeyOf(prop);
    UE_LOGI("glowstick_sync: a hold of '%ls' began here -- its stick is released (%d)%s", key.c_str(), released ? 1 : 0,
            s->role() == coop::net::Role::Host ? ", the host's record goes to every peer" : "");
    if (s->role() == coop::net::Role::Host && !key.empty() && key != L"None")
        coop::prop_save_data::Publish(s, prop, key);
}

void OnHoldRefused(int slot, void* actor) {
    coop::net::Session* const s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host || !IsGlowstick(actor) || slot < 0) return;
    const std::wstring key = KeyOf(actor);
    if (key.empty() || key == L"None") return;
    const bool sent = coop::prop_save_data::PublishToSlot(s, slot, actor, key);
    UE_LOGI("glowstick_sync: slot %d's hold of '%ls' was refused -- the host's record goes to that slot (sent %d)", slot,
            key.c_str(), sent ? 1 : 0);
}

void NoteDestroyed(void* actor) {
    if (!IsHost() || !glowstick_struck::AnyAttached()) return;
    const std::wstring key = KeyOf(actor);
    if (key.empty() || key == L"None") return;
    std::vector<void*> hung;
    glowstick_struck::TakeAttached(key, hung);
    // The game acts on its own copy (the constraint to a dead body breaks or holds); the host's record of each
    // glowstick that hung from it follows on the next pass, once the game has.
    for (void* g : hung) Pend(g_dirty, g);
    if (!hung.empty())
        UE_LOGI("glowstick_sync: the prop '%ls' was destroyed -- %zu glowstick(s) hung from it, their records follow",
                key.c_str(), hung.size());
}

void OnDisconnect() {
    coop::glowstick_stick_judge::OnDisconnect();
    if (g_published || g_converged || g_heldFree || g_echoes || g_sealedStatic || g_bound || g_keyless || g_outsideIndex)
        UE_LOGI("glowstick_sync: this session published %u record(s), made %u stick(s) from records, freed %u at a "
                "hold's begin, took %u echo(es) of its own sticks and sealed %u static; %u stick(s) waited for their "
                "struck prop, %u made once it bound; %u keyless stick(s) not published; %u stick(s) struck a keyed actor outside the "
                "prop index", g_published, g_converged, g_heldFree, g_echoes, g_sealedStatic,
                glowstick_struck::WaitsStarted(), g_bound, g_keyless, g_outsideIndex);
    g_dirty.clear();
    g_before.clear();
    g_pending.clear();
    glowstick_struck::Clear();
    g_session.store(nullptr, std::memory_order_release);
    g_inFlight.clear();
    g_published = g_converged = g_heldFree = g_echoes = g_sealedStatic = g_bound = g_landed = g_made = g_keyless = g_outsideIndex = 0;
    g_converging = false;
}


namespace dev {
Counts Get() { return Counts{g_published, g_landed, g_made, g_heldFree, g_echoes}; }
}  // namespace dev

}  // namespace coop::glowstick_sync
