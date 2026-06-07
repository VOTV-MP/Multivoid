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

// Parse a body into an object. Returns false (and leaves out null) if the body is
// not valid JSON or not a top-level object.
inline bool ParseObject(const std::string& body, Json& out) {
    out = Json::parse(body, nullptr, /*allow_exceptions=*/false);
    return !out.is_discarded() && out.is_object();
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
    return u;
}

}  // namespace coop::net::jsonu
