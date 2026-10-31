"""Dump the state of every skinned base pass draw of a capture, to compare consecutive frames (skin_frames.py).

Run inside RenderDoc's embedded Python (tools/rdc.sh tools/rdc_skinstate.py) with:
  RDC_FILE  path to the .rdc capture
  RDC_OUT   output directory (created): draws.json plus <eid>.bin per draw
The base pass is found as the draws whose render targets include an R16G16_UNORM velocity target among 4 or more.
Per draw: eid, index buffer/vertex buffer identity (to match draws across captures of one session), the object
transform now and last frame, the bones its vertices use (current and previous, 16 floats each) and the vertex
shader's CURRENT_/PREVIOUS_CLIP_SPACE_POSITION for every post-VS vertex. <eid>.bin holds, as float32:
bones_cur, bones_prev (bones*16 each), then cur_clip, prev_clip (verts*4 each).
"""
import json
import os
import struct
import traceback

import renderdoc as rd

OUT = os.environ["RDC_OUT"]
os.makedirs(OUT, exist_ok=True)
LOG = open(os.path.join(OUT, "skinstate.log"), "w", encoding="utf-8")

INT_FORMATS = {
    (rd.CompType.UInt, 1): "B", (rd.CompType.UInt, 2): "H", (rd.CompType.UInt, 4): "I",
    (rd.CompType.SInt, 1): "b", (rd.CompType.SInt, 2): "h", (rd.CompType.SInt, 4): "i",
}


def log(text):
    LOG.write(text + "\n")
    LOG.flush()


def flatten_vars(variables, prefix=""):
    for var in variables:
        if var.members:
            yield from flatten_vars(var.members, prefix + var.name + ".")
        else:
            yield prefix + var.name, var


def cb_values(controller, pipe, stage, refl, wanted):
    values = {}
    shader = pipe.GetShader(stage)
    entry = pipe.GetShaderEntryPoint(stage)
    pipeline_obj = pipe.GetGraphicsPipelineObject()
    for slot, block in enumerate(refl.constantBlocks):
        used = pipe.GetConstantBlock(stage, slot, 0)
        desc = used.descriptor
        try:
            variables = controller.GetCBufferVariableContents(
                pipeline_obj, shader, stage, entry, slot, desc.resource, desc.byteOffset, desc.byteSize)
        except Exception:
            continue
        for name, var in flatten_vars(variables):
            if name in wanted:
                count = max(1, var.rows) * max(1, var.columns)
                if var.type in (rd.VarType.Float, rd.VarType.Half):
                    values[name] = list(var.value.f32v[:count])
                else:
                    values[name] = list(var.value.s32v[:count])
    return values


