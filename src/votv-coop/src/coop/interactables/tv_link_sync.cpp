// coop/interactables/tv_link_sync.cpp -- see coop/interactables/tv_link_sync.h.

#include "coop/interactables/tv_link_sync.h"

#include "coop/comms/chat_feed.h"
#include "coop/config/config.h"
#include "coop/interactables/tv_link_resolver.h"
#include "coop/net/net_clock.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "l10n/l10n.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/ftext_utils.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/devices/tv.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/game_rules.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace coop::tv_link_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace SG = ue_wrap::script_gate;
namespace TV = ue_wrap::tv;
namespace RS = coop::tv_link_resolver;
namespace N  = coop::net;

UE_FACT_FN(kRowOpenLink, "tv_link_sync", L"prop_tv2_C", L"openLink", Declared);
UE_FACT_FN(kRowSearchCommitted, "tv_link_sync", L"ui_tvInterface_C",
           L"BndEvt__ui_tvInterface_textbox_search_K2Node_ComponentBoundEvent_13_OnEditableTextBoxCommittedEvent__DelegateSignature",
           Declared);
UE_FACT_FN(kRowPlayClicked, "tv_link_sync", L"ui_tvInterface_C",
           L"BndEvt__umg_onlineRadio_Button_play_K2Node_ComponentBoundEvent_0_OnButtonClickedEvent__DelegateSignature", Declared);
UE_FACT_FN(kRowStopClicked, "tv_link_sync", L"ui_tvInterface_C",
           L"BndEvt__umg_onlineRadio_Button_stop_K2Node_ComponentBoundEvent_1_OnButtonClickedEvent__DelegateSignature", Declared);
UE_FACT_FN(kRowSliderReleased, "tv_link_sync", L"ui_tvInterface_C",
           L"BndEvt__umg_tvOffline_Slider_playback_K2Node_ComponentBoundEvent_7_OnMouseCaptureEndEvent__DelegateSignature",
           Declared);

constexpr int kTagOpenLink = 0x54564C00;   // 'TVL'
constexpr int kTagCommitted = 0x54564C01;
constexpr int kTagPlay = 0x54564C02;
constexpr int kTagStop = 0x54564C03;
constexpr int kTagSlider = 0x54564C04;
constexpr uint8_t kCommitOnEnter = 1;      // ETextCommit::OnEnter
constexpr uint64_t kParkMs = 180000;       // a joiner's world can take minutes to hold its sets
constexpr uint64_t kStepMs = 500;
constexpr uint64_t kSendEveryMs = 1500;    // one address a peer, at most, in this time
constexpr uint64_t kControlEveryMs = 250;  // and one pause, resume or seek in this
constexpr uint64_t kCatchUpWaitMs = 90000; // how long a set may take to start playing before its catch-up is dropped
constexpr uint32_t kCatchUpFromMs = 3000;  // a set this far in is worth seeking to
constexpr uint32_t kMaxPosMs = 24u * 3600u * 1000u;
constexpr size_t kMaxParked = 8;

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_watched = false;
int  g_applying = 0;            // inside an openLink this module called: the watch sends nothing
bool g_saidRuleOff = false;     // the Custom Content line, once a session
uint64_t g_opened = 0;
uint64_t g_controls = 0;
uint64_t g_lastSendMs = 0;
uint64_t g_lastControlMs = 0;
uint64_t g_nextStepMs = 0;

struct Link {
    std::wstring key;
    std::wstring link;
    uint32_t posMs = 0;     // where the sender's set was
    uint64_t atMs = 0;      // when this peer learned that
    uint64_t untilMs = 0;
};
std::vector<Link> g_parked;     // a link whose set is not in this world yet
std::vector<Link> g_catchUp;    // a set opened mid-video: it seeks once it plays

bool On() {
    static const bool on = coop::config::ResolveFlag(::coop::config_registry::rows::tv_links);
    return on;
}

// The host's rule, replicated to every peer: players other than the host may put videos on sets and control them.
bool PlayersMay() { return coop::config::ResolveFlag(::coop::config_registry::rows::net_tv_links_players); }

// This peer may send: the host always, a client while the rule allows.
bool MaySend(const coop::net::Session* s) { return s && (s->role() == coop::net::Role::Host || PlayersMay()); }

// A line for this player alone, in the chat feed.
void Say(const char* line) {
    UE_LOGI("tv_link: said to this player: %s", line);
    coop::chat_feed::Push(std::string(line), coop::chat_feed::Keep::Transient);
}

