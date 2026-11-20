// coop/props/dingus_internal.h -- what the dingus lane's files share. Private to coop/props.
//
// dingus_sync.cpp: the install, the watch, the found set, the births and the client's hearing. dingus_judge.cpp: the host's
// judge of a client's report.

#pragma once

#include <cstdint>
#include <string>

namespace coop::net {
class Session;
struct WireKey;
}  // namespace coop::net

namespace coop::dingus_sync::detail {

coop::net::Session* SessionPtr();
bool IsHost();
bool IsClient();

// The found set (game thread). AddKey: Added when the key is new, Known when held, Full when the set holds kMaxFound keys (counted
// `full`, said once a session).
enum class Add : uint8_t { Added, Known, Full };
bool Has(const std::wstring& key);
Add AddKey(const std::wstring& key);

// HOST: the key said to every world-ready client, the finder included. False when it cannot go on the wire (counted and said) or
// the send found no peer.
bool Broadcast(const std::wstring& key);

// HOST: the key said to one slot alone, to a finder whose own report named a key the host already holds. Same refusals as Broadcast.
bool SayToSlot(int slot, const std::wstring& key);

// A copy marked found, counted and said when the marking fails (`markFail`, at the first and each power of two).
void MarkCopy(void* actor, const std::wstring& key, const char* why);

// HOST: the find of `actor`'s key, from a judged report: add, mark the host's own copy, say it to every client. A full set still
// marks the copy and says nothing.
void AcceptFound(int slot, const std::wstring& key, void* actor);

// The messages the dispatcher refused, for the judge's drill view.
uint32_t DroppedCount();

// ---- dingus_judge.cpp ----

// HOST: judge one client's report.
void JudgeReport(int senderSlot, const std::wstring& key);
void ClearJudge();

}  // namespace coop::dingus_sync::detail
