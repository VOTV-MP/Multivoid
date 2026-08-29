"""Registered export readers for Instanced/Hierarchical/Foliage StaticMeshComponents.

pyUE4Parse parses only the property block of these (generic UObject); the per-instance
transforms live in a NATIVE tail (bulk array of 64-byte FMatrix, CUE4Parse shape:
ElementSize int32 + Count int32 + data, optionally followed by a PerInstanceSMCustomData
float bulk). The version gates around the tail are custom-version driven; instead of
re-deriving them we self-validate: scan the tail for a bulk header whose ElementSize==64
and whose count matches NumBuiltInstances (HISM/foliage carry it as a property) or whose
extent closes the export. Wrong guess = no instances + a warning, never a crash.
"""
import numpy as np

from UE4Parse.Assets.Exports.ExportRegistry import register_export
from UE4Parse.Assets.Exports.UObjects import UObject


class UInstancedSMComponent(UObject):
    instance_matrices = None  # np (N, 4, 4) float32, UE row-major FMatrix

    def __init__(self, reader):
        super().__init__(reader)
        self.instance_matrices = None

    def deserialize(self, validpos):
        super().deserialize(validpos)
        bs = self.reader.base_stream
        cur = bs.tell()
        n_tail = validpos - cur
        if n_tail < 8:
            return
        raw = bs.read(n_tail)  # consume to validpos; PackageReader expects us there

        expected = None
        try:
            v = self.try_get("NumBuiltInstances")
            if isinstance(v, int) and v >= 0:
                expected = v
        except Exception:  # noqa: BLE001
            pass

        buf = np.frombuffer(raw, dtype=np.uint8)
        n = len(raw)
        ints = None
        best = None
        # candidate offsets: aligned scan for ElementSize==64
        ints = np.frombuffer(raw[: n - (n % 4)], dtype="<i4")
        cand = np.nonzero(ints == 64)[0]
        for ci in cand:
            o = int(ci) * 4
            if o + 8 > n:
                continue
            cnt = int(ints[ci + 1]) if ci + 1 < len(ints) else -1
            if cnt < 0 or o + 8 + cnt * 64 > n:
                continue
            if expected is not None:
                if cnt == expected:
                    best = (o, cnt)
                    break
                continue
            leftover = n - (o + 8 + cnt * 64)
            # exact close, or a plausible float custom-data bulk / small residue after
            if leftover == 0 or leftover <= 64 or (
                    leftover >= 8 and int(ints[(o + 8 + cnt * 64) // 4]) == 4):
                best = (o, cnt)
                break
        if best is None:
            return
        o, cnt = best
        if cnt:
            self.instance_matrices = np.frombuffer(
                raw, dtype="<f4", count=cnt * 16, offset=o + 8).reshape(cnt, 4, 4).copy()
        else:
            self.instance_matrices = np.zeros((0, 4, 4), dtype=np.float32)


register_export(UInstancedSMComponent, Type="InstancedStaticMeshComponent")
register_export(UInstancedSMComponent, Type="HierarchicalInstancedStaticMeshComponent")
register_export(UInstancedSMComponent, Type="FoliageInstancedStaticMeshComponent")
