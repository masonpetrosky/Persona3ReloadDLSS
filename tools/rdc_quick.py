"""Quick check of a RenderDoc capture: does it open and replay, and what is in it.

Run inside RenderDoc's embedded Python (tools/rdc.sh tools/rdc_quick.py) with:
  P3R_RDC  path to the .rdc capture
  P3R_OUT  output log file
Logs the API, the action counts, the debug markers (if the game emits any) and the first and last actions, then
exits the RenderDoc process.
"""
import collections
import os
import traceback

import renderdoc as rd

LOG = open(os.environ["P3R_OUT"], "w", encoding="utf-8")


def log(*parts):
    LOG.write(" ".join(str(p) for p in parts) + "\n")
    LOG.flush()


def flatten(actions, out, depth=0, markers=()):
    for action in actions:
        out.append((action, depth, markers))
        if action.children:
            child = markers + ((action.customName or "",) if action.flags & rd.ActionFlags.PushMarker else ())
            flatten(action.children, out, depth + 1, child)


def main():
    cap = rd.OpenCaptureFile()
    result = cap.OpenFile(os.environ["P3R_RDC"], "", None)
    if result != rd.ResultCode.Succeeded:
        log("open failed:", result)
        return
    log("local replay:", cap.LocalReplaySupport())
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if result != rd.ResultCode.Succeeded:
        log("replay failed:", result)
        return
    props = controller.GetAPIProperties()
    log("api:", props.pipelineType, "vendor:", props.vendor, "degraded:", props.degraded)
    actions = []
    flatten(controller.GetRootActions(), actions)
    kinds = collections.Counter()
    for action, _, _ in actions:
        f = action.flags
        if f & rd.ActionFlags.Dispatch:
            kinds["dispatch"] += 1
        elif f & rd.ActionFlags.Drawcall:
            kinds["draw"] += 1
        elif f & rd.ActionFlags.PushMarker:
            kinds["marker"] += 1
        elif f & rd.ActionFlags.Present:
            kinds["present"] += 1
        elif f & rd.ActionFlags.DispatchRay:
            kinds["dispatch_rays"] += 1
        else:
            kinds["other"] += 1
    log("actions:", len(actions), dict(kinds))
    textures = controller.GetTextures()
    big = collections.Counter((t.width, t.height) for t in textures if t.width >= 1280)
    log("textures:", len(textures), "largest sizes:", big.most_common(6))
    markers = [a.customName for a, _, _ in actions if a.flags & rd.ActionFlags.PushMarker]
    log("first markers:", markers[:40])
    log("--- first 20 and last 20 actions")
    for action, depth, mk in actions[:20] + [(None, 0, ())] + actions[-20:]:
        if action is None:
            log("...")
            continue
        log(action.eventId, "  " * depth + action.GetName(controller.GetStructuredFile()), "|", "/".join(mk)[-120:])
    controller.Shutdown()
    cap.Shutdown()
    log("done")


try:
    main()
except Exception:
    log("FATAL " + traceback.format_exc())
finally:
    LOG.close()
    os._exit(0)
