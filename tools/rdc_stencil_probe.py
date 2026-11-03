"""Reads the scene depth-stencil texture through RenderDoc's pixel picker (depth in red, stencil in green) at the
main TAA dispatch, over a grid and a rectangle, as a second check of the stencil plane next to the raw dump of
tools/rdc_taa_inputs.py.

Run inside RenderDoc's embedded Python (tools/rdc.sh tools/rdc_stencil_probe.py) with:
  P3R_RDC     the .rdc capture
  P3R_EVENT   the TAA dispatch's event (21585 in 10211)
  P3R_TEX     the depth-stencil texture's resource id (2166 in 10211)
  P3R_RECT    optional "x0 y0 x1 y1" probed every 8 pixels (such as the ceiling fan)
  P3R_OUT     output log
"""
import os
import traceback

import renderdoc as rd

OUT = open(os.environ["P3R_OUT"], "w", encoding="utf-8")


def log(*parts):
    OUT.write(" ".join(str(p) for p in parts) + "\n")
    OUT.flush()


try:
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(os.environ["P3R_RDC"], "", None) != rd.ResultCode.Succeeded:
        raise RuntimeError("cannot open the capture")
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if result != rd.ResultCode.Succeeded:
        raise RuntimeError("cannot replay: %s" % result)
    controller.SetFrameEvent(int(os.environ["P3R_EVENT"]), True)
    tex = None
    for t in controller.GetTextures():
        if int(t.resourceId) == int(os.environ["P3R_TEX"]):
            tex = t
    if tex is None:
        raise RuntimeError("no texture %s" % os.environ["P3R_TEX"])
    log("texture", int(tex.resourceId), tex.width, tex.height, tex.format.Name())

    def pick(x, y):
        v = controller.PickPixel(tex.resourceId, x, y, rd.Subresource(0, 0, 0), rd.CompType.Typeless)
        return list(v.floatValue)[:2], list(v.uintValue)[:2]

    points = [(x, y) for y in range(40, tex.height, 80) for x in range(40, tex.width, 80)]
    if os.environ.get("P3R_RECT"):
        x0, y0, x1, y1 = (int(v) for v in os.environ["P3R_RECT"].split())
        points += [(x, y) for y in range(y0, y1, 8) for x in range(x0, x1, 8)]
    stencils = {}
    for x, y in points:
        f, u = pick(x, y)
        key = round(f[1], 6)
        stencils.setdefault(key, []).append((x, y, f[0]))
    log("probed %d pixels" % len(points))
    for key in sorted(stencils):
        sample = stencils[key][:3]
        log("green (stencil) %s: %d pixels, e.g. %s" % (key, len(stencils[key]),
                                                         ", ".join("(%d,%d) depth %.6g" % s for s in sample)))
    controller.Shutdown()
    cap.Shutdown()
except Exception:
    log("FATAL " + traceback.format_exc())
finally:
    OUT.close()
    os._exit(0)
