# hediiiqq

One contributor's code, from the [credit ledger](../credits.md#the-ledger).

- Dish mirror interpolation: the 4 Hz dish pose stream now glides through a
  proper lerp window instead of snapping every 250 ms.
- **A CI gate was failing every build**
  ([#10](https://github.com/VOTV-MP/Multivoid/issues/10)): it called ten MTA
  citations dead when the only thing wrong was that `reference/mtasa-blue` is a
  submodule the workflow deliberately never fetches. The report did the whole
  diagnosis -- it located the asymmetry (check B already skips an absent corpus
  and says so; check A did not), quoted the code's own reasoning back at it,
  named why the timing hid it (the gate landed 2026-08-29, the last green build
  was 2026-07-31), and argued AGAINST the easy allowlist fix because it would
  permanently stop checking those line numbers for anyone running the gate
  locally with submodules populated. Fixed exactly as suggested.
