// coop/net/fake_link_steps.cpp -- see coop/net/fake_link_steps.h.

#include "coop/net/fake_link_steps.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"

#include "ue_wrap/core/log.h"

#pragma warning(push)
#pragma warning(disable: 4100 4127 4191 4244 4245 4267 4310 4324 4458)
#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>
#pragma warning(pop)

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <string_view>

namespace coop::net::fake_link_steps {

namespace {

// ---- The parser ----

std::string_view Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

// A whole-string, non-negative decimal within [0, hi].
bool ParseUint(std::string_view s, int64_t hi, int64_t& out) {
    s = Trim(s);
    if (s.empty()) return false;
    int64_t v = 0;
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || end != s.data() + s.size() || v < 0 || v > hi) return false;
    out = v;
    return true;
}

// Sets `field` from `value` unless the step already carries the key.
bool SetKey(int32_t& field, std::string_view value, int64_t hi) {
    int64_t v = 0;
    if (field >= 0 || !ParseUint(value, hi, v)) return false;
    field = static_cast<int32_t>(v);
    return true;
}

bool ParseStep(std::string_view seg, Step& st) {
    const size_t plus = seg.find('+');
    const size_t colon = seg.find(':');
    if (plus == std::string_view::npos || colon == std::string_view::npos || plus > colon)
        return false;
    const std::string_view origin = Trim(seg.substr(0, plus));
    if (origin == "begin") st.origin = Origin::Begin;
    else if (origin == "wr") st.origin = Origin::WorldReady;
    else return false;
    int64_t sec = 0;
    if (!ParseUint(seg.substr(plus + 1, colon - plus - 1), kMaxAfterSec, sec)) return false;
    st.afterSec = static_cast<uint32_t>(sec);

    std::string_view keys = seg.substr(colon + 1);
    while (true) {
        const size_t amp = keys.find('&');
        const std::string_view pair = keys.substr(0, amp);
        const size_t eq = pair.find('=');
        if (eq == std::string_view::npos) return false;
        const std::string_view key = Trim(pair.substr(0, eq));
        const std::string_view value = pair.substr(eq + 1);
        bool ok = false;
        if (key == "lag") ok = SetKey(st.lagMs, value, kLagMaxMs);
        else if (key == "jitter") ok = SetKey(st.jitterMs, value, kJitterMaxMs);
        else if (key == "loss_send") ok = SetKey(st.lossSendPct, value, kPctMax);
        else if (key == "loss_recv") ok = SetKey(st.lossRecvPct, value, kPctMax);
        else if (key == "kbs") ok = SetKey(st.kbs, value, kKbsMax);
        if (!ok) return false;
        if (amp == std::string_view::npos) break;
        keys.remove_prefix(amp + 1);
    }
    return true;
}

// ---- The armed schedule ----

Schedule g_schedule;
bool g_applied[kMaxSteps];
std::atomic<bool> g_pending{false};
std::atomic<int64_t> g_originMs[kOriginCount];

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

const char* OriginName(Origin o) { return o == Origin::Begin ? "begin" : "wr"; }

}  // namespace

bool ParseSteps(const std::string& text, Schedule& out, std::string& badStep) {
    out = Schedule{};
    badStep.clear();
    if (Trim(text).empty()) return true;
    std::string_view rest = text;
    while (true) {
        const size_t semi = rest.find(';');
        const std::string_view seg = rest.substr(0, semi);
        Step st;
        if (out.count >= kMaxSteps || !ParseStep(seg, st)) {
            badStep.assign(seg);
            out = Schedule{};
            return false;
        }
        out.steps[out.count++] = st;
        if (semi == std::string_view::npos) break;
        rest.remove_prefix(semi + 1);
    }
    return true;
}

// ---- The global writers ----

void WriteLagMs(int ms) {
    if (auto* utils = SteamNetworkingUtils())
        utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_FakePacketLag_Send,
                                         static_cast<int32>(ms));
}

void WriteLossSendPct(int pct) {
    if (auto* utils = SteamNetworkingUtils())
        utils->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketLoss_Send,
                                         static_cast<float>(pct));
}

void WriteLossRecvPct(int pct) {
    if (auto* utils = SteamNetworkingUtils())
        utils->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketLoss_Recv,
                                         static_cast<float>(pct));
}

void WriteJitterMs(int ms) {
    auto* utils = SteamNetworkingUtils();
    if (!utils) return;
    // The transport draws an exponential jitter around the average, capped by Max (default 100 ms)
    // and applied to Pct of the packets (default 75): the cap goes to the transport's own ceiling
    // and every packet is eligible, so the knob is the mean and nothing else.
    utils->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketJitter_Send_Avg,
                                     static_cast<float>(ms));
    utils->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketJitter_Send_Max, 5000.0f);
    utils->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketJitter_Send_Pct,
                                     ms > 0 ? 100.0f : 0.0f);
}

