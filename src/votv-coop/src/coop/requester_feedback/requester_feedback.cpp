// coop/requester_feedback/requester_feedback.cpp -- see coop/requester_feedback/requester_feedback.h.

#include "coop/requester_feedback/requester_feedback.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/text/utf8_codec.h"

#include "ue_wrap/core/ftext_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/votv_lib.h"

#include <atomic>
#include <cstring>
#include <string>
#include <utility>

namespace coop::requester_feedback {
namespace {

namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;
namespace UH = ue_wrap::ufunction_hook;
namespace E  = ue_wrap::engine;
namespace RF = coop::net::requester_feedback;
using coop::net::RequesterFeedbackPayload;

constexpr int kTagHint  = 0x52464201;  // 'RFB' 1
constexpr int kTagGloss = 0x52464202;  // 'RFB' 2
UE_FACT_FN(kRowAddHint, "requester_feedback", nullptr, L"addHint", AnyClass);
UE_FACT_FN(kRowAddGloss, "requester_feedback", L"lib_C", L"addGloss", Declared);
UE_FACT_FN(kRowPlaySound2D, "requester_feedback", L"GameplayStatics", L"PlaySound2D", Declared);

// A line per kind for the first few, then the counts at the session's end say the rest.
constexpr uint64_t kLinesPerKind = 20;

std::atomic<coop::net::Session*> g_session{nullptr};

// The hint seam: the gate's watch on addHint, whatever class carries it, and the offsets read per function (a
// second builder, ui_UI's addHinT, would match the same name and has no such parameters: it runs). Refused for this
// process when the watch cannot be registered or settles dead.
bool           g_hintWatched = false;
bool           g_hintLive = false;
bool           g_hintRefused = false;
R::ParamOffset g_offHintText{L"InText"};
R::ParamOffset g_offHintType{L"type"};

// The gloss seam: the gate's watch on lib_C::addGloss, and its parameters' offsets. Refused for this process when
// the watch cannot be registered or settles dead, or when addGloss or its parameters do not read on a loaded lib_C.
bool    g_glossWatched = false;
bool    g_glossLive = false;
bool    g_glossRefused = false;
int32_t g_offGlossName = -1, g_offGlossLevel = -1;

// The sound seam: the pre hook on GameplayStatics::PlaySound2D, and its parameters' offsets.
void*   g_soundFn = nullptr;
bool    g_soundHooked = false;
bool    g_soundRefused = false;
int32_t g_offSound = -1, g_offVolume = -1, g_offPitch = -1, g_offStart = -1, g_offUi = -1;

// The verb running for a client, while a Scope is open.
bool     g_open = false;
uint8_t  g_slot = 0;
uint32_t g_seq = 0;

// A body whose glosses are a client's while it runs (ForwardInBody): the refiner's decode a client began.
void*    g_bodyFn = nullptr;
uint8_t  g_bodySlot = 0xFF;
uint32_t g_bodySeq = 0;

Counts g_counts;
bool   g_saidSoundMissing = false;
bool   g_saidSoundRefused = false;
bool   g_saidHostScope = false;

// Never set in a shipped run: a Scope does nothing, so the host shows its own feedback again (the red arm).
bool Off() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::requester_feedback_off);
    return s;
}

// Whether a Scope for `slot` opens: not while the red arm is on, and never for the host's own slot or a slot no peer
// holds, since a host's own verb keeps its feedback. The refusal is counted and said once a session.
bool Admit(uint8_t slot) {
    if (Off()) return false;
    if (slot != 0 && slot < coop::net::kMaxPeers) return true;
    ++g_counts.scopesRefused;
    if (!g_saidHostScope) {
        g_saidHostScope = true;
        UE_LOGW("[REQUESTER-FEEDBACK] a scope for slot %u was refused -- the host's own verb keeps its feedback "
                "(said once)", static_cast<unsigned>(slot));
    }
    return false;
}