void* FindByKey(const std::wstring& key) {
    for (void* tv : TV::FindAll())
        if (ue_wrap::prop::GetInteractableKeyString(tv) == key) return tv;
    return nullptr;
}

uint32_t PosMsOf(double seconds) {
    if (!(seconds > 0.0)) return 0;
    const double ms = seconds * 1000.0;
    return ms >= static_cast<double>(kMaxPosMs) ? kMaxPosMs : static_cast<uint32_t>(ms);
}

void Send(coop::net::Session* s, const std::wstring& key, uint8_t op, const std::wstring& link, uint32_t posMs,
          int onlySlot = -1) {
    N::TvLinkPayload p{};
    N::WireKeyFromString(key, p.key);
    p.op = op;
    p.posMs = posMs;
    p.linkLen = static_cast<uint16_t>(link.size());
    for (size_t i = 0; i < link.size(); ++i) p.link[i] = static_cast<char>(link[i]);  // Sendable: ASCII, in bounds
    if (onlySlot >= 0)
        s->SendReliableToSlot(onlySlot, N::ReliableKind::TvLink, &p, sizeof(p));
    else
        s->SendReliable(N::ReliableKind::TvLink, &p, sizeof(p));
}

coop::net::Session* Connected() {
    auto* s = g_session.load(std::memory_order_acquire);
    return (s && s->running() && s->connected()) ? s : nullptr;
}

// Hand `direct` to the set's own openLink, shown on its remote under `shown`. `posMs` is where the sender's set
// already was: far enough in, this set seeks there once it plays.
void Open(void* tv, const std::wstring& key, const std::wstring& shown, const std::wstring& direct, const char* from,
          bool typedHere, uint32_t posMs, uint64_t posAtMs) {
    if (ue_wrap::game_rules::ReadCustomContent() == 0) {
        UE_LOGW("tv_link: '%ls' not opened (%s) -- this save's Custom Content rule is off, and the game refuses "
                "a television's web address without it", shown.c_str(), from);
        if (typedHere) {
            // The player who typed it is told by the game itself: its openLink shows the refusal's hint.
            ++g_applying;
            TV::OpenLink(tv, shown, direct);
            --g_applying;
        } else if (!g_saidRuleOff) {
            g_saidRuleOff = true;
            Say(l10n::T("TV: a player's video link was not played here. This save's Custom Content rule is off."));
        }
        return;
    }
    if (!TV::Powered(tv)) {
        UE_LOGI("tv_link: '%ls' not opened (%s) -- the set has no power", shown.c_str(), from);
        if (typedHere) Say(l10n::T("TV: this set has no power, so the link was not played."));
        return;
    }
    if (!TV::TakeAnyChannel(tv)) {
        UE_LOGW("tv_link: '%ls' not opened (%s) -- the set got no media channel", shown.c_str(), from);
        if (typedHere) Say(l10n::T("TV: every media channel is in use, so the link was not played."));
        return;
    }
    ++g_applying;
    const bool ran = TV::OpenLink(tv, shown, direct);
    --g_applying;
    if (ran) ++g_opened;
    UE_LOGI("tv_link: '%ls' handed to the set's openLink (%s): %d", shown.c_str(), from, ran ? 1 : 0);
    for (size_t i = g_catchUp.size(); i-- > 0;)
        if (g_catchUp[i].key == key) g_catchUp.erase(g_catchUp.begin() + static_cast<std::ptrdiff_t>(i));
    if (ran && posMs >= kCatchUpFromMs) {
        Link c;
        c.key = key;
        c.posMs = posMs;
        c.atMs = posAtMs;
        c.untilMs = coop::net::NowMs() + kCatchUpWaitMs;
        g_catchUp.push_back(std::move(c));
    }
}

// Play `link` on this peer's copy of the set: at once, or after the tool has named its stream.
void Play(void* tv, const std::wstring& key, const std::wstring& link, const char* from, bool typedHere, uint32_t posMs,
          uint64_t posAtMs) {
    if (!RS::NeedsResolve(link)) {
        Open(tv, key, link, link, from, typedHere, posMs, posAtMs);
        return;
    }
    if (!RS::ToolPresent()) {
        UE_LOGW("tv_link: '%ls' (%s) is a YouTube address and this install has no tools\\yt-dlp.exe", link.c_str(), from);
        if (typedHere) Say(l10n::T("TV: this install has no yt-dlp, so a YouTube link cannot play."));
        return;
    }
    UE_LOGI("tv_link: '%ls' (%s) is a YouTube address -- resolving it here", link.c_str(), from);
    if (typedHere) Say(l10n::T("TV: getting the video from YouTube..."));
    const std::string fromCopy(from);
    RS::Resolve(link, [key, link, fromCopy, typedHere, posMs, posAtMs](std::wstring direct) {
        if (direct.empty()) {
            UE_LOGW("tv_link: '%ls' was not resolved -- nothing is played", link.c_str());
            Say(typedHere ? l10n::T("TV: the YouTube link could not be played. The reason is in multivoid.log.")
                          : l10n::T("TV: a player's YouTube link could not be played here."));
            return;
        }
        if (void* set = FindByKey(key)) Open(set, key, link, direct, fromCopy.c_str(), typedHere, posMs, posAtMs);
    });
}