void WriteLinkKbs(int kbs) {
    if (auto* utils = SteamNetworkingUtils())
        utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_FakeRateLimit_Send_Rate,
                                         static_cast<int32>(kbs) * 1024);
}

// ---- The runner ----

namespace {

// Every fake-link quantity to its CONFIGURED value, 0 where its row is unset. The globals live in
// the transport for the whole process, so a step applied in a previous session would otherwise
// open this one (a step that never came due, a restoring step cut off by a stop), and an edited
// row would never be read after the first start.
void ApplyRows() {
    const long kbs = coop::config::ResolveInt(coop::config_registry::rows::net_fakelink_kbs);
    WriteLinkKbs(static_cast<int>(kbs));
    if (kbs > 0)
        UE_LOGW("net: FAKE LINK LIMIT %ld KB/s outbound (drill knob net.fakelink_kbs) -- "
                "packets beyond the budget are silently dropped", kbs);
    const long lagMs = coop::config::ResolveInt(coop::config_registry::rows::net_fakelag_ms);
    WriteLagMs(static_cast<int>(lagMs));
    if (lagMs > 0)
        UE_LOGW("net: FAKE LAG %ld ms on every outbound packet (drill knob net.fakelag_ms)",
                lagMs);
    const long lossSend =
        coop::config::ResolveInt(coop::config_registry::rows::net_fakeloss_send_pct);
    WriteLossSendPct(static_cast<int>(lossSend));
    if (lossSend > 0)
        UE_LOGW("net: FAKE LOSS %ld%% of outbound packets discarded (drill knob "
                "net.fakeloss_send_pct)", lossSend);
    const long lossRecv =
        coop::config::ResolveInt(coop::config_registry::rows::net_fakeloss_recv_pct);
    WriteLossRecvPct(static_cast<int>(lossRecv));
    if (lossRecv > 0)
        UE_LOGW("net: FAKE LOSS %ld%% of received packets discarded (drill knob "
                "net.fakeloss_recv_pct)", lossRecv);
    const long jitterMs = coop::config::ResolveInt(coop::config_registry::rows::net_fakejitter_ms);
    WriteJitterMs(static_cast<int>(jitterMs));
    if (jitterMs > 0)
        UE_LOGW("net: FAKE JITTER mean %ld ms on every outbound packet (drill knob "
                "net.fakejitter_ms)", jitterMs);
}

}  // namespace

void Arm() {
    g_pending.store(false, std::memory_order_release);
    ApplyRows();
    for (auto& o : g_originMs) o.store(0, std::memory_order_relaxed);
    for (bool& a : g_applied) a = false;
    g_schedule = Schedule{};
    const std::string text =
        coop::config::ResolveString(coop::config_registry::rows::net_fakelink_steps);
    std::string bad;
    Schedule parsed;
    if (!ParseSteps(text, parsed, bad)) {
        UE_LOGW("fake_link: net.fakelink_steps ignored whole -- step \"%s\" does not parse "
                "(<begin|wr>+<sec>:<lag|jitter|loss_send|loss_recv|kbs>=<n>[&...]; ...)",
                bad.c_str());
        return;
    }
    if (parsed.count == 0) return;
    g_schedule = parsed;
    UE_LOGI("fake_link: %d step(s) armed from net.fakelink_steps (this peer's own origins)",
            parsed.count);
    g_pending.store(true, std::memory_order_release);
}

void NoteOrigin(Origin origin) {
    if (!g_pending.load(std::memory_order_relaxed)) return;
    int64_t unset = 0;
    g_originMs[static_cast<int>(origin)].compare_exchange_strong(unset, NowMs(),
                                                                 std::memory_order_relaxed);
}

void Tick() {
    if (!g_pending.load(std::memory_order_acquire)) return;
    // No clock until an origin exists: an armed schedule whose origin never comes (a host with no
    // joiner) costs two loads a tick, not a clock read.
    bool anyOrigin = false;
    for (const auto& o : g_originMs) anyOrigin = anyOrigin || o.load(std::memory_order_relaxed) != 0;
    if (!anyOrigin) return;
    const int64_t now = NowMs();
    int waiting = 0;
    for (int i = 0; i < g_schedule.count; ++i) {
        if (g_applied[i]) continue;
        const Step& st = g_schedule.steps[i];
        const int64_t origin =
            g_originMs[static_cast<int>(st.origin)].load(std::memory_order_relaxed);
        if (origin == 0 || now - origin < static_cast<int64_t>(st.afterSec) * 1000) {
            ++waiting;
            continue;
        }
        g_applied[i] = true;
        char keys[160];
        int n = 0;
        auto add = [&](const char* name, int32_t v) {
            if (v < 0) return;
            n += snprintf(keys + n, sizeof(keys) - static_cast<size_t>(n), "%s%s=%d",
                          n ? "&" : "", name, static_cast<int>(v));
        };
        add("lag", st.lagMs);
        add("jitter", st.jitterMs);
        add("loss_send", st.lossSendPct);
        add("loss_recv", st.lossRecvPct);
        add("kbs", st.kbs);
        if (st.lagMs >= 0) WriteLagMs(st.lagMs);
        if (st.jitterMs >= 0) WriteJitterMs(st.jitterMs);
        if (st.lossSendPct >= 0) WriteLossSendPct(st.lossSendPct);
        if (st.lossRecvPct >= 0) WriteLossRecvPct(st.lossRecvPct);
        if (st.kbs >= 0) WriteLinkKbs(st.kbs);
        UE_LOGW("fake_link: step %s+%u applied: %s", OriginName(st.origin), st.afterSec, keys);
    }
    if (waiting == 0) g_pending.store(false, std::memory_order_release);
}

