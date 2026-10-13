// coop/comms/chat_rate.h -- how fast a client may speak, judged on the host: Source's chat flood guard as it ships.
// A line passes when 0.66 s have gone since its sender last talked and a two-tier token bucket allows it. Every
// attempt spends from the bucket, a line that passed starts the interval, and a refused line is dropped for good, with
// no answer (reference/source-sdk-2013/src/game/server/player.cpp:8771-8818, client.cpp:843-862). MTA's server has no
// chat rate, only a length cap (reference/mtasa-blue/Server/mods/deathmatch/Config.h:34) and a mute, both in its say
// (reference/mtasa-blue/Server/mods/deathmatch/logic/CConsoleCommands.cpp:356-369), so Source is the precedent. One
// state per slot, cleared with the occupant. The host's own lines are not counted: bounds apply to clients, where
// Source's listen host is a player like any other. Game thread.

#pragma once

#include <cstdint>

namespace coop::chat_rate {

// One slot's budget; a value-initialised one is Source's fresh player (player.cpp:613-616).
struct TalkBudget {
    uint64_t lastAttemptMs = 0;  // 0 = never: the first attempt's elapsed is the whole clock, a full tier 1
    uint64_t lastTalkMs = 0;     // 0 = never talked
    float    tier1 = 1.0f;
    float    tier2 = 10.0f;
};

// What one attempt meets: Interval when the 0.66 s refuses it (Source's first test), else Bucket when a tier does.
enum class TalkVerdict : uint8_t { Talk, Interval, Bucket };

// One attempt at `nowMs`, a steady clock's milliseconds, on `b`: Source's CanPlayerTalk, both tests evaluated, and on
// Talk the slot has talked now (NotePlayerTalked). Pure.
TalkVerdict Attempt(TalkBudget& b, uint64_t nowMs);

// Attempt on `slot`'s own budget. Host, game thread.
TalkVerdict AttemptLine(uint8_t slot, uint64_t nowMs);

// The arithmetic on local budgets, each row from a stated state, worked from Source's formula in float. Logs
// `chat_rate selftest: ALL PASS (N checks)` or one FAIL line per failing case.
bool RunChatRateSelftest();

}  // namespace coop::chat_rate
