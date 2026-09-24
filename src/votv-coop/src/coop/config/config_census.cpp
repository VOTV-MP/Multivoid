// coop/config/config_census.cpp -- what this launch is actually configured with, said out loud.
//
// A drill that varies a setting and never checks the setting took effect is not an experiment: a
// send-rate run measured a controller its own log said was off, and every conclusion drawn from
// it described a binary that never ran the code under test. So a peer publishes the rows a layer
// actually supplied -- the value as RESOLVED, and which layer won -- and a rig asserts its own
// independent variables against that before it measures anything.
//
// The two lines are a contract with tools outside the tree, like coop/session/rig_ready.h's:
//   config: EFFECTIVE <key>=<value> (<env|ini>)
//   config: EFFECTIVE end -- <n> row(s) configured
// An absent row means the peer took that row's default, which holds only if the ini was readable,
// so the end line carries that verdict too. A credential row -- named by the registry, never
// guessed from a spelling -- prints as <set>, and a refused raw value says so beside the default
// it fell back to, or beside itself on a fail-closed row, where nothing falls back.

#include "coop/config/config.h"

#include "config_internal.h"
#include "coop/config/config_registry.h"
#include "ue_wrap/core/log.h"

#include <cstdio>
#include <string>

namespace coop::config {
namespace {

using config_registry::Kind;
using config_registry::Row;

// A credential, or the minted identity, is reported as PRESENT and never quoted: this log is
// pasted into bug reports and captured by CI, and a lobby password in it outlives the session.
// Which keys those are is the registry's fact, not this file's guess at a spelling.
bool Redacted(const Row& r) {
    return r.kind == Kind::Identity || config_registry::IsCredentialKey(r.key);
}

// The resolved value in the one spelling a reader and a rig both compare: a flag is 1 or 0, an
// integer its own digits, a float the catalog's C-locale emission, an enum its canonical token.
// Every kind goes through the same *FromRaw core the live Resolve functions use, so this is
// exactly what the RESOLVER returns -- before a consumer's own clamp. Two consumers clamp further
// and the census does not follow them there: ReadNickname caps length and repertoire, and
// ReadPlayerSkin replaces an unknown skin with a random starter.
std::string Resolved(const Row& r, const std::string& raw) {
    char buf[64];
    switch (r.kind) {
        case Kind::Flag:
            return internal::FlagFromRaw(&r, true, raw) ? "1" : "0";
        case Kind::Int:
            std::snprintf(buf, sizeof(buf), "%ld", internal::IntFromRaw(&r, true, raw));
            return buf;
        case Kind::Float:
            return internal::FormatFloat(internal::FloatFromRaw(&r, true, raw));
        case Kind::Enum:
            return internal::EnumFromRaw(&r, true, raw);
        case Kind::String:
        case Kind::Identity:
            break;
    }
    return raw;  // free strings are unvalidated, and an identity never reaches here
}

// A refused value printed as itself, which is whatever a file or an environment held: printable
// ASCII only and capped, so the line stays one line a rig's pattern can read.
std::string Printable(const std::string& raw) {
    constexpr size_t kMax = 64;
    std::string s;
    for (const char c : raw) {
        if (s.size() == kMax) { s += "..."; break; }
        s += (c >= 0x20 && c < 0x7f) ? c : '?';
    }
    return s;
}

}  // namespace

void ReportEffectiveConfig() {
    size_t count = 0;
    const Row* rows = config_registry::Rows(count);
    // The redaction list names rows by key, so a row renamed without it would start printing a
    // password. One pass at boot turns that into a line somebody reads.
    size_t credCount = 0;
    const char* const* creds = config_registry::CredentialKeys(credCount);
    for (size_t i = 0; i < credCount; ++i)
        if (!config_registry::FindRow(creds[i]))
            UE_LOGE("config: the redaction list names '%s', which is not a registry row -- a "
                    "renamed credential row will have its value logged in full", creds[i]);
    int configured = 0;
    for (size_t i = 0; i < count; ++i) {
        const Row& r = rows[i];
        std::string raw;
        bool fromEnv = false;
        // Nobody configured it: the row default stands, and saying so for every untouched row
        // would bury the handful that were.
        if (!internal::PickRawLayered(&r, raw, &fromEnv)) continue;
        ++configured;
        // A raw value the reader refuses leaves the row at its default. That is the failure a rig
        // has to see stated rather than infer from a value that merely looks wrong, and it is the
        // same validator the writer and the boot sweep use, so there is one verdict per value.
        std::string why;
        const bool valid = ValueValidForKey(r.key, raw, &why);
        const bool refused = !valid && r.failClosed;
        const std::string value = Redacted(r) ? "<set>" : refused ? Printable(raw) : Resolved(r, raw);
        if (valid)
            UE_LOGI("config: EFFECTIVE %s=%s (%s)", r.key, value.c_str(),
                    fromEnv ? "env" : "ini");
        else
            UE_LOGW("config: EFFECTIVE %s=%s (%s, rejected: %s%s)", r.key, value.c_str(),
                    fromEnv ? "env" : "ini", why.c_str(),
                    refused ? "; fail-closed, so what it governs is refused" : "");
    }
    // The end line carries the ini's verdict, because an absent row is only evidence of a default
    // when the file was READABLE. A locked or failing ini reads as absent for every key, so a
    // census that said nothing about it would report `0 row(s) configured` and a drill would take
    // that as proof the peer took every default. It is the one way this instrument can be
    // confidently wrong, so it is stated on the line a reader greps.
    if (IniUnreadableSeen())
        UE_LOGW("config: EFFECTIVE end -- %d row(s) configured, but the ini unreadable this "
                "launch: an absent row below proves nothing, only env was readable", configured);
    else
        UE_LOGI("config: EFFECTIVE end -- %d row(s) configured", configured);
}

}  // namespace coop::config