// `text` into the payload as UTF-8. A hint's text is cut to `cutAt` bytes on a code-point boundary (`cutAt` 0 cuts
// nothing); a name is never cut: a gloss or sound name that does not fit the payload is refused, since a cut one
// would name another entry or no sound. False for text that is empty or does not fit.
bool PutText(RequesterFeedbackPayload& p, const std::wstring& text, size_t cutAt = 0) {
    std::string u = coop::text::ToUtf8Exact(text);
    if (cutAt != 0) u = coop::text::CapUtf8Bytes(std::move(u), cutAt);
    if (u.empty() || u.size() > RF::kTextCap) return false;
    std::memcpy(p.text, u.data(), u.size());
    p.textLen = static_cast<uint8_t>(u.size());
    return true;
}

bool SendTo(uint8_t slot, uint32_t seq, RequesterFeedbackPayload& p) {
    auto* s = g_session.load(std::memory_order_acquire);
    p.seq = seq;
    if (!s || !s->connected() ||
        !s->SendReliableToSlot(slot, coop::net::ReliableKind::RequesterFeedback, &p, static_cast<int>(sizeof(p)))) {
        ++g_counts.lost;
        return false;
    }
    return true;
}

// A hint a Scope's verb raises is its requester's: refused here, sent there.
sg::Verdict OnHintPre(const sg::Call& c) {
    if (!g_open) return sg::Verdict::Run;
    const int32_t offText = g_offHintText.Of(c.function);
    const int32_t offType = g_offHintType.Of(c.function);
    if (offText < 0 || offType < 0 || !c.locals) return sg::Verdict::Run;  // not lib_C's builder: the hint stays here
    const std::wstring text = ue_wrap::ftext_utils::FTextToString(c.locals + offText);
    RequesterFeedbackPayload p{};
    p.op = RF::kOpHint;
    p.hintType = c.locals[offType];
    if (!PutText(p, text, RF::kHintTextMax)) {
        ++g_counts.lost;
        UE_LOGW("[REQUESTER-FEEDBACK] slot %u's hint '%ls' cannot cross the wire -- refused here, not sent",
                static_cast<unsigned>(g_slot), text.c_str());
        return sg::Verdict::Cancel;
    }
    if (!SendTo(g_slot, g_seq, p)) return sg::Verdict::Cancel;
    if (++g_counts.hintsSent <= kLinesPerKind)
        UE_LOGI("[REQUESTER-FEEDBACK] routed hint type=%u to slot %u (#%u): \"%ls\"", static_cast<unsigned>(p.hintType),
                static_cast<unsigned>(g_slot), g_seq, text.c_str());
    return sg::Verdict::Cancel;
}

// The gloss a scope's verb adds, or a client's decode finishing in a forwarded body, is that client's: refused
// here, sent there.
sg::Verdict OnGlossPre(const sg::Call& c) {
    uint8_t slot = g_slot;
    uint32_t seq = g_seq;
    if (!g_open) {
        if (!g_bodyFn || g_bodySlot == 0xFF || !sg::IsBodyActive(g_bodyFn)) return sg::Verdict::Run;
        slot = g_bodySlot;
        seq = g_bodySeq;
    }
    if (g_offGlossName < 0 || g_offGlossLevel < 0) return sg::Verdict::Run;  // unread: the gloss stays here
    const int32_t level = *reinterpret_cast<const int32_t*>(c.locals + g_offGlossLevel);
    const std::wstring name = R::ToString(*reinterpret_cast<const R::FName*>(c.locals + g_offGlossName));
    RequesterFeedbackPayload p{};
    p.op = RF::kOpGloss;
    if (level < 0 || level > 255 || !PutText(p, name)) {
        ++g_counts.lost;
        UE_LOGW("[REQUESTER-FEEDBACK] slot %u's gloss '%ls' (level %d) cannot cross the wire -- refused here, not sent",
                static_cast<unsigned>(slot), name.c_str(), level);
        return sg::Verdict::Cancel;
    }
    p.glossLevel = static_cast<uint8_t>(level);
    if (!SendTo(slot, seq, p)) return sg::Verdict::Cancel;
    if (++g_counts.glossesSent <= kLinesPerKind)
        UE_LOGI("[REQUESTER-FEEDBACK] routed gloss level=%d to slot %u (#%u): '%ls'%s", level,
                static_cast<unsigned>(slot), seq, name.c_str(), g_open ? "" : " (a forwarded body's)");
    return sg::Verdict::Cancel;
}

