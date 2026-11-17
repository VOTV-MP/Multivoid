# gust138

One contributor's code, from the [credit ledger](../credits.md#the-ledger).

- **A box's last change** ([#64](https://github.com/VOTV-MP/Multivoid/pull/64)): the record lane published a
  drive box or reel case at most once per 300 ms and dropped the rest, so a second change soon after the first
  never reached the other peers. Every change now publishes. The commits are kept as written; a two-peer drill
  opens a box and takes its drive about 380 ms apart, and its red, with a publish window restored, ends the far
  box with the drive still in.
- **A coin picked up by a client** (the same request): a client's E press credited the coin and destroyed it on
  that client before the host answered. The press now forwards and is cancelled, and the host credits it. The
  drill counts no local credit on the client and the same balance on both peers; its red counts one.
- **A balance nobody took** (the same request): the host now resends its balance until a peer accepts it.

---
