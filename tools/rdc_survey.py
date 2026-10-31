"""Survey a RenderDoc capture of the game: list every draw/dispatch with its bound resources.

Run inside RenderDoc's embedded Python (qrenderdoc.exe --python tools/rdc_survey.py) with:
  RDC_FILE  path to the .rdc capture
  RDC_OUT  output directory (created if missing)
Writes resources.tsv, actions.tsv and survey.log, then exits the RenderDoc process.
"""
import os
import sys
import time
import traceback

import renderdoc as rd

CAPTURE = os.environ["RDC_FILE"]
OUT_DIR = os.environ["RDC_OUT"]
os.makedirs(OUT_DIR, exist_ok=True)
LOG = open(os.path.join(OUT_DIR, "survey.log"), "w", encoding="utf-8")


def log(*parts):
    LOG.write(" ".join(str(p) for p in parts) + "\n")
    LOG.flush()


def ok(result):
    code = getattr(result, "code", result)
    return code == rd.ResultCode.Succeeded


def flatten(actions, out, depth=0, markers=()):
    for action in actions:
        out.append((action, depth, markers))
        if action.children:
            child_markers = markers
            if action.flags & rd.ActionFlags.PushMarker:
                child_markers = markers + (action.customName or "",)
            flatten(action.children, out, depth + 1, child_markers)


def describe_resource(rid, textures, buffers, names):
    if rid == rd.ResourceId.Null():
        return "-"
    key = int(rid)
    name = names.get(key, "")
    tex = textures.get(key)
    if tex is not None:
        return "tex%d[%dx%d%s %s mips=%d%s]" % (
            key, tex.width, tex.height,
            ("x%d" % tex.arraysize) if tex.arraysize > 1 else "",
            tex.format.Name(), tex.mips, (" '%s'" % name) if name else "")
    buf = buffers.get(key)
    if buf is not None:
        return "buf%d[%d bytes%s]" % (key, buf.length, (" '%s'" % name) if name else "")
    return "res%d%s" % (key, (" '%s'" % name) if name else "")


def bound_list(used_list, refl_list, textures, buffers, names):
    parts = []
    for used in used_list:
        desc = used.descriptor
        idx = used.access.index
        label = ""
        if refl_list is not None and 0 <= idx < len(refl_list):
            label = refl_list[idx].name
        view_fmt = ""
        try:
            if desc.format.Name() not in ("", "Unknown"):
                view_fmt = " as " + desc.format.Name()
        except Exception:
            pass
        parts.append("%s=%s%s" % (label or ("#%d" % idx),
                                   describe_resource(desc.resource, textures, buffers, names), view_fmt))
    return parts


def main():
    log("python", sys.version)
    cap = rd.OpenCaptureFile()
    result = cap.OpenFile(CAPTURE, "", None)
    if not ok(result):
        log("OpenFile failed:", result)
        return
    if not cap.LocalReplaySupport():
        log("no local replay support")
        return
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if not ok(result):
        log("OpenCapture failed:", result)
        return
    started = time.time()

    textures = {int(t.resourceId): t for t in controller.GetTextures()}
    buffers = {int(b.resourceId): b for b in controller.GetBuffers()}
    names = {int(r.resourceId): r.name for r in controller.GetResources()}
    with open(os.path.join(OUT_DIR, "resources.tsv"), "w", encoding="utf-8") as f:
        f.write("id\twidth\theight\tdepth\tarray\tmips\tformat\tflags\tname\n")
        for key, t in sorted(textures.items()):
            f.write("%d\t%d\t%d\t%d\t%d\t%d\t%s\t%s\t%s\n" % (
                key, t.width, t.height, t.depth, t.arraysize, t.mips, t.format.Name(),
                str(t.creationFlags), names.get(key, "")))
    log("textures", len(textures), "buffers", len(buffers))

    sf = controller.GetStructuredFile()
    flat = []
    flatten(controller.GetRootActions(), flat)
    log("actions", len(flat))

    stages = [("VS", rd.ShaderStage.Vertex), ("PS", rd.ShaderStage.Pixel), ("CS", rd.ShaderStage.Compute)]
    with open(os.path.join(OUT_DIR, "actions.tsv"), "w", encoding="utf-8") as f:
        f.write("eid\tkind\tname\tmarkers\tdetails\n")
        for action, depth, markers in flat:
            flags = action.flags
            is_draw = bool(flags & rd.ActionFlags.Drawcall)
            is_dispatch = bool(flags & rd.ActionFlags.Dispatch)
            kind = "draw" if is_draw else "dispatch" if is_dispatch else \
                "clear" if flags & rd.ActionFlags.Clear else \
                "copy" if flags & (rd.ActionFlags.Copy | rd.ActionFlags.Resolve) else \
                "present" if flags & rd.ActionFlags.Present else \
                "marker" if flags & rd.ActionFlags.PushMarker else "other"
            name = action.GetName(sf)
            details = []
            if is_draw or is_dispatch:
                try:
                    controller.SetFrameEvent(action.eventId, False)
                    pipe = controller.GetPipelineState()
                    if is_dispatch:
                        details.append("dims=%s" % (tuple(action.dispatchDimension),))
                    else:
                        details.append("indices=%d inst=%d" % (action.numIndices, action.numInstances))
                    for label, stage in stages:
                        shader = pipe.GetShader(stage)
                        if shader == rd.ResourceId.Null():
                            continue
                        refl = pipe.GetShaderReflection(stage)
                        details.append("%s=shader%d" % (label, int(shader)))
                        ro = bound_list(pipe.GetReadOnlyResources(stage, True),
                                        refl.readOnlyResources if refl else None, textures, buffers, names)
                        rw = bound_list(pipe.GetReadWriteResources(stage, True),
                                        refl.readWriteResources if refl else None, textures, buffers, names)
                        if ro:
                            details.append("%s.srv{%s}" % (label, "; ".join(ro)))
                        if rw:
                            details.append("%s.uav{%s}" % (label, "; ".join(rw)))
                    if is_draw:
                        outs = [describe_resource(d.resource, textures, buffers, names)
                                for d in pipe.GetOutputTargets() if d.resource != rd.ResourceId.Null()]
                        if outs:
                            details.append("RT{%s}" % "; ".join(outs))
                        depth_target = pipe.GetDepthTarget()
                        if depth_target.resource != rd.ResourceId.Null():
                            details.append("DS{%s}" % describe_resource(depth_target.resource, textures, buffers, names))
                except Exception:
                    details.append("ERROR " + traceback.format_exc().replace("\n", " | "))
            f.write("%d\t%s\t%s\t%s\t%s\n" % (action.eventId, kind, name, " > ".join(markers), "  ".join(details)))
    log("done in %.1fs" % (time.time() - started))
    controller.Shutdown()
    cap.Shutdown()


try:
    main()
except Exception:
    log("FATAL", traceback.format_exc())
finally:
    LOG.close()
    os._exit(0)
