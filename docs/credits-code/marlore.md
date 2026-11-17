# Marlore

One contributor's code, from the [credit ledger](../credits.md#the-ledger).

- **The trash carry, as a player met it** ([#29](https://github.com/VOTV-MP/Multivoid/pull/29)):
  two commits written against a live session, naming nine things that went wrong. The patch
  could not be merged as written (it sits on the history withdrawn in September, and each hunk
  was measured against the tree before anything was taken), so every symptom was re-run from
  our own chair with a drill, and what reproduced was fixed where it starts. Four did, and the
  commits carry their name:
  - **A refused grab was never answered.** The client's request stayed pending, so the next
    grab of that pile by anybody else read as its own confirmation and its use presses became
    throws of a clump it did not hold. The host now answers every refusal, to the one who
    asked, by request number.
  - **A pile looked different on every peer.** The pull request pointed at the pile's rotation;
    the census that followed measured 865 of 871 piles turned differently on the two machines.
    The game re-rolls the visible mesh's turn and size on every construction and saves neither,
    so the host's look now travels with the pile.
  - **A thrower refused every later grab.** Their fix ended the hold at the throw; that half is
    what shipped. A hold used to last until the clump landed as a pile, and a clump that comes
    to rest on a box never does.
  - **A player who fell kept carrying.** The game drops what a fainting player holds, but a
    client's clump is in its puppet's hand on the host, where that drop cannot reach.
  Beside those: a trash mirror whose GC pin fails is still kept as one we made (the
  pull request destroyed it; the defect under that was a lost record), and the drill that watches
  a host-thrown clump from the client exists because their report asked the question.
  One did not reproduce on the current tree on either path (a thrown clump frozen in the air),
  and three turned out to be the tree already doing the right thing; the pull request thread
  says which and why.
