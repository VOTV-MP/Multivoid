// coop/config/config_registry.cpp -- the declarative per-key row table.
//
// See config_registry.h. Row order within a section is the order the T1b
// reformat emits; the SECTION order is kSectionOrder. Completeness vs the
// call-site census is proven by the enum-completeness instrument (design §5).

#include "coop/config/config_registry.h"

#include <cstring>
#include <string>

namespace coop::config_registry {
namespace {

constexpr double kNoRange = 0.0;

// Shorthand: most rows are flags/strings with no range, no tokens, no env.
constexpr Row F(const char* key, const char* sec, const char* env = nullptr) {
    return Row{key, sec, Kind::Flag, kNoRange, kNoRange, nullptr, env, false};
}
constexpr Row S(const char* key, const char* sec, const char* env = nullptr) {
    return Row{key, sec, Kind::String, kNoRange, kNoRange, nullptr, env, false};
}
constexpr Row I(const char* key, const char* sec, double lo, double hi,
                const char* env = nullptr) {
    return Row{key, sec, Kind::Int, lo, hi, nullptr, env, false};
}
constexpr Row Fl(const char* key, const char* sec, double lo, double hi,
                 const char* env = nullptr) {
    return Row{key, sec, Kind::Float, lo, hi, nullptr, env, false};
}
constexpr Row E(const char* key, const char* sec, const char* tokens,
                const char* env = nullptr) {
    return Row{key, sec, Kind::Enum, kNoRange, kNoRange, tokens, env, false};
}

constexpr Row kRows[] = {
    // ---- [net] --------------------------------------------------------------
    // net.nick: the ONE seeded-active row (user-ruled; the skeleton writes
    // net.nick=<kMyNameDefault> as a visible, editable line).
    Row{"net.nick", "net", Kind::String, kNoRange, kNoRange, nullptr,
        "VOTVCOOP_NET_NICK", true},
    E("net.role", "net", "host|client", "VOTVCOOP_NET_ROLE"),
    S("net.peer", "net", "VOTVCOOP_NET_PEER"),
    I("net.port", "net", 1, 65535, "VOTVCOOP_NET_PORT"),
    E("net.topology", "net", "lan|p2p", "VOTVCOOP_NET_TOPOLOGY"),
    S("net.master", "net", "VOTVCOOP_MASTER_URL"),
    F("net.master.custom", "net"),
    S("net.signaling", "net", "VOTVCOOP_NET_SIGNALING"),
    S("net.signaling_token", "net", "VOTVCOOP_NET_SIGNALING_TOKEN"),
    S("net.identity", "net", "VOTVCOOP_NET_IDENTITY"),
    S("net.host_identity", "net", "VOTVCOOP_NET_HOST_IDENTITY"),
    S("net.stun", "net", "VOTVCOOP_NET_STUN"),
    S("net.turn", "net", "VOTVCOOP_NET_TURN"),
    S("net.turn_user", "net", "VOTVCOOP_NET_TURN_USER"),
    S("net.turn_pass", "net", "VOTVCOOP_NET_TURN_PASS"),
    E("net.ice", "net", "all|relay|disable|default", "VOTVCOOP_NET_ICE"),

    // ---- [player] -----------------------------------------------------------
    Row{"player_guid", "player", Kind::Identity, kNoRange, kNoRange, nullptr, nullptr, false},
    Row{"player_skin", "player", Kind::Identity, kNoRange, kNoRange, nullptr, nullptr, false},
    F("nameplate", "player"),
    S("nick_color", "player"),

    // ---- [ui] ---------------------------------------------------------------
    // ui.scale range = ui/scale.cpp's user-pref clamp [kUserMin, kUserMax];
    // scale.cpp consumes THIS row's range (single source since arc 2).
    Fl("ui.scale", "ui", 0.75, 1.75),
    F("ui.netstats", "ui"),
    F("ui.chat.peer_actions", "ui"),
    S("browser.lastdirect", "ui"),
    F("multiplayer_menu_off", "ui"),

    // ---- [voice] ------------------------------------------------------------
    F("voice.enabled", "voice", "VOTVCOOP_VOICE_ENABLED"),
    E("voice.mode", "voice", "ptt|activation", "VOTVCOOP_VOICE_MODE"),
    S("voice.ptt_key", "voice"),
    S("voice.whisper_key", "voice"),
    S("voice.mute_key", "voice"),
    Fl("voice.threshold_db", "voice", -120, 20, "VOTVCOOP_VOICE_THRESHOLD_DB"),
    Fl("voice.mic_gain_db", "voice", -60, 60),
    S("voice.mic_device", "voice"),
    S("voice.output_device", "voice"),
    Fl("voice.volume", "voice", 0, 10),
    Fl("voice.distance_cm", "voice", 100, 1000000),
    I("voice.jitter_threshold", "voice", 0, 60),
    I("voice.prebuffer_frames", "voice", 0, 60),
    F("voice.test_tone", "voice", "VOTVCOOP_VOICE_TEST_TONE"),
    F("voice.loopback", "voice", "VOTVCOOP_VOICE_LOOPBACK"),

    // ---- [dev] --------------------------------------------------------------
    F("enabled", "dev"),   // master kill-switch (absent = granular switches decide)
    F("devkeys", "dev"),
    S("save", "dev", "VOTVCOOP_SAVE"),
    F("fresh_boot", "dev", "VOTVCOOP_FRESH"),
    F("static_2nd_player", "dev"),
    F("freecam", "dev"),
    F("spawn_menu_unlock", "dev"),
    F("desk_diag", "dev"),
    I("desk_diag_ms", "dev", 100, 60000),
    I("vitals_keepalive_sec", "dev", 0, 86400),
    F("window_log", "dev"),
    F("window_synth", "dev"),
    F("weather_observer_log", "dev"),
    F("weather_probe", "dev"),
    F("meadow_selftest", "dev"),
    F("laptop_selftest", "dev"),
    F("deck_selftest", "dev"),
    F("drive_selftest", "dev"),
    F("desk_snd_selftest", "dev"),
    F("container_selftest", "dev"),
    F("inventory_selftest", "dev"),
    F("starterkit_test", "dev"),
    F("reseed_orphan_selftest", "dev"),
    F("pile_delta_probe", "dev"),
    F("garbage_pickup_probe", "dev"),
    F("grime_log", "dev"),
    F("interactable_log", "dev"),
    F("client_model_probe", "dev"),
    F("delivery_census", "dev"),
    F("flashlight_log", "dev"),
    F("kerfur_census", "dev"),
    F("object_overlay", "dev"),
    F("ragdoll_bone_overlay", "dev"),
    F("join_window_pos_trace", "dev"),
    F("puppet_head_probe", "dev"),
    F("native_pile_inert_probe", "dev"),
    F("inventory_probe", "dev"),
    F("spawn_order_probe", "dev"),
    F("eid_lifetime_trace", "dev"),
    F("pinecone_probe", "dev"),
    F("live_store_readout", "dev"),
    F("heap_probe", "dev"),
    F("drone_probe", "dev"),
    F("drone_probe_drive", "dev"),
    F("pile_dup_probe", "dev"),
    F("perf_probe", "dev"),
    F("perf_probe_selftime", "dev"),
    F("gnatives_probe", "dev"),
    F("gnatives_probe_disabled", "dev"),
    F("lightswitch_probe", "dev"),
    F("sleep_probe", "dev"),
    F("vm_dispatch_log", "dev"),
    F("door_probe", "dev"),
    F("rng_roll_census", "dev"),
    F("leak_probe", "dev"),
    F("force_chippile_unclaim", "dev"),
    F("disable_completeness_floor", "dev"),
    F("keypad_probe", "dev"),
    F("keypad_synth", "dev"),
    F("save_identity_map_log", "dev"),
    F("save_identity_bind", "dev"),
    F("force_kerfur_unmap", "dev"),
    F("force_save_churn", "dev"),
};
constexpr size_t kRowCount = sizeof(kRows) / sizeof(kRows[0]);

}  // namespace

const Row* Rows(size_t& count) {
    count = kRowCount;
    return kRows;
}

const Row* FindRow(const char* key) {
    if (!key || !*key) return nullptr;
    for (const Row& r : kRows)
        if (_stricmp(r.key, key) == 0) return &r;
    return nullptr;
}

bool IsKnownKey(const char* key) {
    if (FindRow(key)) return true;
    // Composed family: ui.font.<role> for a registered role (ci, like all key
    // matching under the unified rule). The dead bare "ui.font" (F28) and an
    // unknown role both fall through -> unknown.
    constexpr const char* kPrefix = "ui.font.";
    constexpr size_t kPrefixLen = 8;
    if (_strnicmp(key, kPrefix, kPrefixLen) == 0) {
        for (size_t i = 0; i < kFontRoleCount; ++i)
            if (_stricmp(key + kPrefixLen, kFontRoleKeys[i]) == 0) return true;
    }
    return false;
}

}  // namespace coop::config_registry