def main():
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(os.environ["RDC_FILE"], "", None) != rd.ResultCode.Succeeded:
        raise RuntimeError("cannot open capture")
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if result != rd.ResultCode.Succeeded:
        raise RuntimeError("cannot replay: %s" % result)
    textures = {int(t.resourceId): t for t in controller.GetTextures()}

    actions = []

    def walk(items):
        for a in items:
            if a.flags & rd.ActionFlags.Drawcall:
                actions.append(a)
            if a.children:
                walk(a.children)

    walk(controller.GetRootActions())
    draws = []
    view_values = None
    for action in actions:
        if action.numIndices == 0:
            continue
        controller.SetFrameEvent(action.eventId, False)
        pipe = controller.GetPipelineState()
        targets = [t for t in pipe.GetOutputTargets() if t.resource != rd.ResourceId.Null()]
        if len(targets) < 4:
            continue
        if not any(int(t.resource) in textures and textures[int(t.resource)].format.Name() == "R16G16_UNORM"
                   for t in targets):
            continue
        refl = pipe.GetShaderReflection(rd.ShaderStage.Vertex)
        if refl is None:
            continue
        names = [r.name for r in refl.readOnlyResources]
        if "BoneMatrices" not in names or "PreviousBoneMatrices" not in names:
            continue
        try:
            views = {}
            for used in pipe.GetReadOnlyResources(rd.ShaderStage.Vertex, True):
                idx = used.access.index
                if 0 <= idx < len(names) and names[idx] in ("BoneMatrices", "PreviousBoneMatrices"):
                    d = used.descriptor
                    views[names[idx]] = (d.resource, int(d.byteOffset), int(d.byteSize))
            ib = pipe.GetIBuffer()
            stride = ib.byteStride or 2
            raw = bytes(controller.GetBufferData(ib.resourceId, ib.byteOffset + action.indexOffset * stride,
                                                 action.numIndices * stride))
            indices = struct.unpack("<%d%s" % (action.numIndices, "H" if stride == 2 else "I"), raw)
            lo, hi = min(indices), max(indices)
            vbs = pipe.GetVBuffers()
            max_bone = 0
            for attr in pipe.GetVertexInputs():
                if attr.name.upper() not in ("ATTRIBUTE3", "ATTRIBUTE14"):
                    continue
                vb = vbs[attr.vertexBuffer]
                key = (attr.format.compType, attr.format.compByteWidth)
                if key not in INT_FORMATS:
                    continue
                n = hi - lo + 1
                data = bytes(controller.GetBufferData(
                    vb.resourceId, vb.byteOffset + (action.baseVertex + lo) * vb.byteStride, n * vb.byteStride))
                fmt = "<%d%s" % (attr.format.compCount, INT_FORMATS[key])
                for v in range(n):
                    max_bone = max(max_bone, max(struct.unpack_from(fmt, data, v * vb.byteStride + attr.byteOffset)))
            bones = max_bone + 1
            cur_res, cur_off, _ = views["BoneMatrices"]
            prev_res, prev_off, _ = views["PreviousBoneMatrices"]
            bones_cur = bytes(controller.GetBufferData(cur_res, cur_off, bones * 64))
            bones_prev = bytes(controller.GetBufferData(prev_res, prev_off, bones * 64))
            wanted = {"Primitive_LocalToWorld", "Primitive_PreviousLocalToWorld", "PerBoneMotionBlur",
                      "View_TemporalAAJitter", "View_ViewSizeAndInvSize"}
            values = cb_values(controller, pipe, rd.ShaderStage.Vertex, refl, wanted)
            if view_values is None and "View_TemporalAAJitter" in values:
                view_values = {k: values[k] for k in ("View_TemporalAAJitter", "View_ViewSizeAndInvSize")
                               if k in values}
            # post-VS clip positions (RenderDoc's layout: SV_Position first, then the signature order)
            controller.SetFrameEvent(action.eventId, True)
            pipe = controller.GetPipelineState()
            offsets, offset = {}, 16
            for sig in refl.outputSignature:
                if sig.systemValue == rd.ShaderBuiltin.Position:
                    continue
                offsets[(sig.semanticIdxName or sig.semanticName).upper()] = offset
                offset += sig.compCount * 4
            cur_key = next(k for k in offsets if k.startswith("CURRENT_CLIP_SPACE_POSITION"))
            prev_key = next(k for k in offsets if k.startswith("PREVIOUS_CLIP_SPACE_POSITION"))
            mesh = controller.GetPostVSData(0, 0, rd.MeshDataStage.VSOut)
            vstride = mesh.vertexByteStride
            post = bytes(controller.GetBufferData(mesh.vertexResourceId, mesh.vertexByteOffset,
                                                  vstride * mesh.numIndices))
            count = len(post) // vstride
            cur_clip = b"".join(post[i * vstride + offsets[cur_key]:i * vstride + offsets[cur_key] + 16]
                                for i in range(count))
            prev_clip = b"".join(post[i * vstride + offsets[prev_key]:i * vstride + offsets[prev_key] + 16]
                                 for i in range(count))
            with open(os.path.join(OUT, "%d.bin" % action.eventId), "wb") as f:
                f.write(bones_cur + bones_prev + cur_clip + prev_clip)
            draws.append({
                "eid": action.eventId, "indices": action.numIndices, "first_index": action.indexOffset,
                "base_vertex": action.baseVertex, "ib": [int(ib.resourceId), int(ib.byteOffset)],
                "vbs": [[int(v.resourceId), int(v.byteOffset)] for v in vbs if v.resourceId != rd.ResourceId.Null()],
                "l2w": values.get("Primitive_LocalToWorld"), "prev_l2w": values.get("Primitive_PreviousLocalToWorld"),
                "perbone": values.get("PerBoneMotionBlur", [None])[0], "bones": bones, "verts": count,
                "bone_views": [[int(cur_res), cur_off], [int(prev_res), prev_off]],
            })
        except Exception as exc:
            log("eid %d: %s" % (action.eventId, traceback.format_exc()))
    with open(os.path.join(OUT, "draws.json"), "w", encoding="utf-8") as f:
        json.dump({"view": view_values, "draws": draws}, f)
    log("%d skinned draws" % len(draws))
    controller.Shutdown()
    cap.Shutdown()


try:
    main()
except Exception:
    log("FATAL " + traceback.format_exc())
finally:
    LOG.close()
    os._exit(0)