// ---- The selftest ----

bool RunSelftest() {
    int pass = 0, total = 0;
    auto check = [&](bool ok, const char* what) {
        ++total;
        if (ok) { ++pass; return; }
        UE_LOGE("fake_link_steps selftest FAIL: %s", what);
    };
    Schedule s;
    std::string bad;
    check(ParseSteps("", s, bad) && s.count == 0, "an empty schedule is valid and empty");
    check(ParseSteps("begin+5:lag=100", s, bad) && s.count == 1 &&
              s.steps[0].origin == Origin::Begin && s.steps[0].afterSec == 5 &&
              s.steps[0].lagMs == 100 && s.steps[0].jitterMs == -1 && s.steps[0].kbs == -1,
          "one begin step, one key; the absent keys stay absent");
    check(ParseSteps("wr+0:lag=0", s, bad) && s.count == 1 &&
              s.steps[0].origin == Origin::WorldReady && s.steps[0].lagMs == 0,
          "the wr origin, and a 0 value is a value (it turns the knob off)");
    check(ParseSteps("begin+2:lag=80&jitter=20&loss_send=5&loss_recv=3&kbs=256", s, bad) &&
              s.count == 1 && s.steps[0].lagMs == 80 && s.steps[0].jitterMs == 20 &&
              s.steps[0].lossSendPct == 5 && s.steps[0].lossRecvPct == 3 &&
              s.steps[0].kbs == 256,
          "all five keys in one step");
    check(ParseSteps("begin+3:lag=100; wr+10:lag=0&loss_send=0", s, bad) && s.count == 2 &&
              s.steps[0].origin == Origin::Begin && s.steps[1].origin == Origin::WorldReady &&
              s.steps[1].afterSec == 10 && s.steps[1].lossSendPct == 0,
          "two steps on both origins, with a space after the separator");
    check(!ParseSteps("begin+3:lag=100;now+4:lag=1", s, bad) && bad == "now+4:lag=1" &&
              s.count == 0,
          "a bad origin in the second step names that step and empties the schedule");
    check(!ParseSteps("begin3:lag=1", s, bad), "no '+' is malformed");
    check(!ParseSteps("begin+3", s, bad), "no ':' is malformed");
    check(!ParseSteps("begin+3:", s, bad), "a step with no key is malformed");
    check(!ParseSteps("begin+3:lag", s, bad), "a key with no '=' is malformed");
    check(!ParseSteps("begin+3:lag=", s, bad), "a key with no value is malformed");
    check(!ParseSteps("begin+3:lag=x", s, bad), "a non-numeric value is malformed");
    check(!ParseSteps("begin+3:lag=-1", s, bad), "a negative value is malformed");
    check(!ParseSteps("begin+3:lag=2001", s, bad), "a value past the row's range is malformed");
    check(!ParseSteps("begin+3:loss_send=101", s, bad), "a percentage past 100 is malformed");
    check(!ParseSteps("begin+3:speed=1", s, bad), "an unknown key is malformed");
    check(!ParseSteps("begin+3:lag=1&lag=2", s, bad), "a key twice in one step is malformed");
    check(!ParseSteps("begin+3:lag=1,loss_send=2", s, bad),
          "a comma is not the key separator (the rig's --config splits on it)");
    check(!ParseSteps("begin+3:lag=1&", s, bad), "a trailing key separator is an empty key");
    check(!ParseSteps("begin+x:lag=1", s, bad), "a non-numeric time is malformed");
    check(!ParseSteps("begin+3:lag=1;", s, bad), "a trailing separator is an empty step");
    {
        std::string many;
        for (int i = 0; i <= kMaxSteps; ++i) many += (i ? ";wr+1:lag=1" : "wr+1:lag=1");
        check(!ParseSteps(many, s, bad), "more than kMaxSteps steps is malformed");
    }
    const bool ok = (pass == total);
    if (ok) UE_LOGI("fake_link_steps selftest: PASS (%d/%d cases)", pass, total);
    else    UE_LOGE("fake_link_steps selftest: FAIL (%d/%d cases)", pass, total);
    return ok;
}

}  // namespace coop::net::fake_link_steps
