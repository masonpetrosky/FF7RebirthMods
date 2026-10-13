"""Screen-space bounding boxes of draws (from RenderDoc's post-VS data) to find which pixels a draw covers.

Run inside RenderDoc's embedded Python (tools/rdc.sh tools/rdc_drawbox.py) with:
  RDC_FILE  path to the .rdc capture
  RDC_OUT   output text file
  RDC_EIDS  comma-separated draw event IDs
Prints per draw: vertex count, x/y pixel range of vertices in front of the camera (viewport mapping), and depth range.
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
    for eid in [int(x) for x in os.environ["RDC_EIDS"].split(",") if x.strip()]:
        controller.SetFrameEvent(eid, True)
        pipe = controller.GetPipelineState()
        vp = pipe.GetViewport(0)
        mesh = controller.GetPostVSData(0, 0, rd.MeshDataStage.VSOut)
        if mesh.vertexResourceId == rd.ResourceId.Null():
            lines.append("%d: no post-VS data" % eid)
            continue
        count = mesh.numIndices
        data = bytes(controller.GetBufferData(mesh.vertexResourceId, mesh.vertexByteOffset,
                                              mesh.vertexByteStride * max(count, 1)))
        xs, ys, zs = [], [], []
        stride = mesh.vertexByteStride
        n = len(data) // stride
        for i in range(n):
            x, y, z, w = struct.unpack_from("<4f", data, i * stride)  # SV_Position is the first output
            if w <= 0:
                continue
            xs.append(vp.x + (x / w * 0.5 + 0.5) * vp.width)
            ys.append(vp.y + (0.5 - y / w * 0.5) * vp.height)
            zs.append(z / w)
        if xs:
            lines.append("%d: %d verts, x %.0f..%.0f, y %.0f..%.0f, z %.5f..%.5f" % (
                eid, n, min(xs), max(xs), min(ys), max(ys), min(zs), max(zs)))
        else:
            lines.append("%d: %d verts, none in front" % (eid, n))
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