// A set was asked for a video by the game's own path (a row of its remote's list): a web address is
// sent, a file of this player's folder is not.
void OnOpenLinkPost(const SG::Call& call) {
    if (g_applying > 0 || !On() || !call.locals || !TV::IsTv(call.object)) return;
    auto* s = Connected();
    if (!s || !MaySend(s)) return;
    std::wstring link;
    bool isUrl = false;
    if (!TV::ReadOpenLinkArgs(call.function, call.locals, nullptr, &link, &isUrl) || !isUrl) return;
    if (!RS::Sendable(link)) {
        UE_LOGI("tv_link: a set opened an address this lane does not send (%zu characters)", link.size());
        return;
    }
    const std::wstring key = ue_wrap::prop::GetInteractableKeyString(call.object);
    if (key.empty() || key == L"None") return;
    const uint64_t now = coop::net::NowMs();
    if (now - g_lastSendMs < kSendEveryMs) return;
    g_lastSendMs = now;
    Send(s, key, N::kTvOpPlay, link, 0);
    UE_LOGI("tv_link: key='%ls' opened '%ls' here -- sent", key.c_str(), link.c_str());
}

// Enter in the search box of a set's remote: a text that is an address is a link to play.
void OnSearchCommittedPost(const SG::Call& call) {
    if (!On() || !call.locals || !call.object) return;
    static void* sFn = nullptr;
    static int32_t sTextOff = -1, sMethodOff = -1;
    if (sFn != call.function) {
        sFn = call.function;
        sTextOff = R::FindParamOffset(call.function, L"Text");
        sMethodOff = R::FindParamOffset(call.function, L"CommitMethod");
    }
    if (sTextOff < 0 || sMethodOff < 0 || call.locals[sMethodOff] != kCommitOnEnter) return;
    const std::wstring text = ue_wrap::ftext_utils::FTextToString(call.locals + sTextOff);
    if (text.size() < 8 || (text.compare(0, 7, L"http://") != 0 && text.compare(0, 8, L"https://") != 0)) return;
    void* tv = TV::RemoteSet(call.object);
    if (!tv) return;
    if (!PlayLocal(tv, text)) {
        UE_LOGW("tv_link: the address typed on the remote was refused ('%ls')", text.c_str());
        Say(l10n::T("TV: that link cannot be used. It must be an http or https address with no spaces or quotes."));
    }
    // The box held the address, not a search: emptied, the remote shows its list again.
    const bool cleared = TV::ClearRemoteSearch(call.object);
    UE_LOGI("tv_link: the remote's search box emptied after the link: %d", cleared ? 1 : 0);
}

// The remote's play and stop buttons and its playback slider, after the game has run them on this peer's set.
void OnRemoteControl(const SG::Call& call, uint8_t op) {
    if (!On() || !call.object) return;
    void* tv = TV::RemoteSet(call.object);
    if (!tv || !TV::PlayerOf(tv)) return;
    const double at = op == N::kTvOpSeek ? TV::RemoteSliderSeconds(call.object) : TV::PlayTime(tv);
    ControlLocal(tv, op, at);
}
void OnPlayClickedPost(const SG::Call& call) { OnRemoteControl(call, N::kTvOpResume); }
void OnStopClickedPost(const SG::Call& call) { OnRemoteControl(call, N::kTvOpPause); }
void OnSliderReleasedPost(const SG::Call& call) { OnRemoteControl(call, N::kTvOpSeek); }

