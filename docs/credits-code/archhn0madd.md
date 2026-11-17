# archhn0madd

One contributor's code, from the [credit ledger](../credits.md#the-ledger).

- **Rejoin without a full relaunch** — the fix for SirWilliam's report below.
  After a quit-to-menu the dying world and its ragdolled `mainPlayer_C` stay in
  `GUObjectArray` until the GC purge, and both of the boot poll's "where are we?"
  reads answered from that dead world: the corpse read as *in gameplay*, so a join
  booted into a world that was never loaded, and the dying world's `untitled` name
  read as *already loading*, so the `open` was never issued. Both reads now go
  through `world_identity` — the module that exists precisely because a dying
  world's actors outlive it — under one owner, `SurveyBootWorld`. It also
  un-strands a host trying to re-host after a death-flee.
  Contributed on the fork [Multifoid](https://github.com/archhn0madd/Multifoid);
  adopted as `engine_save.cpp`'s `SurveyBootWorld` with authorship preserved.
