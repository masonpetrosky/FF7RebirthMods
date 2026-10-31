"""Dump the full state of chosen draws/dispatches from a RenderDoc capture.

Run inside RenderDoc's embedded Python (qrenderdoc.exe --python tools/rdc_inspect.py) with:
  RDC_FILE   path to the .rdc capture
  RDC_OUT   output directory
  RDC_EIDS  comma-separated event IDs to inspect
  RDC_SAVE  optional: "1" to also save every bound texture (raw .bin + .png preview)
For each event writes eid_<n>/state.txt (bindings, value ranges, constant buffers) and the shader
disassembly per stage, then exits the RenderDoc process.
"""
import os
import sys
import traceback

import renderdoc as rd

CAPTURE = os.environ["RDC_FILE"]
OUT_DIR = os.environ["RDC_OUT"]
EIDS = [int(x) for x in os.environ["RDC_EIDS"].split(",") if x.strip()]
SAVE = os.environ.get("RDC_SAVE") == "1"
os.makedirs(OUT_DIR, exist_ok=True)
LOG = open(os.path.join(OUT_DIR, "inspect.log"), "a", encoding="utf-8")


def log(*parts):
    LOG.write(" ".join(str(p) for p in parts) + "\n")
    LOG.flush()


def ok(result):
    return getattr(result, "code", result) == rd.ResultCode.Succeeded


def fmt_var(var, indent=0):
    pad = "  " * indent
    lines = []
    if var.members:
        lines.append("%s%s:" % (pad, var.name))
        for member in var.members:
            lines.extend(fmt_var(member, indent + 1))
        return lines
    count = max(1, var.rows) * max(1, var.columns)
    try:
        if var.type in (rd.VarType.Float, rd.VarType.Half, rd.VarType.Double):
            values = ["%.6g" % v for v in var.value.f32v[:count]]
        elif var.type in (rd.VarType.SInt, rd.VarType.SShort, rd.VarType.SByte, rd.VarType.SLong):
            values = [str(v) for v in var.value.s32v[:count]]
        else:
            values = [str(v) for v in var.value.u32v[:count]]
    except Exception:
        values = ["?"]
    lines.append("%s%s (%dx%d %s) = %s" % (pad, var.name, var.rows, var.columns, var.type, ", ".join(values)))
    return lines


def texture_stats(controller, rid, tex):
    try:
        lo, hi = controller.GetMinMax(rid, rd.Subresource(0, 0, 0), rd.CompType.Typeless)
        return "min=(%s) max=(%s)" % (", ".join("%.4g" % v for v in lo.floatValue),
                                      ", ".join("%.4g" % v for v in hi.floatValue))
    except Exception as exc:
        return "minmax failed: %s" % exc


def save_texture(controller, rid, path_base):
    try:
        data = controller.GetTextureData(rid, rd.Subresource(0, 0, 0))
        with open(path_base + ".bin", "wb") as f:
            f.write(bytes(data))
        save = rd.TextureSave()
        save.resourceId = rid
        save.mip = 0
        save.slice.sliceIndex = 0
        save.alpha = rd.AlphaMapping.Discard
        save.destType = rd.FileType.PNG
        controller.SaveTexture(save, path_base + ".png")
    except Exception as exc:
        log("save failed", int(rid), exc)


