// coop/net/json_util.h -- exception-free nlohmann accessors for the master plane.
//
// INTERNAL to the lobby plane: include ONLY from .cpp files that parse master
// responses (lobby_client.cpp, lobby_announcer.cpp). It pulls in the full
// nlohmann/json.hpp (large) -- keeping it out of headers keeps that compile cost
// localized to those two TUs.
//
// The accessors never throw: they find the key, verify the JSON type, and only then
// extract -- a missing or wrong-typed field yields the default. (A network peer can
// send anything; defensive parsing is correct here, not a crutch -- RULE 1.)

#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace coop::net::jsonu {

using Json = nlohmann::json;

inline std::string Str(const Json& j, const char* key) {
    auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

inline int Int(const Json& j, const char* key) {
    auto it = j.find(key);
    return (it != j.end() && it->is_number_integer()) ? it->get<int>() : 0;
}

inline bool Bool(const Json& j, const char* key) {
    auto it = j.find(key);
    return (it != j.end() && it->is_boolean()) ? it->get<bool>() : false;
}

// Length-capped string (audit L5): a hostile/MITM master must not hand back a multi-MB
// `name` the browser re-renders every frame, nor an oversized identity/token flowing
// into GNS config. Truncates to maxlen bytes.
inline std::string StrN(const Json& j, const char* key, size_t maxlen) {
    std::string s = Str(j, key);
    if (s.size() > maxlen) s.resize(maxlen);
    return s;
}

// Clamped integer (audit L4): a hostile master's out-of-range count/age must not narrow
// to garbage/negative in the UI.
inline int IntClamped(const Json& j, const char* key, int lo, int hi) {
    int v = Int(j, key);
    return v < lo ? lo : (v > hi ? hi : v);
}

// A credential field safe to splice into GNS's comma-separated TURN lists (audit L6):
// reject a value containing a comma or whitespace, which would inject an extra list
// element and desync the parallel user/pass/uri lists. Returns "" if unsafe/oversized.
inline std::string CredField(const Json& j, const char* key, size_t maxlen) {
    std::string s = StrN(j, key, maxlen);
    if (s.find(',') != std::string::npos ||
        s.find_first_of(" \t\r\n") != std::string::npos)
        return std::string();
    return s;
}

// Parse a body into an object. Returns false (and leaves out null) if the body is
// not valid JSON, not a top-level object, or nested past kMaxDepth.
//
// C-1 (security audit 2026-07-16): cap nesting depth AT PARSE. nlohmann's parser is
// iterative, but the resulting value's DESTRUCTOR recurses one frame per nesting
// level -- so a deeply-nested body (e.g. `{"a":{"a":{...}}}`, easily within the 1 MiB
// byte cap) would overflow the worker thread's stack when the local Json goes out of
// scope: an UNCATCHABLE SEH crash (0xC00000FD) that a hostile or MITM'd master (the
// plane is plain HTTP) could trigger at boot (/v1/latest) or on any browser refresh /
// join. The parse callback DISCARDS any value past the limit, so the built tree can
// never get deep enough to overflow on destruction, and we reject the whole body.
// Real master responses nest ~3 deep; 32 is generous headroom.
inline bool ParseObject(const std::string& body, Json& out) {
    constexpr int kMaxDepth = 32;
    bool tooDeep = false;
    auto cb = [&tooDeep](int depth, Json::parse_event_t, Json& /*parsed*/) -> bool {
        if (depth > kMaxDepth) {
            tooDeep = true;
            return false;  // prune -> the deep subtree is never built
        }
        return true;
    };
    out = Json::parse(body, cb, /*allow_exceptions=*/false);
    return !tooDeep && !out.is_discarded() && out.is_object();
}

// Serialize a body with the `replace` error handler: a string containing invalid
// UTF-8 (e.g. a user pasting odd bytes into the host-name box) yields replacement
// chars instead of THROWING json::type_error -- which, escaping a detached worker
// thread, would std::terminate the whole game process. (Audit.)
inline std::string Dump(const Json& j) {
    return j.dump(-1, ' ', false, Json::error_handler_t::replace);
}

// Pull the first TURN uri out of a master `turn` block's uris[] and strip the
// "?transport=..." query, so GNS's P2P_TURN_ServerList gets a bare "turn:host:port".
// Empty if there is no usable uri.
inline std::string FirstTurnUri(const Json& turn) {
    auto it = turn.find("uris");
    if (it == turn.end() || !it->is_array() || it->empty()) return std::string();
    const auto& first = (*it)[0];
    if (!first.is_string()) return std::string();
    std::string u = first.get<std::string>();
    const size_t q = u.find('?');
    if (q != std::string::npos) u = u.substr(0, q);
    if (u.size() > 256) return std::string();
    // Audit L6: a comma/whitespace in the uri would desync GNS's parallel TURN lists.
    if (u.find(',') != std::string::npos ||
        u.find_first_of(" \t\r\n") != std::string::npos)
        return std::string();
    return u;
}

}  // namespace coop::net::jsonu
