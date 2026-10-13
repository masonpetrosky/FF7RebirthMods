#!/bin/sh
# Runs a RenderDoc Python script headless: RDC_FILE=<capture> RDC_OUT=<out> tools/rdc.sh tools/<script>.py
# (paths in RDC_* must be Windows paths; the scripts exit RenderDoc themselves).
RD=${RENDERDOC_DIR:-$(dirname "$0")/../../EldenRingDLSS/.tools/renderdoc/RenderDoc_1.46_64}
"$RD/qrenderdoc.exe" --python "$(cygpath -w "$1")"
