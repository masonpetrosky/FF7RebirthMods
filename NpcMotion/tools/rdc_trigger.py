"""Triggers a frame capture in the running game (started by launch_rdc.ps1) and waits until it is written.

Run inside RenderDoc's embedded Python (qrenderdoc.exe --python tools/rdc_trigger.py) with:
  RDC_OUT     log file (one line per event; the capture paths are listed there)
  RDC_FRAMES  optional: consecutive frames to capture (default 1)
  RDC_DELAY   optional: seconds to wait before triggering (default 0)
"""
import os
import time
import traceback

import renderdoc as rd

LOG = open(os.environ["RDC_OUT"], "w", encoding="utf-8")


def log(text):
    LOG.write(text + "\n")
    LOG.flush()


def main():
    targets = []
    ident = rd.EnumerateRemoteTargets("", 0)
    while ident:
        targets.append(ident)
        ident = rd.EnumerateRemoteTargets("", ident)
    log("targets %s" % targets)
    if not targets:
        return
    control = rd.CreateTargetControl("", targets[-1], "ff7r-trigger", True)
    if control is None:
        log("cannot connect")
        return
    log("connected: %s pid %d api %s" % (control.GetTarget(), control.GetPID(), control.GetAPI()))
    time.sleep(float(os.environ.get("RDC_DELAY", "0")))
    frames = int(os.environ.get("RDC_FRAMES", "1"))
    control.TriggerCapture(frames)
    log("triggered %d frame(s)" % frames)
    deadline = time.time() + 300
    got = 0
    while time.time() < deadline and got < frames:
        msg = control.ReceiveMessage(None)
        if msg.type == rd.TargetControlMessageType.NewCapture:
            cap = msg.newCapture
            log("capture id %d frame %d bytes %d path %s" % (cap.captureId, cap.frameNumber, cap.byteSize, cap.path))
            got += 1
        elif msg.type == rd.TargetControlMessageType.Disconnected:
            log("disconnected")
            break
        elif msg.type == rd.TargetControlMessageType.Noop:
            time.sleep(0.1)
        else:
            log("message %s" % msg.type)
    control.Shutdown()
    log("done, %d capture(s)" % got)


try:
    main()
except Exception:
    log("FATAL " + traceback.format_exc())
finally:
    LOG.close()
    os._exit(0)
