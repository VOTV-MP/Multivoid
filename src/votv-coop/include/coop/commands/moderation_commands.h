// coop/commands/moderation_commands.h -- the moderation verbs as commands: /kick, /ban, /banid,
// /unban, /tphere, /cremate.
//
// A domain's command bindings: this file registers the roots into a Registry and the handlers
// call the domain through the PORTS handed in, captured by value, so no file static holds them.
// The production registry is given the real ports (coop/moderation, command_sync); the selftest
// gives its own Registry fakes that record what they receive. The dispatcher has already decided
// who may run a command and has checked its qualifiers (offline target, an offline id the host
// has no record of, exempt target, who is told): a handler here never asks the permission system.
//
// Engine-free like the model files: it reaches coop/moderation only for the verbs' types, and
// coop/permissions for the action record.

#pragma once

#include "coop/commands/command_registry.h"
#include "coop/commands/command_targets.h"
#include "coop/moderation/cremate.h"
#include "coop/moderation/moderation.h"
#include "coop/permissions/action_log.h"

#include <string>
#include <string_view>
#include <vector>

namespace coop::commands::moderation {

namespace mod = coop::moderation;

struct Ports {
    mod::ModResult (*kick)(const mod::PlayerToken& token, const char* reason) = nullptr;
    mod::ModResult (*ban)(const mod::PlayerToken& token, const char* reason, bool byAddress) = nullptr;
    mod::ModResult (*banOffline)(const char* id, const char* reason, bool byAddress) = nullptr;
    mod::ModResult (*unban)(const char* playerId) = nullptr;
    mod::ModResult (*teleport)(const mod::PlayerToken& token) = nullptr;
    // Starts the trip to the crematorium; the later lines of the trip reach the order's seats through the port's own
    // reply function.
    mod::cremate::Result (*cremate)(const mod::cremate::Order& order) = nullptr;
    // A running hosted session: the verbs act only inside one.
    bool (*hosted)() = nullptr;
    // The banned ids that start with a prefix of an id.
    std::vector<std::string> (*idsWithPrefix)(std::string_view hexPrefix) = nullptr;
    // The nick a seen-players record holds for an id, to name an offline target in the reply;
    // empty when there is none or the record has no nick.
    std::string (*recordNick)(std::string_view id) = nullptr;
    // One reply line to a caller, now or later (a seated player, or the host's own feed).
    void (*notify)(const Caller& to, std::string_view line) = nullptr;
    // Records an admin action the verb carried out; null records nothing.
    void (*log)(const coop::permissions::Action& action) = nullptr;
};

// Registers /kick, /ban, /banid, /unban, /tphere, /cremate into `reg`, in that order. False (and the
// registry as it was after the last root that registered) when a root is refused.
bool Register(Registry& reg, const Ports& p);

}  // namespace coop::commands::moderation
