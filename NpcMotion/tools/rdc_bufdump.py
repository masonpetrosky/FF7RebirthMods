"""Write raw bytes of a buffer range from a RenderDoc capture (RDC_FILE, RDC_OUT file, RDC_BUF id, RDC_OFF, RDC_LEN)."""
import os
import renderdoc as rd
try:
    cap = rd.OpenCaptureFile()
    cap.OpenFile(os.environ["RDC_FILE"], "", None)
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    controller.SetFrameEvent(int(os.environ.get("RDC_EID", "0")) or controller.GetRootActions()[-1].eventId, True)
    target = int(os.environ["RDC_BUF"])
    rid = next(b.resourceId for b in controller.GetBuffers() if int(b.resourceId) == target)
    data = controller.GetBufferData(rid, int(os.environ["RDC_OFF"]), int(os.environ["RDC_LEN"]))
    with open(os.environ["RDC_OUT"], "wb") as f:
        f.write(bytes(data))
    controller.Shutdown(); cap.Shutdown()
finally:
    os._exit(0)