// Another peer's pause, resume or seek, done to this peer's copy of the set through its player's own verbs.
void ApplyControl(void* tv, const std::wstring& key, uint8_t op, uint32_t posMs, unsigned senderSlot) {
    const double at = static_cast<double>(posMs) / 1000.0;
    bool ran = false;
    const char* what = "?";
    switch (op) {
    case N::kTvOpPause:
        what = "pause";
        ran = TV::Pause(tv);
        break;
    case N::kTvOpResume:
        what = "resume";
        if (posMs > 0) TV::Seek(tv, at);
        ran = TV::Resume(tv);
        break;
    case N::kTvOpSeek:
        what = "seek";
        ran = TV::Seek(tv, at);
        break;
    default:
        return;
    }
    if (ran) ++g_controls;
    UE_LOGI("tv_link: key='%ls' %s at %.1fs from slot %u: %d", key.c_str(), what, at, senderSlot, ran ? 1 : 0);
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_watched) return;
    g_watched = true;
    const bool a = SG::WatchName(kRowOpenLink, kTagOpenLink, nullptr, &OnOpenLinkPost);
    const bool b = SG::WatchName(kRowSearchCommitted, kTagCommitted, nullptr, &OnSearchCommittedPost);
    const bool c = SG::WatchName(kRowPlayClicked, kTagPlay, nullptr, &OnPlayClickedPost);
    const bool d = SG::WatchName(kRowStopClicked, kTagStop, nullptr, &OnStopClickedPost);
    const bool e = SG::WatchName(kRowSliderReleased, kTagSlider, nullptr, &OnSliderReleasedPost);
    if (a && b && c && d && e)
        UE_LOGI("tv_link: the television's openLink and its remote's search box, buttons and slider are watched "
                "(links %s, tool %s)", On() ? "on" : "OFF by tv_links=0", RS::ToolPresent() ? "present" : "absent");
    else
        UE_LOGE("tv_link: a watch was refused (openLink %d, search box %d, play %d, stop %d, slider %d) -- that part of "
                "a television does not cross", a ? 1 : 0, b ? 1 : 0, c ? 1 : 0, d ? 1 : 0, e ? 1 : 0);
}

bool PlayLocal(void* tv, const std::wstring& link) {
    if (!On() || !TV::IsTv(tv) || !RS::ValidLink(link)) return false;
    if (auto* session = Connected(); session && !MaySend(session)) {
        UE_LOGI("tv_link: a link typed here is not played -- the host's rule keeps TV links to the host");
        Say(l10n::T("TV: the host has turned off video links from players."));
        return true;
    }
    const std::wstring key = ue_wrap::prop::GetInteractableKeyString(tv);
    if (!RS::Sendable(link)) {
        UE_LOGI("tv_link: key='%ls' -- an address of %zu characters is past the wire's %d; it plays here only", key.c_str(),
                link.size(), N::kTvLinkMax);
        Say(l10n::T("TV: that link is too long to share, so it plays on your screen only."));
    } else if (auto* s = Connected(); s && !key.empty() && key != L"None") {
        Send(s, key, N::kTvOpPlay, link, 0);
        g_lastSendMs = coop::net::NowMs();
    }
    UE_LOGI("tv_link: key='%ls' asked for '%ls' on this machine", key.c_str(), link.c_str());
    Play(tv, key, link, "typed here", /*typedHere=*/true, 0, 0);
    return true;
}

bool ControlLocal(void* tv, uint8_t op, double seconds) {
    if (!On() || !TV::IsTv(tv) || op < N::kTvOpPause || op > N::kTvOpSeek) return false;
    auto* s = Connected();
    const std::wstring key = ue_wrap::prop::GetInteractableKeyString(tv);
    if (!s || !MaySend(s) || key.empty() || key == L"None") return false;
    const uint64_t now = coop::net::NowMs();
    if (now - g_lastControlMs < kControlEveryMs) return false;
    g_lastControlMs = now;
    Send(s, key, op, std::wstring(), PosMsOf(seconds));
    UE_LOGI("tv_link: key='%ls' %s at %.1fs here -- sent", key.c_str(),
            op == N::kTvOpPause ? "pause" : op == N::kTvOpResume ? "resume" : "seek", seconds);
    return true;
}