def inspect(controller, eid, textures, buffers, names, targets):
    out = os.path.join(OUT_DIR, "eid_%d" % eid)
    os.makedirs(out, exist_ok=True)
    controller.SetFrameEvent(eid, True)
    pipe = controller.GetPipelineState()
    lines = ["event %d" % eid]
    try:
        vp = pipe.GetViewport(0)
        lines.append("viewport x=%g y=%g w=%g h=%g" % (vp.x, vp.y, vp.width, vp.height))
    except Exception as exc:
        lines.append("viewport unavailable: %s" % exc)
    # D3D12 keeps the last compute pipeline bound during draws, so the action decides, not the bound shaders.
    action = controller.GetAction(eid) if hasattr(controller, "GetAction") else None
    if action is not None:
        compute = bool(action.flags & rd.ActionFlags.Dispatch)
    else:
        compute = pipe.GetShader(rd.ShaderStage.Compute) != rd.ResourceId.Null()
    stages = [rd.ShaderStage.Compute] if compute else [rd.ShaderStage.Vertex, rd.ShaderStage.Pixel]
    pipeline_obj = pipe.GetComputePipelineObject() if compute else pipe.GetGraphicsPipelineObject()

    def describe(rid):
        key = int(rid)
        tex = textures.get(key)
        if tex is not None:
            return "tex%d %dx%d %s mips=%d '%s'" % (key, tex.width, tex.height, tex.format.Name(),
                                                    tex.mips, names.get(key, ""))
        buf = buffers.get(key)
        if buf is not None:
            return "buf%d %d bytes '%s'" % (key, buf.length, names.get(key, ""))
        return "res%d '%s'" % (key, names.get(key, ""))

    seen = set()
    for stage in stages:
        shader = pipe.GetShader(stage)
        refl = pipe.GetShaderReflection(stage)
        entry = pipe.GetShaderEntryPoint(stage)
        lines.append("")
        lines.append("== %s shader%d entry=%s" % (stage, int(shader), entry))
        for title, used_list, refl_list in (
                ("SRV", pipe.GetReadOnlyResources(stage, True), refl.readOnlyResources if refl else []),
                ("UAV", pipe.GetReadWriteResources(stage, True), refl.readWriteResources if refl else [])):
            for used in used_list:
                idx = used.access.index
                label = refl_list[idx].name if 0 <= idx < len(refl_list) else "#%d" % idx
                rid = used.descriptor.resource
                stats = texture_stats(controller, rid, textures[int(rid)]) if int(rid) in textures else ""
                lines.append("  %s %-28s %s  view=%s  %s" % (title, label, describe(rid),
                                                            used.descriptor.format.Name(), stats))
                seen.add(int(rid))
        # constant buffers with decoded variables when reflection allows it
        if refl:
            for slot, block in enumerate(refl.constantBlocks):
                try:
                    used = pipe.GetConstantBlock(stage, slot, 0)
                    desc = used.descriptor
                    variables = controller.GetCBufferVariableContents(
                        pipeline_obj, shader, stage, entry, slot, desc.resource, desc.byteOffset, desc.byteSize)
                    lines.append("  CBV %s (slot %d, %s +%d, %d bytes)" % (
                        block.name, slot, describe(desc.resource), desc.byteOffset, desc.byteSize))
                    for var in variables:
                        lines.extend(fmt_var(var, 2))
                except Exception as exc:
                    lines.append("  CBV %s (slot %d) failed: %s" % (block.name, slot, exc))
        for target in targets[:1]:
            try:
                text = controller.DisassembleShader(pipeline_obj, refl, target)
                with open(os.path.join(out, "shader_%s.txt" % str(stage).split(".")[-1]), "w",
                          encoding="utf-8") as f:
                    f.write(text)
            except Exception as exc:
                lines.append("  disassembly failed: %s" % exc)
    if not compute:
        for i, desc in enumerate(pipe.GetOutputTargets()):
            if desc.resource != rd.ResourceId.Null():
                lines.append("  RT%d %s view=%s" % (i, describe(desc.resource), desc.format.Name()))
                seen.add(int(desc.resource))
        depth = pipe.GetDepthTarget()
        if depth.resource != rd.ResourceId.Null():
            lines.append("  DSV %s view=%s" % (describe(depth.resource), depth.format.Name()))
    if SAVE:
        for key in sorted(seen):
            if key in textures:
                save_texture(controller, textures[key].resourceId, os.path.join(out, "tex%d" % key))
    with open(os.path.join(out, "state.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    log("inspected", eid)


def main():
    cap = rd.OpenCaptureFile()
    if not ok(cap.OpenFile(CAPTURE, "", None)):
        log("OpenFile failed")
        return
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if not ok(result):
        log("OpenCapture failed:", result)
        return
    textures = {int(t.resourceId): t for t in controller.GetTextures()}
    buffers = {int(b.resourceId): b for b in controller.GetBuffers()}
    names = {int(r.resourceId): r.name for r in controller.GetResources()}
    targets = controller.GetDisassemblyTargets(True)
    log("disassembly targets:", list(targets))
    for eid in EIDS:
        try:
            inspect(controller, eid, textures, buffers, names, targets)
        except Exception:
            log("event", eid, "failed", traceback.format_exc())
    controller.Shutdown()
    cap.Shutdown()


try:
    main()
except Exception:
    log("FATAL", traceback.format_exc())
finally:
    LOG.close()
    os._exit(0)
