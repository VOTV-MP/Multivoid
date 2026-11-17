# Tarangok

One contributor's code, from the [credit ledger](../credits.md#the-ledger).

- **KO respawn** (`death.ko_respawn`): the death lane — the config surface, the
  KO/respawn shape, and the first attempt at answering VOTV's kick-to-menu
  permadeath. The mechanism has been reworked twice since (the death section of
  `docs/players.md` is where it stands); the lane and the idea are theirs.
- **Live mannequin skin preview**: hovering a skin in the F1 menu shows it on a
  real in-world mannequin.
- Cross-peer held-prop visibility (clients now see props carried by other
  clients, not just the host's).
- Container extraction (a client-extracted item now reaches the host's world),
  and the author-side volume re-derive.
- Duplicate keyed props on a joining client (the double starting suitcase).
