"""Dumps the main TAA dispatch's input textures (raw bytes) from one or more captures, for offline analysis.

Run inside RenderDoc's embedded Python (tools/rdc.sh tools/rdc_taa_inputs.py) with:
  P3R_RDCS  ';'-separated .rdc captures
  P3R_OUT   output folder; gets <capture>_t<slot>.bin per input and <capture>.json describing them
The main TAA dispatch is the TAAStandalone dispatch whose velocity input (t3) is not the 1x1 dummy (see
docs/recon.md). Depth-stencil textures come out as RenderDoc returns them (for D32S8: depth and stencil interleaved,
8 bytes per pixel).
"""
import json
import os
import traceback

import renderdoc as rd

TAA_HASH = "203152694ac6f5d3b98e5464598f5669"
OUT_DIR = os.environ["P3R_OUT"]
os.makedirs(OUT_DIR, exist_ok=True)
LOG = open(os.path.join(OUT_DIR, "dump.log"), "w", encoding="utf-8")


def log(*parts):
    LOG.write(" ".join(str(p) for p in parts) + "\n")
    LOG.flush()


def shader_hash(refl):
    raw = bytes(refl.rawBytes)
    found = {raw[4:20].hex()} if raw[:4] == b"DXBC" else set()
    pos = raw.find(b"HASH")
    if pos >= 0:
        found.add(raw[pos + 12:pos + 28].hex())
    return found


def dump_capture(path):
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(path, "", None) != rd.ResultCode.Succeeded:
        log("cannot open", path)
        return
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if result != rd.ResultCode.Succeeded:
        log("cannot replay", path, result)
        return
    name = os.path.splitext(os.path.basename(path))[0]
    textures = {int(t.resourceId): t for t in controller.GetTextures()}
    dispatches = []

    def walk(items):
        for a in items:
            if a.flags & rd.ActionFlags.Dispatch:
                dispatches.append(a)
            walk(a.children)

    walk(controller.GetRootActions())
    hashes = {}
    for action in dispatches:
        controller.SetFrameEvent(action.eventId, False)
        pipe = controller.GetPipelineState()
        refl = pipe.GetShaderReflection(rd.ShaderStage.Compute)
        if refl is None:
            continue
        key = int(refl.resourceId)
        if key not in hashes:
            hashes[key] = shader_hash(refl)
        if TAA_HASH not in hashes[key]:
            continue
        inputs = {int(u.access.index): u.descriptor.resource for u in
                  pipe.GetReadOnlyResources(rd.ShaderStage.Compute, True)}
        velocity = textures.get(int(inputs.get(3, rd.ResourceId.Null())))
        if velocity is None or velocity.width <= 1:
            continue
        info = {"event": action.eventId, "inputs": {}}
        for slot, rid in sorted(inputs.items()):
            tex = textures.get(int(rid))
            if tex is None or slot > 4:
                continue
            data = bytes(controller.GetTextureData(rid, rd.Subresource(0, 0, 0)))
            out = os.path.join(OUT_DIR, "%s_t%d.bin" % (name, slot))
            with open(out, "wb") as f:
                f.write(data)
            info["inputs"]["t%d" % slot] = {"tex": int(rid), "width": tex.width, "height": tex.height,
                                            "format": tex.format.Name(), "bytes": len(data)}
            log("%s event %d t%d tex%d %dx%d %s: %d bytes" % (name, action.eventId, slot, int(rid), tex.width,
                                                               tex.height, tex.format.Name(), len(data)))
        with open(os.path.join(OUT_DIR, name + ".json"), "w", encoding="utf-8") as f:
            json.dump(info, f, indent=1)
        break
    controller.Shutdown()
    cap.Shutdown()


try:
    for capture in [p for p in os.environ["P3R_RDCS"].split(";") if p]:
        dump_capture(capture)
except Exception:
    log("FATAL " + traceback.format_exc())
finally:
    LOG.close()
    os._exit(0)
