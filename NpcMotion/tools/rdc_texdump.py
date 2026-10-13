"""Dump textures from a RenderDoc capture as raw bytes (mip 0, slice 0) plus a PNG from RenderDoc's own conversion.

Run inside RenderDoc's embedded Python (tools/rdc.sh tools/rdc_texdump.py) with:
  RDC_FILE   path to the .rdc capture
  RDC_OUT    output directory
  RDC_DUMPS  comma-separated <eid>:<texture id> pairs; the texture is read as it is after that event
             (eid 0 = the end of the frame)
Writes <eid>_<id>.bin, <eid>_<id>.png and <eid>_<id>.txt (width height format), and dump.log.
"""
import os
import traceback

import renderdoc as rd

OUT = os.environ["RDC_OUT"]
os.makedirs(OUT, exist_ok=True)
LOG = open(os.path.join(OUT, "dump.log"), "a", encoding="utf-8")


def log(text):
    LOG.write(text + "\n")
    LOG.flush()


def main():
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(os.environ["RDC_FILE"], "", None) != rd.ResultCode.Succeeded:
        raise RuntimeError("cannot open capture")
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if result != rd.ResultCode.Succeeded:
        raise RuntimeError("cannot replay: %s" % result)
    textures = {int(t.resourceId): t for t in controller.GetTextures()}
    last = controller.GetRootActions()[-1].eventId
    for pair in os.environ["RDC_DUMPS"].split(","):
        if not pair.strip():
            continue
        eid, tid = (int(x) for x in pair.split(":"))
        tex = textures.get(tid)
        if tex is None:
            log("no texture %d" % tid)
            continue
        controller.SetFrameEvent(eid or last, True)
        base = os.path.join(OUT, "%d_%d" % (eid, tid))
        data = controller.GetTextureData(tex.resourceId, rd.Subresource(0, 0, 0))
        with open(base + ".bin", "wb") as f:
            f.write(bytes(data))
        with open(base + ".txt", "w") as f:
            f.write("%d %d %s\n" % (tex.width, tex.height, tex.format.Name()))
        save = rd.TextureSave()
        save.resourceId = tex.resourceId
        save.mip = 0
        save.slice.sliceIndex = 0
        save.alpha = rd.AlphaMapping.Discard
        save.destType = rd.FileType.PNG
        controller.SaveTexture(save, base + ".png")
        log("eid %d tex %d %dx%d %s: %d bytes" % (eid, tid, tex.width, tex.height, tex.format.Name(), len(data)))
    controller.Shutdown()
    cap.Shutdown()


try:
    main()
except Exception:
    log("FATAL " + traceback.format_exc())
finally:
    LOG.close()
    os._exit(0)
