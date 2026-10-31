"""Dump raw API calls (with parameters) and D3D12 root-signature bindings from a RenderDoc capture.

Run inside RenderDoc's embedded Python (qrenderdoc.exe --python tools/rdc_events.py) with:
  RDC_FILE    path to the .rdc capture
  RDC_OUT    output directory
  RDC_RANGE  "first-last" event range to list every API call in (e.g. 57500-57575)
  RDC_ROOT   comma-separated event IDs whose root signature bindings to dump
"""
import os
import traceback

import renderdoc as rd

CAPTURE = os.environ["RDC_FILE"]
OUT_DIR = os.environ["RDC_OUT"]
FIRST, LAST = (int(x) for x in os.environ["RDC_RANGE"].split("-"))
ROOT_EIDS = [int(x) for x in os.environ.get("RDC_ROOT", "").split(",") if x.strip()]
os.makedirs(OUT_DIR, exist_ok=True)


def sd_value(obj):
    basetype = obj.type.basetype
    data = obj.data
    try:
        if basetype == rd.SDBasic.Resource:
            rid = data.basic.id
            return "res%d '%s'" % (int(rid), NAMES.get(int(rid), "")) if rid != rd.ResourceId.Null() else "null"
        if basetype == rd.SDBasic.UnsignedInteger:
            return str(data.basic.u)
        if basetype == rd.SDBasic.SignedInteger:
            return str(data.basic.i)
        if basetype == rd.SDBasic.Float:
            return "%.6g" % data.basic.d
        if basetype == rd.SDBasic.Boolean:
            return str(data.basic.b)
        if basetype == rd.SDBasic.Enum:
            return data.str or str(data.basic.u)
        return data.str
    except Exception as exc:
        return "?(%s)" % exc


NAMES = {}


def sd_to_text(obj, depth=0, limit=6):
    """Compact rendering of a structured-data object (API call parameters)."""
    pad = "  " * depth
    name = obj.name
    if obj.NumChildren() == 0:
        value = sd_value(obj)
        return ["%s%s = %s" % (pad, name, value)]
    lines = ["%s%s:" % (pad, name)]
    if depth >= limit:
        return lines + ["%s  ..." % pad]
    for i in range(obj.NumChildren()):
        lines.extend(sd_to_text(obj.GetChild(i), depth + 1, limit))
    return lines


def main():
    cap = rd.OpenCaptureFile()
    cap.OpenFile(CAPTURE, "", None)
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    sf = controller.GetStructuredFile()
    NAMES.update({int(r.resourceId): r.name for r in controller.GetResources()})

    out = []

    def walk(actions):
        for action in actions:
            for ev in action.events:
                if FIRST <= ev.eventId <= LAST:
                    chunk = sf.chunks[ev.chunkIndex]
                    out.append("== event %d: %s" % (ev.eventId, chunk.name))
                    for i in range(chunk.NumChildren()):
                        out.extend("   " + line for line in sd_to_text(chunk.GetChild(i), 0))
            walk(action.children)

    walk(controller.GetRootActions())
    with open(os.path.join(OUT_DIR, "events_%d_%d.txt" % (FIRST, LAST)), "w", encoding="utf-8") as f:
        f.write("\n".join(out) + "\n")

    for eid in ROOT_EIDS:
        controller.SetFrameEvent(eid, True)
        d3d = controller.GetD3D12PipelineState()
        lines = ["event %d D3D12State attrs: %s" % (eid, [a for a in dir(d3d) if not a.startswith("_")])]
        for i, el in enumerate(d3d.rootElements):
            fields = []
            for attr in dir(el):
                if attr.startswith("_"):
                    continue
                try:
                    value = getattr(el, attr)
                except Exception:
                    continue
                if callable(value):
                    continue
                text = str(value)
                fields.append("%s=%s" % (attr, text if len(text) < 200 else text[:200] + "..."))
            lines.append("  [%d] %s" % (i, "  ".join(fields)))
        with open(os.path.join(OUT_DIR, "root_%d.txt" % eid), "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
    controller.Shutdown()
    cap.Shutdown()


try:
    main()
except Exception:
    with open(os.path.join(OUT_DIR, "events_error.txt"), "w", encoding="utf-8") as f:
        f.write(traceback.format_exc())
finally:
    os._exit(0)
