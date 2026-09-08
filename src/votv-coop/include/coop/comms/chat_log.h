// coop/comms/chat_log.h -- the LOBBY's chat record. Host-owned, wire-sourced only.
//
// This is not coop::chat_feed, and the difference is the whole reason the file exists. The feed
// is one peer's VIEW: it mixes what was said with that player's own UI notices, it obeys that
// peer's TTL and overflow, and it starts empty for whoever just arrived. Seeding a joiner from a
// peer's feed would hand them somebody else's notifications as the lobby's history, so the
// history had to be CREATED rather than snapshotted. This is it: an ordered record of every chat
// line the host has committed, each with the host-assigned lineSeq that IS the total order every
// peer sorts by. Only the host holds it; a client receives rows and renders them.
//
// Two histories, deliberately: the log is identical for everyone, the feed stays each peer's own
// view. So the claim is narrow and stated narrowly -- THE CHAT SUBSEQUENCE IS IDENTICAL ACROSS
// PEERS, the full interleaving is per-peer. HOST AND GAME THREAD ONLY: the authoring and the
// lineSeq assignment are one act, at one authority, on one thread, because the relay path that
// used to carry chat fires on the NET thread, before the game thread it would order against.

#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace coop::chat_log {

// One committed chat line.
struct Row {
    uint32_t    lineSeq = 0;   // host-monotone; the total order
    uint8_t     slot = 0;      // the speaker's peer slot AT THE TIME -- a world-entity
                               // handle (it drives the overhead bubble), NOT identity:
                               // by the time a joiner reads this row the slot may hold
                               // somebody else entirely
    uint32_t    nickArgb = 0;  // the speaker's CUSTOM colour then, 0 = they had none
    std::string nick;          // UTF-8, as it was when they said it
    std::string text;          // UTF-8, the message
};

// Commit a line and return its lineSeq. Host, game thread.
uint32_t Append(uint8_t slot, const std::string& nick, uint32_t nickArgb,
                const std::string& text);

// Walk the retained record oldest-first. Host, game thread.
void ForEach(const std::function<void(const Row&)>& fn);

// How many rows are held (0 on a client).
int Count();

// Drop the record. Called beside chat_feed::Reset() at BOTH of its sites -- session
// start and the leave funnel -- or a stop-and-re-host in one process would seed lobby
// B's joiner with lobby A's conversation.
void Reset();

}  // namespace coop::chat_log
