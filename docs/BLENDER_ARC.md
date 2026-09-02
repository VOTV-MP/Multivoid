# BLENDER_ARC — moved

VotvIO — the Blender addon that reads a Voices of the Void `.sav` and rebuilds the whole scene
from the game's own pak — **left this repository on 2026-09-02** and became its own project:

> **https://github.com/pelmentor/VotvIO**

It grew here first, at `tools/blender/votvio/`, across 28 commits between 2026-08-29 and
2026-08-30. All of that history went with it (`git subtree split`), and so did this document:
it is now `docs/ARC.md` in that repository, still the living log of every fix and the
measurement behind it.

Nothing in Multivoid depends on the addon, and the addon depends on nothing here — the two
projects only share the game they read. The lessons VotvIO paid for about cooked UE4 data stay
in this tree, in [LESSONS.md](LESSONS.md) §6, with their pointers updated to the new repo.

*This file is a signpost, kept because the old path was public. It carries no facts of its own.*
