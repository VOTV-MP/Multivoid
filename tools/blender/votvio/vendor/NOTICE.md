# Vendored third-party code

## UE4Parse (pyUE4Parse)

- Source: https://github.com/MinshuG/pyUE4Parse (MIT License, (c) MinshuG)
- Vendored 2026-08-29 for VotvIO. UE4Parse must be importable as a top-level package
  (it uses absolute self-imports), so `votvio/__init__.py` puts this folder on sys.path
  only when no other UE4Parse is importable.

Local patches (all empirically derived; see the VotvIO design doc):
1. `Assets/Exports/StaticMesh/UStaticMesh.py` — removed the unconditional
   `minMobileLODIdx` int32 read for game >= 4.27. CUE4Parse gates this field on the
   per-game flag `StaticMesh.KeepMobileMinLODSettingOnDesktop` (default OFF); VotV is
   cooked without it, and the stray read misaligned every LOD parse
   ("Invalid boolean value", LODs=[]).
2. `Assets/Objects/FPropertyTag.py` — Usmap imports made optional (UE5 mappings,
   never used for UE4.27).
3. `Provider/__init__.py` — MappingProvider import guarded (same reason).
4. `Encryption/FAESKey.py` — `-> AES.new` annotation quoted so the module imports
   without pycryptodome (VotV pak is unencrypted).
5. `Assets/Exports/Textures/Decoder.py` — PIL import made optional (VotvIO decodes
   textures with numpy; the legacy decode() path is unused).
6. Dropped `Assets/Exports/Textures/utils.cp311-win_amd64.pyd` (cp311-only compiled
   texture helper; Blender 5.1 ships python 3.13).
