"""Locate the VotV install (the folder holding VotV-WindowsNoEditor.pak) and the save dir."""
import os

PAK_NAME = "VotV-WindowsNoEditor.pak"

# Known install shapes, deepest-first relative probes from a user-picked root.
_REL_PROBES = (
    os.path.join("VotV", "Content", "Paks"),
    os.path.join("WindowsNoEditor", "VotV", "Content", "Paks"),
    os.path.join("Content", "Paks"),
    "",
)

# Machine-common fallbacks tried when no root is configured.
_DEFAULT_ROOTS = (
    r"D:\Projects\Programming\VOTV_MP\Game_0.9.0n_HOST\WindowsNoEditor",
    r"C:\Games\VotV",
    r"D:\Games\VotV",
)


def _probe_root(root):
    if not root:
        return None
    root = os.path.abspath(os.path.expanduser(root))
    for rel in _REL_PROBES:
        cand = os.path.join(root, rel)
        if os.path.isfile(os.path.join(cand, PAK_NAME)):
            return cand
    # walk one level of subfolders (itch installs often nest once)
    try:
        subs = [os.path.join(root, d) for d in os.listdir(root)
                if os.path.isdir(os.path.join(root, d))]
    except OSError:
        return None
    for sub in subs:
        for rel in _REL_PROBES:
            cand = os.path.join(sub, rel)
            if os.path.isfile(os.path.join(cand, PAK_NAME)):
                return cand
    return None


def find_paks_dir(configured_root=""):
    """Return the folder containing the pak, or None."""
    env = os.environ.get("VOTVIO_PAKS", "")
    if env and os.path.isfile(os.path.join(env, PAK_NAME)):
        return env
    hit = _probe_root(configured_root)
    if hit:
        return hit
    for root in _DEFAULT_ROOTS:
        hit = _probe_root(root)
        if hit:
            return hit
    return None


def default_save_dir():
    local = os.environ.get("LOCALAPPDATA", "")
    cand = os.path.join(local, "VotV", "Saved", "SaveGames")
    return cand if os.path.isdir(cand) else ""
