"""Save the vertex and pixel shader bytecode of given draws (RenderDoc's rawBytes, the container the game passed).

Run inside RenderDoc's embedded Python (tools/rdc.sh tools/rdc_shaderdump.py) with:
  RDC_FILE  path to the .rdc capture
  RDC_OUT   output directory: <eid>_vs.bin, <eid>_ps.bin
  RDC_EIDS  comma-separated draw event IDs
"""
import os
import traceback

import renderdoc as rd

out = os.environ["RDC_OUT"]
os.makedirs(out, exist_ok=True)
try:
    cap = rd.OpenCaptureFile()
    cap.OpenFile(os.environ["RDC_FILE"], "", None)
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    for eid in [int(x) for x in os.environ["RDC_EIDS"].split(",") if x.strip()]:
        controller.SetFrameEvent(eid, False)
        pipe = controller.GetPipelineState()
        for stage, name in ((rd.ShaderStage.Vertex, "vs"), (rd.ShaderStage.Pixel, "ps")):
            refl = pipe.GetShaderReflection(stage)
            if refl is not None:
                with open(os.path.join(out, "%d_%s.bin" % (eid, name)), "wb") as f:
                    f.write(bytes(refl.rawBytes))
    controller.Shutdown()
    cap.Shutdown()
except Exception:
    with open(os.path.join(out, "error.txt"), "w") as f:
        f.write(traceback.format_exc())
os._exit(0)
