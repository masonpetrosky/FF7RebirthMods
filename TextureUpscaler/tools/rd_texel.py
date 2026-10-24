"""RenderDoc (qrenderdoc --python) helper for checking texel density in a capture.

Env: RDC_FILE, RDC_OUT (dir), RDC_MODE = overview | pixels, RDC_PIXELS = "x,y;x,y" (render-target pixels, pixels mode).
overview: saves the last G-buffer pass's colour targets and depth as PNGs (gbuf_<i>.png) to pick pixels from.
pixels: for each pixel, finds the last draw that wrote its depth, reads that draw's transformed vertices, finds the
triangle under the pixel and reports, for every 2-component texture-coordinate candidate in the vertex outputs:
the view depth, the UV change per screen pixel, screen pixels per texel for each bound texture size, and the world
size of one UV unit at that triangle (cm per UV, comparable to the mesh's LocalUVDensities). Writes pixels.json.
The environment is drawn with mesh shaders (IndirectDispatchMesh); their UV0 arrives in COLOR0.xy.
"""
import json
import math
import os
import struct
import traceback

import renderdoc as rd

OUT = os.environ["RDC_OUT"]
os.makedirs(OUT, exist_ok=True)
logf = open(os.path.join(OUT, "rd_texel.log"), "a", encoding="utf-8")


def log(s):
    logf.write(s + "\n")
    logf.flush()


def flatten(actions, acc):
    for a in actions:
        acc.append(a)
        flatten(a.children, acc)
    return acc


def gbuffer_draws(ctl, acts):
    """Draws with a depth target and 4+ colour targets (the base pass)."""
    found = []
    for a in acts:
        if not (a.flags & rd.ActionFlags.Drawcall):
            continue
        outs = [o for o in a.outputs if o != rd.ResourceId.Null()]
        if len(outs) >= 4 and a.depthOut != rd.ResourceId.Null():
            found.append(a)
    return found


def save_png(ctl, rid, path, comp=0):
    ts = rd.TextureSave()
    ts.resourceId = rid
    ts.destType = rd.FileType.PNG
    ts.mip = 0
    ts.slice.sliceIndex = 0
    ts.alpha = rd.AlphaMapping.Discard
    ctl.SaveTexture(ts, path)


def read_floats(ctl, buf, offset, size):
    data = ctl.GetBufferData(buf, offset, size)
    return data


def mat_from_floats(f):
    return [f[0:4], f[4:8], f[8:12], f[12:16]]


