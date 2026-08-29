"""Random working poses for articulated fixtures, the way a fresh game load looks.

The game animates these at runtime (dish tracking, turbine spin); cooked templates hold
the neutral pose. Pivot components and axes are measured from each BP's SCS tree:
dish_C: DefaultSceneRoot -> axis_Z (yaw) -> axis_Y (pitch) -> prot_satellite_dish;
windturbine_C: base -> axis_room (yaw, tower top) -> axis_blades (rotor) -> blades;
coordRadarDish_C: base -> rot_Z (spin).
Seeded by the actor's save key so re-imports are stable.
"""
import hashlib
import math
import random

from mathutils import Matrix

# class -> [(pivot component base name, local axis, min_deg, max_deg)]
POSE_TABLE = {
    "dish_C": [("axis_Z", "Z", 0.0, 360.0), ("axis_Y", "Y", -55.0, -10.0)],
    "windturbine_C": [("axis_room", "Z", 0.0, 360.0), ("axis_blades", "Y", 0.0, 360.0)],
    "coordRadarDish_C": [("rot_Z", "Z", 0.0, 360.0)],
    "serverBox_dish_C": [("dish", "Z", 0.0, 360.0)],
}


def _rng(seed_text):
    h = hashlib.sha1(seed_text.encode("utf-8", "replace")).digest()
    return random.Random(int.from_bytes(h[:8], "little"))


def pose_rotation(class_name, comp_base, seed_text):
    """Extra local rotation Matrix for this component, or None."""
    rules = POSE_TABLE.get(class_name)
    if not rules:
        return None
    for base, axis, lo, hi in rules:
        if base == comp_base:
            r = _rng(f"{seed_text}:{comp_base}")
            ang = math.radians(r.uniform(lo, hi))
            return Matrix.Rotation(ang, 4, axis)
    return None


def has_pose(class_name):
    return class_name in POSE_TABLE