// A 2D sound a scope's verb plays is heard by its requester: refused here, sent there.
sg::Verdict OnSound2DPre(void*, void*, const uint8_t* parms) {
    if (!g_open) return sg::Verdict::Run;
    void* sound = *reinterpret_cast<void* const*>(parms + g_offSound);
    RequesterFeedbackPayload p{};
    p.op = RF::kOpSound2D;
    p.volume = *reinterpret_cast<const float*>(parms + g_offVolume);
    p.pitch = *reinterpret_cast<const float*>(parms + g_offPitch);
    p.startTime = *reinterpret_cast<const float*>(parms + g_offStart);
    p.uiSound = parms[g_offUi] ? 1 : 0;
    const std::wstring name = sound ? E::SoundName(sound) : std::wstring();
    if (!PutText(p, name)) {
        ++g_counts.lost;
        if (!g_saidSoundRefused) {
            g_saidSoundRefused = true;
            UE_LOGW("[REQUESTER-FEEDBACK] slot %u's 2D sound '%ls' cannot cross the wire -- refused here, not sent "
                    "(said once)", static_cast<unsigned>(g_slot), name.c_str());
        }
        return sg::Verdict::Cancel;
    }
    if (!SendTo(g_slot, g_seq, p)) return sg::Verdict::Cancel;
    if (++g_counts.soundsSent <= kLinesPerKind)
        UE_LOGI("[REQUESTER-FEEDBACK] routed sound2d to slot %u (#%u): '%ls'", static_cast<unsigned>(g_slot), g_seq,
                name.c_str());
    return sg::Verdict::Cancel;
}

// A name watch's registration and liveness, retried until it settles. `what` names the seam in the refusals.
void InstallWatch(const ue_wrap::facts::Function& row, int tag, sg::PreFn pre, const char* what, bool& watched,
                  bool& live, bool& refused) {
    if (!watched && !refused) {
        watched = sg::WatchName(row, tag, pre, nullptr);
        if (!watched) {
            refused = true;  // a full watch table or no gate: said by the gate, and final
            UE_LOGE("[REQUESTER-FEEDBACK] the gate took no watch on %s -- a verb the host runs for a client will not "
                    "run", what);
        }
    }
    if (watched && !live && !refused) {
        sg::ResolvePendingNames();
        live = sg::NameWatchLive(row, tag);
        if (!live && sg::NameWatchSettled(row, tag)) {
            refused = true;
            UE_LOGE("[REQUESTER-FEEDBACK] the watch on %s settled dead -- a verb the host runs for a client will not "
                    "run", what);
        }
    }
}

void ResolveGloss() {
    if (g_glossRefused || (g_offGlossName >= 0 && g_offGlossLevel >= 0)) return;
    void* cls = ue_wrap::object_index::ClassByName(L"lib_C");
    if (!cls) return;  // loading: a later tick finds it
    void* fn = R::FindFunction(cls, L"addGloss");
    g_offGlossName = fn ? R::FindParamOffset(fn, L"name") : -1;
    g_offGlossLevel = fn ? R::FindParamOffset(fn, L"level") : -1;
    if (g_offGlossName < 0 || g_offGlossLevel < 0) {
        g_glossRefused = true;
        UE_LOGE("[REQUESTER-FEEDBACK] lib_C's addGloss or its parameters do not read (fn=%p name=%d level=%d) -- a "
                "verb the host runs for a client will not run", fn, g_offGlossName, g_offGlossLevel);
    }
}

