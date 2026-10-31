"""Data flow of an event range: per draw/dispatch its shader names and the resource IDs it reads and writes, to follow
a texture through the post-processing (e.g. which motion texture the motion blur reads).

Run inside RenderDoc's embedded Python (qrenderdoc.exe --python tools/rdc_flow.py) with:
  RDC_FILE    path to the .rdc capture
  RDC_OUT    output text file
  RDC_FIRST, RDC_LAST  event range
One line per action: event, VS / PS (or CS) debug names, reads (register name = resource id size format) and writes
(render targets, depth, UAVs).
"""
import os
import re
import traceback

import renderdoc as rd

NAME_RE = re.compile(rb"[\\/]([A-Za-z0-9_\[\]@\.\-]+)\.[pcv]po\.pdb")


def debug_name(refl):
    if refl is None:
        return "-"
    m = NAME_RE.search(bytes(refl.rawBytes))
    return m.group(1).decode(errors="replace") if m else "?"


def main():
    lines = []
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(os.environ["RDC_FILE"], "", None) != rd.ResultCode.Succeeded:
        raise RuntimeError("cannot open capture")
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if result != rd.ResultCode.Succeeded:
        raise RuntimeError("cannot replay capture: %s" % result)
    try:
        first, last = int(os.environ["RDC_FIRST"]), int(os.environ["RDC_LAST"])
        textures = {int(t.resourceId): t for t in controller.GetTextures()}

        def desc(rid):
            t = textures.get(int(rid))
            return "%d(%dx%d %s)" % (int(rid), t.width, t.height, t.format.Name()) if t else "%d(buf)" % int(rid)

        actions = []

        def walk(items):
            for a in items:
                if a.flags & (rd.ActionFlags.Drawcall | rd.ActionFlags.Dispatch) and first <= a.eventId <= last:
                    actions.append(a)
                walk(a.children)

        walk(controller.GetRootActions())
        for a in actions:
            controller.SetFrameEvent(a.eventId, False)
            pipe = controller.GetPipelineState()
            compute = bool(a.flags & rd.ActionFlags.Dispatch)
            stages = [rd.ShaderStage.Compute] if compute else [rd.ShaderStage.Vertex, rd.ShaderStage.Pixel]
            names = "/".join(debug_name(pipe.GetShaderReflection(s)) for s in stages)
            reads, writes = [], []
            for s in stages:
                refl = pipe.GetShaderReflection(s)
                for used in pipe.GetReadOnlyResources(s, True):
                    i = used.access.index
                    label = refl.readOnlyResources[i].name if refl and 0 <= i < len(refl.readOnlyResources) else "#%d" % i
                    reads.append("%s=%s" % (label, desc(used.descriptor.resource)))
                for used in pipe.GetReadWriteResources(s, True):
                    writes.append("uav " + desc(used.descriptor.resource))
            if not compute:
                for d in pipe.GetOutputTargets():
                    if d.resource != rd.ResourceId.Null():
                        writes.append("rt " + desc(d.resource))
                depth = pipe.GetDepthTarget()
                if depth.resource != rd.ResourceId.Null():
                    writes.append("ds " + desc(depth.resource))
            lines.append("%d %s n=%d | reads %s | writes %s" % (a.eventId, names, a.numIndices * max(1, a.numInstances),
                                                             "; ".join(reads), "; ".join(writes)))
    except Exception:
        lines.append(traceback.format_exc())
    finally:
        controller.Shutdown()
        cap.Shutdown()
        with open(os.environ["RDC_OUT"], "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
        os._exit(0)


main()