void OnLink(const coop::net::TvLinkPayload& p, uint8_t senderSlot) {
    if (!On()) return;
    const std::wstring key = N::StringFromWireKey(p.key);
    if (key.empty() || p.posMs > kMaxPosMs) return;
    if (senderSlot != 0 && !PlayersMay()) {
        static bool sSaid = false;
        if (!sSaid) UE_LOGI("tv_link: slot %u's television message is ignored -- the host's rule keeps them to the host",
                            static_cast<unsigned>(senderSlot));
        sSaid = true;
        return;
    }
    if (p.op != N::kTvOpPlay) {
        if (void* tv = FindByKey(key)) ApplyControl(tv, key, p.op, p.posMs, senderSlot);
        return;
    }
    if (p.linkLen == 0 || p.linkLen > N::kTvLinkMax) return;
    std::wstring link(p.linkLen, L'\0');
    for (size_t i = 0; i < p.linkLen; ++i) link[i] = static_cast<wchar_t>(static_cast<unsigned char>(p.link[i]));
    if (!RS::Sendable(link)) {
        UE_LOGW("tv_link: slot %u sent an address this lane refuses (%u characters)", static_cast<unsigned>(senderSlot),
                static_cast<unsigned>(p.linkLen));
        return;
    }
    const uint64_t now = coop::net::NowMs();
    char from[24];
    std::snprintf(from, sizeof(from), "slot %u", static_cast<unsigned>(senderSlot));
    if (void* tv = FindByKey(key)) {
        Play(tv, key, link, from, /*typedHere=*/false, p.posMs, now);
        return;
    }
    for (size_t i = g_parked.size(); i-- > 0;)
        if (g_parked[i].key == key) g_parked.erase(g_parked.begin() + static_cast<std::ptrdiff_t>(i));
    if (g_parked.size() >= kMaxParked) g_parked.erase(g_parked.begin());
    Link parked;
    parked.key = key;
    parked.link = link;
    parked.posMs = p.posMs;
    parked.atMs = now;
    parked.untilMs = now + kParkMs;
    g_parked.push_back(std::move(parked));
    UE_LOGI("tv_link: key='%ls' link parked (no such set here yet)", key.c_str());
}

void Tick() {
    if (g_parked.empty() && g_catchUp.empty()) return;
    const uint64_t now = coop::net::NowMs();
    if (now < g_nextStepMs) return;
    g_nextStepMs = now + kStepMs;
    for (size_t i = g_parked.size(); i-- > 0;) {
        const Link parked = g_parked[i];
        void* tv = FindByKey(parked.key);
        if (tv || now > parked.untilMs) g_parked.erase(g_parked.begin() + static_cast<std::ptrdiff_t>(i));
        if (tv) Play(tv, parked.key, parked.link, "parked", /*typedHere=*/false, parked.posMs, parked.atMs);
    }
    // A set opened mid-video seeks to where the sender's set is by now, once its own player has started.
    for (size_t i = g_catchUp.size(); i-- > 0;) {
        const Link c = g_catchUp[i];
        void* tv = FindByKey(c.key);
        const bool playing = tv && TV::IsPlaying(tv) == 1 && TV::PlayTime(tv) >= 0.0;
        if (!playing && tv && now <= c.untilMs) continue;
        g_catchUp.erase(g_catchUp.begin() + static_cast<std::ptrdiff_t>(i));
        if (!playing) continue;
        const double length = TV::Duration(tv);
        double target = (static_cast<double>(c.posMs) + static_cast<double>(now - c.atMs)) / 1000.0;
        if (length > 1.0 && target >= length) target = std::fmod(target, length);  // a clip that plays round
        const bool ran = TV::Seek(tv, target);
        if (ran) ++g_controls;
        UE_LOGI("tv_link: key='%ls' joined mid-video -- seek to %.1fs of %.1fs: %d", c.key.c_str(), target, length, ran ? 1 : 0);
    }
}

void QueueConnectBroadcastForSlot(int slot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!On() || !s || s->role() != N::Role::Host) return;
    // What each set of the host's world is playing now, read from the set: nothing is remembered between
    // sessions, and a set that has stopped has nothing to bring a joiner. This lane opens a link under the
    // address as typed for its name, so that is what a YouTube video is sent as; a row of the game's own list
    // is sent as its address.
    for (void* tv : TV::FindAll()) {
        std::wstring name, link;
        bool isUrl = false;
        if (TV::IsPlaying(tv) != 1 || !TV::ReadCurrent(tv, &name, &link, &isUrl) || !isUrl) continue;
        const std::wstring& address = RS::Sendable(name) ? name : link;
        const std::wstring key = ue_wrap::prop::GetInteractableKeyString(tv);
        if (!RS::Sendable(address) || key.empty() || key == L"None") continue;
        const uint32_t posMs = PosMsOf(TV::PlayTime(tv));
        Send(s, key, N::kTvOpPlay, address, posMs, slot);
        UE_LOGI("tv_link: key='%ls' '%ls' at %.1fs -> joiner slot %d", key.c_str(), address.c_str(),
                static_cast<double>(posMs) / 1000.0, slot);
    }
}

uint64_t Opened() { return g_opened; }
uint64_t Controls() { return g_controls; }

void OnDisconnect() {
    g_parked.clear();
    g_catchUp.clear();
    g_saidRuleOff = false;
    g_lastSendMs = 0;
    g_lastControlMs = 0;
    g_nextStepMs = 0;
}

}  // namespace coop::tv_link_sync
