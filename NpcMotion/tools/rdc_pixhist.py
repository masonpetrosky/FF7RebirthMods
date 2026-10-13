"""Pixel history: which events wrote given pixels of a texture, with the values before and after each write.

Run inside RenderDoc's embedded Python (tools/rdc.sh tools/rdc_pixhist.py) with:
  RDC_FILE   path to the .rdc capture
  RDC_OUT    output text file
  RDC_TEX    texture id
  RDC_PIXELS semicolon-separated x,y[,label] (texture coordinates)
  RDC_EID    optional: history up to this event (default: end of frame)
"""
import os
import traceback

import renderdoc as rd

lines = []


def fmt(value):
    return "(%s)" % ", ".join("%.5g" % v for v in value.col.floatValue[:4])


def main():
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(os.environ["RDC_FILE"], "", None) != rd.ResultCode.Succeeded:
        raise RuntimeError("cannot open capture")
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if result != rd.ResultCode.Succeeded:
        raise RuntimeError("cannot replay: %s" % result)
    tid = int(os.environ["RDC_TEX"])
    tex = next(t for t in controller.GetTextures() if int(t.resourceId) == tid)
    eid = int(os.environ.get("RDC_EID", "0")) or controller.GetRootActions()[-1].eventId
    controller.SetFrameEvent(eid, True)
    for item in os.environ["RDC_PIXELS"].split(";"):
        parts = item.split(",")
        x, y = int(parts[0]), int(parts[1])
        label = parts[2] if len(parts) > 2 else ""
        lines.append("== pixel %d,%d %s (tex %d %s)" % (x, y, label, tid, tex.format.Name()))
        history = controller.PixelHistory(tex.resourceId, x, y, rd.Subresource(0, 0, 0), rd.CompType.Typeless)
        for mod in history:
            flags = []
            if mod.shaderDiscarded:
                flags.append("discarded")
            if mod.depthTestFailed:
                flags.append("depthfail")
            if mod.stencilTestFailed:
                flags.append("stencilfail")
            if mod.backfaceCulled:
                flags.append("backface")
            if mod.scissorClipped:
                flags.append("scissor")
            lines.append("  eid %d prim %d: %s -> shader %s -> %s depth %.6g %s" % (
                mod.eventId, mod.primitiveID, fmt(mod.preMod), fmt(mod.shaderOut), fmt(mod.postMod),
                mod.postMod.depth, " ".join(flags)))
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