def analyze_pixel(ctl, acts_by_eid, hist_id, tex_info, px, py, view_to_clip):
    hist = ctl.PixelHistory(hist_id, px, py, rd.Subresource(0, 0, 0), rd.CompType.Typeless)
    seen = [(h.eventId, h.Passed(), h.depthTestFailed, h.shaderDiscarded) for h in hist]
    writes = [h for h in hist if h.Passed() and h.eventId in acts_by_eid
              and (acts_by_eid[h.eventId].flags & (rd.ActionFlags.Drawcall | rd.ActionFlags.MeshDispatch))
              and h.eventId < 7585]
    if not writes:
        return {"pixel": [px, py], "error": "no base colour write", "history": seen[-12:]}
    eid = writes[-1].eventId
    act = acts_by_eid[eid]
    ctl.SetFrameEvent(eid, True)
    pipe = ctl.GetPipelineState()
    vp = pipe.GetViewport(0)
    # bound pixel-shader textures
    textures = []
    for ro in pipe.GetReadOnlyResources(rd.ShaderStage.Pixel):
        t = tex_info.get(int(ro.descriptor.resource))
        if t and t[0] >= 64:
            textures.append(t)
    mesh_shader = bool(act.flags & rd.ActionFlags.MeshDispatch)
    stage = rd.ShaderStage.Mesh if mesh_shader else rd.ShaderStage.Vertex
    vsout = pipe.GetShaderReflection(stage).outputSignature
    mf = ctl.GetPostVSData(0, 0, rd.MeshDataStage.MeshOut if mesh_shader else rd.MeshDataStage.VSOut)
    log("eid %d %s: post data stride %d, %d indices, index stride %d, outputs %s" % (
        eid, "mesh" if mesh_shader else "vertex", mf.vertexByteStride, mf.numIndices, mf.indexByteStride,
        [(s.semanticName, s.semanticIndex, s.compCount, str(s.systemValue)) for s in vsout]))
    stride = mf.vertexByteStride
    # element offsets in the post-VS buffer, in signature order, each element compCount floats
    elems, off = [], 0
    for s in vsout:
        elems.append((s.semanticName, s.semanticIndex, s.systemValue, s.compCount, off))
        off += 16  # RenderDoc's D3D12 stream-out layout keeps one float4 per output register
    if off != stride:
        elems, off = [], 0
        for s in vsout:
            elems.append((s.semanticName, s.semanticIndex, s.systemValue, s.compCount, off))
            off += 4 * s.compCount
    nidx = mf.numIndices
    if mf.indexResourceId != rd.ResourceId.Null():
        ib = ctl.GetBufferData(mf.indexResourceId, mf.indexByteOffset, nidx * mf.indexByteStride)
        fmt = "<%d%s" % (nidx, "I" if mf.indexByteStride == 4 else "H")
        indices = list(struct.unpack_from(fmt, ib, 0))
    else:
        indices = list(range(nidx))
    nverts = max(indices) + 1 if indices else 0
    vb = ctl.GetBufferData(mf.vertexResourceId, mf.vertexByteOffset, nverts * stride)
    pos_off = next(e[4] for e in elems if e[2] == rd.ShaderBuiltin.Position)

    def vert(i):
        return struct.unpack_from("<4f", vb, i * stride + pos_off)

    def to_screen(c):
        x, y, z, w = c
        return (vp.x + (x / w * 0.5 + 0.5) * vp.width, vp.y + (0.5 - y / w * 0.5) * vp.height, w)

    hit = None
    for t in range(0, nidx - 2, 3):
        i0, i1, i2 = indices[t], indices[t + 1], indices[t + 2]
        c0, c1, c2 = vert(i0), vert(i1), vert(i2)
        if c0[3] <= 0 or c1[3] <= 0 or c2[3] <= 0:
            continue
        s0, s1, s2 = to_screen(c0), to_screen(c1), to_screen(c2)
        qx, qy = px + 0.5, py + 0.5
        den = (s1[1] - s2[1]) * (s0[0] - s2[0]) + (s2[0] - s1[0]) * (s0[1] - s2[1])
        if abs(den) < 1e-9:
            continue
        l0 = ((s1[1] - s2[1]) * (qx - s2[0]) + (s2[0] - s1[0]) * (qy - s2[1])) / den
        l1 = ((s2[1] - s0[1]) * (qx - s2[0]) + (s0[0] - s2[0]) * (qy - s2[1])) / den
        l2 = 1 - l0 - l1
        if min(l0, l1, l2) < -1e-4:
            continue
        depth = 1.0 / (l0 / s0[2] + l1 / s1[2] + l2 / s2[2])
        if hit is None or depth < hit[0]:
            hit = (depth, (i0, i1, i2), (s0, s1, s2))
    if hit is None:
        return {"pixel": [px, py], "eid": eid, "error": "no triangle"}
    depth, (i0, i1, i2), (s0, s1, s2) = hit

    def bary(qx, qy):
        den = (s1[1] - s2[1]) * (s0[0] - s2[0]) + (s2[0] - s1[0]) * (s0[1] - s2[1])
        l0 = ((s1[1] - s2[1]) * (qx - s2[0]) + (s2[0] - s1[0]) * (qy - s2[1])) / den
        l1 = ((s2[1] - s0[1]) * (qx - s2[0]) + (s0[0] - s2[0]) * (qy - s2[1])) / den
        return l0, l1, 1 - l0 - l1

    def persp(vals, qx, qy):
        l = bary(qx, qy)
        ws = (s0[2], s1[2], s2[2])
        num = [sum(l[k] * vals[k][c] / ws[k] for k in range(3)) for c in range(len(vals[0]))]
        den = sum(l[k] / ws[k] for k in range(3))
        return [n / den for n in num]

    # view-space positions from clip (x_view = x_clip / P00, y_view = y_clip / P11, z_view = w)
    p00, p11 = view_to_clip[0][0], view_to_clip[1][1]
    clips = [vert(i0), vert(i1), vert(i2)]
    views = [(c[0] / p00, c[1] / p11, c[3]) for c in clips]

    def sub(a, b):
        return [a[k] - b[k] for k in range(3)]

    def cross(a, b):
        return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]

    area_world = 0.5 * math.sqrt(sum(c * c for c in cross(sub(views[1], views[0]), sub(views[2], views[0]))))
    cands = []
    for name, sidx, sysv, comp, eoff in elems:
        if sysv == rd.ShaderBuiltin.Position:
            continue
        for k in range(0, comp - 1, 2):
            uv = [struct.unpack_from("<2f", vb, i * stride + eoff + 4 * k) for i in (i0, i1, i2)]
            qx, qy = px + 0.5, py + 0.5
            u0 = persp(uv, qx, qy)
            ux = persp(uv, qx + 0.5, qy)
            ux_ = persp(uv, qx - 0.5, qy)
            uy = persp(uv, qx, qy + 0.5)
            uy_ = persp(uv, qx, qy - 0.5)
            dx = math.hypot(ux[0] - ux_[0], ux[1] - ux_[1])
            dy = math.hypot(uy[0] - uy_[0], uy[1] - uy_[1])
            area_uv = 0.5 * abs((uv[1][0] - uv[0][0]) * (uv[2][1] - uv[0][1]) - (uv[2][0] - uv[0][0]) * (uv[1][1] - uv[0][1]))
            cm_per_uv = math.sqrt(area_world / area_uv) if area_uv > 0 else None
            cands.append({"elem": "%s%d.%s" % (name, sidx, "xyzw"[k:k + 2]), "uv": [round(v, 4) for v in u0],
                          "uv_per_px": max(dx, dy), "cm_per_uv": cm_per_uv,
                          "px_per_texel": {str(t[0]): (1.0 / (max(dx, dy) * t[0]) if max(dx, dy) > 0 else None) for t in textures}})
    return {"pixel": [px, py], "eid": eid, "draw": act.customName or "", "indices": nidx,
            "depth_cm": depth, "textures": textures, "candidates": cands}


