"""Dump the D3D12 root signature and constant buffer bindings at one draw of a capture.

Run inside RenderDoc's embedded Python (tools/rdc.sh tools/rdc_rootsig.py) with:
  RDC_FILE  path to the .rdc capture
  RDC_OUT   output directory (created): rootsig.txt
  RDC_EID   event id of the draw
"""
import os
import traceback

import renderdoc as rd

OUT = os.environ["RDC_OUT"]
os.makedirs(OUT, exist_ok=True)
LOG = open(os.path.join(OUT, "rootsig.txt"), "w", encoding="utf-8")


def log(text):
    LOG.write(text + "\n")
    LOG.flush()


def fields(obj):
    result = {}
    for name in dir(obj):
        if name.startswith("_") or name in ("this", "thisown"):
            continue
        try:
            value = getattr(obj, name)
        except Exception:
            continue
        if callable(value):
            continue
        result[name] = value
    return result


def describe(obj, indent="  ", depth=0):
    for name, value in fields(obj).items():
        if isinstance(value, (int, float, str, bool)) or value is None:
            log("%s%s = %r" % (indent, name, value))
        elif isinstance(value, (bytes, bytearray)):
            log("%s%s = <%d bytes>" % (indent, name, len(value)))
        elif hasattr(value, "__len__") and not hasattr(value, "keys"):
            try:
                items = list(value)
            except Exception:
                log("%s%s = %s" % (indent, name, value))
                continue
            log("%s%s = [%d items]" % (indent, name, len(items)))
            if depth < 2:
                for i, item in enumerate(items[:8]):
                    if isinstance(item, (int, float, str, bool)):
                        log("%s  [%d] %r" % (indent, i, item))
                    else:
                        log("%s  [%d]" % (indent, i))
                        describe(item, indent + "    ", depth + 1)
        else:
            text = str(value)
            if depth < 2 and " object at " in text:
                log("%s%s:" % (indent, name))
                describe(value, indent + "  ", depth + 1)
            else:
                log("%s%s = %s" % (indent, name, text))


def main():
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(os.environ["RDC_FILE"], "", None) != rd.ResultCode.Succeeded:
        raise RuntimeError("cannot open capture")
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if result != rd.ResultCode.Succeeded:
        raise RuntimeError("cannot replay: %s" % result)
    eid = int(os.environ["RDC_EID"])
    controller.SetFrameEvent(eid, True)
    d3d12 = controller.GetD3D12PipelineState()
    log("eid %d root signature %s" % (eid, d3d12.rootSignatureResourceId))
    for i, element in enumerate(d3d12.rootElements):
        log("root element %d:" % i)
        describe(element, "  ", 1)
    pipe = controller.GetPipelineState()
    refl = pipe.GetShaderReflection(rd.ShaderStage.Vertex)
    log("vertex shader constant blocks:")
    for slot, block in enumerate(refl.constantBlocks):
        log("  [%d] %s" % (slot, block.name))
        describe(block, "    ", 2)
    controller.Shutdown()
    cap.Shutdown()


try:
    main()
except Exception:
    log("FATAL " + traceback.format_exc())
finally:
    LOG.close()
    os._exit(0)
