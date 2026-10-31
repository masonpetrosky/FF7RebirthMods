"""Per-vertex screen motion that the base pass vertex shader computes (CURRENT_ vs PREVIOUS_CLIP_SPACE_POSITION).

Run inside RenderDoc's embedded Python (tools/rdc.sh tools/rdc_postvs_motion.py) with:
  RDC_FILE  path to the .rdc capture
  RDC_OUT   output text file
  RDC_EIDS  comma-separated draw event IDs
  RDC_JITTER  optional "cx,cy,px,py": View_TemporalAAJitter (current xy, previous zw), removed like UE's velocity
Prints per draw: vertices in front, motion in pixels (median, p90, max) and the vertex with the largest motion.
"""
import os
import struct
import traceback

import renderdoc as rd

lines = []


def main():
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(os.environ["RDC_FILE"], "", None) != rd.ResultCode.Succeeded:
        raise RuntimeError("cannot open capture")
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if result != rd.ResultCode.Succeeded:
        raise RuntimeError("cannot replay: %s" % result)
    jitter = [float(x) for x in os.environ.get("RDC_JITTER", "0,0,0,0").split(",")]
    for eid in [int(x) for x in os.environ["RDC_EIDS"].split(",") if x.strip()]:
        controller.SetFrameEvent(eid, True)
        pipe = controller.GetPipelineState()
        vp = pipe.GetViewport(0)
        refl = pipe.GetShaderReflection(rd.ShaderStage.Vertex)
        # RenderDoc's post-VS buffer: SV_Position first, then the other outputs in signature order, packed
        offsets, offset = {"SV_POSITION": (0, 4)}, 16
        for sig in refl.outputSignature:
            name = (sig.semanticIdxName or sig.semanticName).upper()
            if sig.systemValue == rd.ShaderBuiltin.Position:
                continue
            offsets[name] = (offset, sig.compCount)
            offset += sig.compCount * 4
        mesh = controller.GetPostVSData(0, 0, rd.MeshDataStage.VSOut)
        stride = mesh.vertexByteStride
        cur_key = next((k for k in offsets if k.startswith("CURRENT_CLIP_SPACE_POSITION")), None)
        prev_key = next((k for k in offsets if k.startswith("PREVIOUS_CLIP_SPACE_POSITION")), None)
        if cur_key is None or prev_key is None:
            lines.append("%d: outputs %s (no clip positions)" % (eid, sorted(offsets)))
            continue
        if offset != stride:
            lines.append("%d: signature %d bytes vs post-VS stride %d; offsets may be off: %s" % (
                eid, offset, stride, sorted(offsets.items(), key=lambda kv: kv[1])))
        n = mesh.numIndices
        data = bytes(controller.GetBufferData(mesh.vertexResourceId, mesh.vertexByteOffset, stride * n))
        motions = []
        best = None
        for i in range(len(data) // stride):
            c = struct.unpack_from("<4f", data, i * stride + offsets[cur_key][0])
            p = struct.unpack_from("<4f", data, i * stride + offsets[prev_key][0])
            if c[3] <= 0 or p[3] <= 0:
                continue
            dx = (c[0] / c[3] - p[0] / p[3]) - (jitter[0] - jitter[2])
            dy = (c[1] / c[3] - p[1] / p[3]) - (jitter[1] - jitter[3])
            mx, my = dx * 0.5 * vp.width, -dy * 0.5 * vp.height
            m = (mx * mx + my * my) ** 0.5
            motions.append(m)
            if best is None or m > best[0]:
                sx = vp.x + (c[0] / c[3] * 0.5 + 0.5) * vp.width
                sy = vp.y + (0.5 - c[1] / c[3] * 0.5) * vp.height
                best = (m, sx, sy, mx, my)
        motions.sort()
        if motions:
            k = len(motions)
            lines.append("%d: %d verts, motion px median %.3f p90 %.3f max %.3f at (%.0f,%.0f) d=(%.2f,%.2f)" % (
                eid, k, motions[k // 2], motions[int(k * 0.9)], motions[-1], best[1], best[2], best[3], best[4]))
        else:
            lines.append("%d: no vertices in front" % eid)
    controller.Shutdown()
    cap.Shutdown()


try:
    main()
except Exception:
    lines.append("FATAL " + traceback.format_exc())
finally:
    with open(os.environ["RDC_OUT"], "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    os._exit(0)