def main():
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(os.environ["RDC_FILE"], "", None) != rd.ResultCode.Succeeded:
        raise RuntimeError("open failed")
    res, ctl = cap.OpenCapture(rd.ReplayOptions(), None)
    if res != rd.ResultCode.Succeeded:
        raise RuntimeError("replay failed %s" % res)
    acts = flatten(ctl.GetRootActions(), [])
    gb = gbuffer_draws(ctl, acts)
    log("%d actions, %d base-pass draws (eid %d..%d)" % (len(acts), len(gb), gb[0].eventId, gb[-1].eventId))
    texs = {int(t.resourceId): t for t in ctl.GetTextures()}
    tex_info = {k: (t.width, t.height, t.format.Name(), t.mips) for k, t in texs.items()}
    last = gb[-1]
    depth_id = last.depthOut
    if os.environ.get("RDC_MODE", "overview") == "overview":
        ctl.SetFrameEvent(last.eventId, True)
        for i, o in enumerate([o for o in last.outputs if o != rd.ResourceId.Null()]):
            t = texs[int(o)]
            save_png(ctl, o, os.path.join(OUT, "gbuf_%d.png" % i))
            log("gbuf_%d: res %d %dx%d %s" % (i, int(o), t.width, t.height, t.format.Name()))
        t = texs[int(depth_id)]
        log("depth: res %d %dx%d %s" % (int(depth_id), t.width, t.height, t.format.Name()))
        return
    # View_ViewToClip from the vertex shader's View constant buffer at the last base-pass draw
    ctl.SetFrameEvent(last.eventId, True)
    pipe = ctl.GetPipelineState()
    refl = pipe.GetShaderReflection(rd.ShaderStage.Vertex)
    view_to_clip = None
    for i, cb in enumerate(refl.constantBlocks):
        if cb.name not in ("View", "cbView"):
            continue
        bind = pipe.GetConstantBlock(rd.ShaderStage.Vertex, i, 0)
        vars_ = ctl.GetCBufferVariableContents(pipe.GetGraphicsPipelineObject(), refl.resourceId, rd.ShaderStage.Vertex,
                                               pipe.GetShaderEntryPoint(rd.ShaderStage.Vertex), i,
                                               bind.descriptor.resource, bind.descriptor.byteOffset, bind.descriptor.byteSize)
        for v in vars_:
            if v.name.endswith("ViewToClip"):
                f = [v.value.f32v[k] for k in range(16)]
                view_to_clip = mat_from_floats(f)
                log("ViewToClip row-major as read: %s" % view_to_clip)
    if view_to_clip is None:
        raise RuntimeError("View_ViewToClip not found")
    by_eid = {a.eventId: a for a in acts}
    base_colour_id = [o for o in last.outputs if o != rd.ResourceId.Null()][3]  # GBufferC (base colour)
    results = []
    for item in os.environ["RDC_PIXELS"].split(";"):
        x, y = (int(v) for v in item.split(","))
        try:
            results.append(analyze_pixel(ctl, by_eid, base_colour_id, tex_info, x, y, view_to_clip))
        except Exception:
            results.append({"pixel": [x, y], "error": traceback.format_exc()[-800:]})
        log("pixel %d,%d done" % (x, y))
    json.dump(results, open(os.path.join(OUT, "pixels.json"), "w"), indent=1, default=str)
    ctl.Shutdown()
    cap.Shutdown()


try:
    main()
except Exception:
    log("FATAL " + traceback.format_exc())
finally:
    logf.close()
    os._exit(0)
