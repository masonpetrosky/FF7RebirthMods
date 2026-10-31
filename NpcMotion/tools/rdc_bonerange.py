"""Compare current and previous bones of skinned draws over the bones their vertices actually use.

Run inside RenderDoc's embedded Python (tools/rdc.sh tools/rdc_bonerange.py) with:
  RDC_FILE   path to the .rdc capture
  RDC_OUT    output text file (tab-separated)
  RDC_FIRST, RDC_LAST  event range (skinned draws: VS binds BoneMatrices and PreviousBoneMatrices)
For every draw: the highest bone index referenced by its vertices (ATTRIBUTE3 = BLENDINDICES, over the indices it
draws), then over bones 0..max: the largest difference of rotation/scale entries, of translations (cm), and how
many bones differ at all. A draw whose bones are all identical shows no skinning motion in the velocity buffer.
"""
import os
import struct
import traceback

import renderdoc as rd

out = ["eid\tindices\tused_bones\trot_diff\ttrans_diff_cm\tbones_changed\tblend_attr"]

INT_FORMATS = {
    (rd.CompType.UInt, 1): "B", (rd.CompType.UInt, 2): "H", (rd.CompType.UInt, 4): "I",
    (rd.CompType.SInt, 1): "b", (rd.CompType.SInt, 2): "h", (rd.CompType.SInt, 4): "i",
}


def main():
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(os.environ["RDC_FILE"], "", None) != rd.ResultCode.Succeeded:
        raise RuntimeError("cannot open capture")
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if result != rd.ResultCode.Succeeded:
        raise RuntimeError("cannot replay: %s" % result)
    first, last = int(os.environ["RDC_FIRST"]), int(os.environ["RDC_LAST"])
    actions = []

    def walk(items):
        for a in items:
            if first <= a.eventId <= last and a.flags & rd.ActionFlags.Drawcall:
                actions.append(a)
            if a.children:
                walk(a.children)

    walk(controller.GetRootActions())
    for action in actions:
        try:
            controller.SetFrameEvent(action.eventId, False)
            pipe = controller.GetPipelineState()
            refl = pipe.GetShaderReflection(rd.ShaderStage.Vertex)
            if refl is None:
                continue
            names = [r.name for r in refl.readOnlyResources]
            if "BoneMatrices" not in names or "PreviousBoneMatrices" not in names:
                continue
            views = {}
            for used in pipe.GetReadOnlyResources(rd.ShaderStage.Vertex, True):
                idx = used.access.index
                if 0 <= idx < len(names) and names[idx] in ("BoneMatrices", "PreviousBoneMatrices"):
                    d = used.descriptor
                    views[names[idx]] = (d.resource, int(d.byteOffset), int(d.byteSize))
            # indices of this draw
            ib = pipe.GetIBuffer()
            stride = ib.byteStride or 2
            raw = bytes(controller.GetBufferData(ib.resourceId, ib.byteOffset + action.indexOffset * stride,
                                                 action.numIndices * stride))
            indices = struct.unpack("<%d%s" % (action.numIndices, "H" if stride == 2 else "I"), raw)
            lo, hi = min(indices), max(indices)
            # BLENDINDICES attribute(s): ATTRIBUTE3 (and ATTRIBUTE14 for the extra four influences if present)
            attrs = [a for a in pipe.GetVertexInputs() if a.name.upper() in ("ATTRIBUTE3", "ATTRIBUTE14")]
            vbs = pipe.GetVBuffers()
            max_bone = -1
            desc = []
            for attr in attrs:
                vb = vbs[attr.vertexBuffer]
                fmt = attr.format
                key = (fmt.compType, fmt.compByteWidth)
                if key not in INT_FORMATS:
                    desc.append("%s:%s?" % (attr.name, fmt.Name()))
                    continue
                code = INT_FORMATS[key]
                count = fmt.compCount
                first_vtx = action.baseVertex + lo
                n = hi - lo + 1
                data = bytes(controller.GetBufferData(vb.resourceId, vb.byteOffset + first_vtx * vb.byteStride,
                                                      n * vb.byteStride))
                for v in range(n):
                    base = v * vb.byteStride + attr.byteOffset
                    vals = struct.unpack_from("<%d%s" % (count, code), data, base)
                    max_bone = max(max_bone, max(vals))
                desc.append("%s:%s" % (attr.name, fmt.Name()))
            bones = max_bone + 1
            cur_res, cur_off, cur_size = views["BoneMatrices"]
            prev_res, prev_off, prev_size = views["PreviousBoneMatrices"]
            size = bones * 64
            cur = struct.unpack("<%df" % (size // 4), bytes(controller.GetBufferData(cur_res, cur_off, size)))
            prev = struct.unpack("<%df" % (size // 4), bytes(controller.GetBufferData(prev_res, prev_off, size)))
            rot_diff, trans_diff, changed = 0.0, 0.0, 0
            for i in range(bones):
                r = max(abs(cur[i * 16 + k] - prev[i * 16 + k]) for k in (0, 1, 2, 4, 5, 6, 8, 9, 10))
                t = max(abs(cur[i * 16 + k] - prev[i * 16 + k]) for k in (12, 13, 14))
                rot_diff, trans_diff = max(rot_diff, r), max(trans_diff, t)
                if r > 1e-5 or t > 1e-3:
                    changed += 1
            out.append("%d\t%d\t%d\t%.5g\t%.5g\t%d\t%s" % (action.eventId, action.numIndices, bones, rot_diff,
                                                         trans_diff, changed, " ".join(desc)))
        except Exception as exc:
            out.append("%d\tERROR %s" % (action.eventId, str(exc).replace("\n", " ")))
    controller.Shutdown()
    cap.Shutdown()


try:
    main()
except Exception:
    out.append("FATAL " + traceback.format_exc())
finally:
    with open(os.environ["RDC_OUT"], "w", encoding="utf-8") as f:
        f.write("\n".join(out) + "\n")
    os._exit(0)