void InstallSound() {
    if (g_soundHooked || g_soundRefused) return;
    const ue_wrap::facts::Resolved soundFn = ue_wrap::facts::Resolve(kRowPlaySound2D);
    g_soundFn = soundFn.Fn();
    if (!soundFn) return;
    g_offSound = R::FindParamOffset(g_soundFn, L"Sound");
    g_offVolume = R::FindParamOffset(g_soundFn, L"VolumeMultiplier");
    g_offPitch = R::FindParamOffset(g_soundFn, L"PitchMultiplier");
    g_offStart = R::FindParamOffset(g_soundFn, L"StartTime");
    g_offUi = R::FindParamOffset(g_soundFn, L"bIsUISound");
    if (g_offSound < 0 || g_offVolume < 0 || g_offPitch < 0 || g_offStart < 0 || g_offUi < 0 ||
        !UH::InstallPreHook(soundFn, &OnSound2DPre, /*armed=*/false)) {
        g_soundRefused = true;
        UE_LOGE("[REQUESTER-FEEDBACK] no pre hook on PlaySound2D (offsets %d/%d/%d/%d/%d) -- a verb the host runs for "
                "a client will not run", g_offSound, g_offVolume, g_offPitch, g_offStart, g_offUi);
        return;
    }
    g_soundHooked = true;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    InstallWatch(kRowAddHint, kTagHint, &OnHintPre, "addHint", g_hintWatched, g_hintLive, g_hintRefused);
    InstallWatch(kRowAddGloss, kTagGloss, &OnGlossPre, "lib_C::addGloss", g_glossWatched, g_glossLive,
                 g_glossRefused);
    ResolveGloss();
    InstallSound();
}

bool Ready() {
    return g_hintLive && g_glossLive && g_offGlossName >= 0 && g_offGlossLevel >= 0 && g_soundHooked;
}

bool Refused() { return g_hintRefused || g_glossRefused || g_soundRefused; }

Counts CountsNow() { return g_counts; }

Scope::Scope(uint8_t slot, uint32_t seq)
    : outerSlot_(g_slot), outerSeq_(g_seq), outerOpen_(g_open), active_(Admit(slot)) {
    if (!active_) return;
    g_slot = slot;
    g_seq = seq;
    g_open = true;
    if (g_soundHooked) UH::SetArmed(g_soundFn, &OnSound2DPre, true);
}

Scope::~Scope() {
    if (!active_) return;
    g_slot = outerSlot_;
    g_seq = outerSeq_;
    g_open = outerOpen_;
    if (g_soundHooked && !g_open) UH::SetArmed(g_soundFn, &OnSound2DPre, false);
}

uint8_t CurrentSlot() { return g_open ? g_slot : 0xFF; }

uint32_t CurrentSeq() { return g_open ? g_seq : 0; }

void ForwardInBody(void* bodyFn, uint8_t slot, uint32_t seq) {
    g_bodyFn = slot == 0xFF ? nullptr : bodyFn;
    g_bodySlot = slot;
    g_bodySeq = seq;
}

