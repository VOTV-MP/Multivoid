# AwazonDotCom

One contributor's code, from the [credit ledger](../credits.md#the-ledger).

- **A floppy row's line breaks** ([#53](https://github.com/VOTV-MP/Multivoid/pull/53)): the floppy slot,
  floppy box and laptop buffer lanes encoded their strings with the chat encoder, which drops control
  characters, so the host's several-line server-data row reached a client on one line and the game could
  not find its type. The commit, authored as DexxInTime, is kept as written; the text codec now owns the
  three lanes' strings both ways, and the laptop and floppy drills carry rows with line breaks to prove it.
- **A drive rack with one author** (the same request): a client's drive rack was written by the rack lane and
  by the save-record lane, whose record the rack lane would read as the player's takes and sets. The commit
  claims the rack from the save-record lane and holds a joiner's rack canonicals through its join; its
  several-rows guard was taken out, and the host now sends a rack it first sees mid-session, with a drill
  that spawns a rack holding rows to prove it.