void OnFeedback(const RequesterFeedbackPayload& p) {
    if (p.op > RF::kOpSound2D || (p.op == RF::kOpHint && p.hintType > RF::kHintTypeMax)) {
        ++g_counts.badOps;
        UE_LOGW("[REQUESTER-FEEDBACK] the host's feedback (#%u) has an unknown op %u or hint type %u -- dropped",
                p.seq, static_cast<unsigned>(p.op), static_cast<unsigned>(p.hintType));
        return;
    }
    std::wstring text;
    const size_t cap = p.op == RF::kOpHint ? RF::kHintTextMax : RF::kTextCap;
    if (p.textLen == 0 || p.textLen > cap || !coop::text::FromUtf8Strict(p.text, p.textLen, &text)) {
        ++g_counts.lost;
        UE_LOGW("[REQUESTER-FEEDBACK] the host's feedback op %u (#%u) has a malformed text (%u bytes) -- dropped",
                static_cast<unsigned>(p.op), p.seq, static_cast<unsigned>(p.textLen));
        return;
    }
    // A hint, a gloss and a sound are made through this machine's player.
    void* local = coop::players::Registry::Get().Local();
    if (!local) {
        ++g_counts.lost;
        UE_LOGW("[REQUESTER-FEEDBACK] the host's feedback op %u (#%u) came with no player here -- dropped",
                static_cast<unsigned>(p.op), p.seq);
        return;
    }
    switch (p.op) {
    case RF::kOpHint:
        if (!ue_wrap::votv_lib::AddHint(text, p.hintType, local)) {
            ++g_counts.lost;
            UE_LOGW("[REQUESTER-FEEDBACK] the host's hint (#%u) did not run here", p.seq);
            return;
        }
        if (++g_counts.hintsShown <= kLinesPerKind)
            UE_LOGI("[REQUESTER-FEEDBACK] showed hint \"%ls\" type=%u (#%u)", text.c_str(),
                    static_cast<unsigned>(p.hintType), p.seq);
        return;
    case RF::kOpGloss:
        if (!ue_wrap::votv_lib::AddGloss(text, p.glossLevel, local)) {
            ++g_counts.lost;
            UE_LOGW("[REQUESTER-FEEDBACK] the host's gloss '%ls' (#%u) did not run here", text.c_str(), p.seq);
            return;
        }
        if (++g_counts.glossesMade <= kLinesPerKind)
            UE_LOGI("[REQUESTER-FEEDBACK] showed gloss '%ls' level=%u (#%u)", text.c_str(),
                    static_cast<unsigned>(p.glossLevel), p.seq);
        return;
    case RF::kOpSound2D: {
        // No asset load on a host's word: a sound not loaded here is dropped.
        void* sound = E::FindSound(text);
        if (!sound) {
            ++g_counts.lost;
            if (!g_saidSoundMissing) {
                g_saidSoundMissing = true;
                UE_LOGW("[REQUESTER-FEEDBACK] the sound '%ls' (#%u) is not loaded here -- dropped (said once)",
                        text.c_str(), p.seq);
            }
            return;
        }
        E::PlaySound2D(local, sound, p.volume, p.pitch, p.startTime, p.uiSound != 0);
        if (++g_counts.soundsMade <= kLinesPerKind)
            UE_LOGI("[REQUESTER-FEEDBACK] showed sound2d '%ls' (#%u)", text.c_str(), p.seq);
        return;
    }
    }
}

void OnDisconnect() {
    const Counts& c = g_counts;
    if (c.hintsSent || c.glossesSent || c.soundsSent || c.hintsShown || c.glossesMade || c.soundsMade || c.lost ||
        c.badOps || c.scopesRefused)
        UE_LOGI("[REQUESTER-FEEDBACK] session end -- routed hints=%llu glosses=%llu sounds=%llu, showed hints=%llu "
                "glosses=%llu sounds=%llu, lost=%llu bad=%llu scopes refused=%llu", c.hintsSent, c.glossesSent,
                c.soundsSent, c.hintsShown, c.glossesMade, c.soundsMade, c.lost, c.badOps, c.scopesRefused);
    g_counts = Counts{};
    g_saidSoundMissing = false;
    g_saidSoundRefused = false;
    g_saidHostScope = false;
    g_open = false;
    g_slot = 0;
    g_seq = 0;
    g_bodyFn = nullptr;
    g_bodySlot = 0xFF;
    g_bodySeq = 0;
    if (g_soundHooked) UH::SetArmed(g_soundFn, &OnSound2DPre, false);
}

}  // namespace coop::requester_feedback
