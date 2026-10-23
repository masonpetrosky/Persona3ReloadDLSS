"""Finds HUD draws whose pixel shader writes values outside 0..1 (or NaN), which 8-bit targets clamp away.

Run inside RenderDoc's embedded Python (tools/rdc.sh tools/rdc_hud_outputs.py) with:
  P3R_RDCS  ';'-separated .rdc captures
  P3R_OUT   output text file
  P3R_STEP  optional: grid step in pixels of the back buffer (default 48)
  P3R_RECT  optional: x0,y0,x1,y1 in back buffer pixels to sample (default: the whole screen)
Samples a pixel grid on the swap chain image and on every 8-bit render target of half its size (the HUD's
1920x1080 targets) with pixel history, which reports each draw's shader output before blending and clamping. An HDR
mod that turns those targets into float ones (RenoDX does, with resource cloning) keeps such values: a bright or
wrongly coloured shape where the game looks fine.
"""
import math
import os
import traceback

import renderdoc as rd

OUT = open(os.environ["P3R_OUT"], "w", encoding="utf-8")
STEP = int(os.environ.get("P3R_STEP", "48"))
EPS = 1.0 / 512


def log(*parts):
    OUT.write(" ".join(str(p) for p in parts) + "\n")
    OUT.flush()


def shader_hash(refl):
    if refl is None:
        return "-"
    raw = bytes(refl.rawBytes)
    pos = raw.find(b"HASH")
    return raw[pos + 12:pos + 28].hex() if pos >= 0 else raw[4:20].hex()


def bad(values):
    return any(math.isnan(v) or math.isinf(v) or v < -EPS or v > 1 + EPS for v in values)


def describe_event(controller, textures, eid):
    controller.SetFrameEvent(eid, False)
    pipe = controller.GetPipelineState()
    parts = []
    for stage, name in ((rd.ShaderStage.Vertex, "VS"), (rd.ShaderStage.Pixel, "PS")):
        refl = pipe.GetShaderReflection(stage)
        parts.append("%s=shader%d(%s)" % (name, int(refl.resourceId) if refl else 0, shader_hash(refl)))
    srvs = []
    for used in pipe.GetReadOnlyResources(rd.ShaderStage.Pixel, True):
        tex = textures.get(int(used.descriptor.resource))
        if tex is not None:
            srvs.append("t%d=tex%d(%dx%d %s)" % (used.access.index, int(tex.resourceId), tex.width, tex.height,
                                                tex.format.Name()))
    if srvs:
        parts.append("srv{%s}" % ", ".join(srvs))
    blends = pipe.GetColorBlends()
    if blends:
        b = blends[0]
        parts.append("blend%s color(%s,%s,%s) alpha(%s,%s,%s) mask%x" % (
            "" if b.enabled else " off", b.colorBlend.source, b.colorBlend.destination, b.colorBlend.operation,
            b.alphaBlend.source, b.alphaBlend.destination, b.alphaBlend.operation, b.writeMask))
    return "  ".join(parts)


def scan(path, rect):
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(path, "", None) != rd.ResultCode.Succeeded:
        log("cannot open", path)
        return
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if result != rd.ResultCode.Succeeded:
        log("cannot replay", path, result)
        return
    log("=== %s" % path)
    textures = {int(t.resourceId): t for t in controller.GetTextures()}
    swap = [t for t in textures.values() if t.creationFlags & rd.TextureCategory.SwapBuffer]
    if not swap:
        log("no swap chain image")
        return
    width, height = swap[0].width, swap[0].height
    targets = [(t, 1) for t in swap]
    targets += [(t, 2) for t in textures.values()
                if t.width * 2 == width and t.height * 2 == height and t.creationFlags & rd.TextureCategory.ColorTarget
                and t.format.Name().startswith(("B8G8R8A8", "R8G8B8A8"))]
    x0, y0, x1, y1 = rect or (0, 0, width, height)
    found = {}
    for tex, scale in targets:
        sampled = 0
        for y in range(y0 + STEP // 2, y1, STEP):
            for x in range(x0 + STEP // 2, x1, STEP):
                px, py = x // scale, y // scale
                history = controller.PixelHistory(tex.resourceId, px, py, rd.Subresource(0, 0, 0),
                                                  rd.CompType.Typeless)
                sampled += 1
                for mod in history:
                    out = list(mod.shaderOut.col.floatValue)
                    if not bad(out):
                        continue
                    entry = found.setdefault((int(tex.resourceId), mod.eventId), {"pixels": [], "min": [1e30] * 4,
                                                                                  "max": [-1e30] * 4, "nan": 0})
                    entry["pixels"].append((px, py))
                    for i, v in enumerate(out):
                        if math.isnan(v):
                            entry["nan"] += 1
                        else:
                            entry["min"][i] = min(entry["min"][i], v)
                            entry["max"][i] = max(entry["max"][i], v)
        log("tex%d %dx%d %s: %d pixels sampled" % (int(tex.resourceId), tex.width, tex.height, tex.format.Name(),
                                                   sampled))
    for (tid, eid), entry in sorted(found.items(), key=lambda kv: kv[0][1]):
        pixels = entry["pixels"]
        xs, ys = [p[0] for p in pixels], [p[1] for p in pixels]
        log("event %d on tex%d: %d sampled pixels out of range, x %d-%d y %d-%d, min %s max %s nan %d" % (
            eid, tid, len(pixels), min(xs), max(xs), min(ys), max(ys),
            ["%.4g" % v for v in entry["min"]], ["%.4g" % v for v in entry["max"]], entry["nan"]))
        log("   " + describe_event(controller, textures, eid))
    if not found:
        log("no draw wrote values outside 0..1 at the sampled pixels")
    controller.Shutdown()
    cap.Shutdown()


try:
    rect_env = os.environ.get("P3R_RECT")
    rect = tuple(int(v) for v in rect_env.split(",")) if rect_env else None
    for capture in [p for p in os.environ["P3R_RDCS"].split(";") if p]:
        scan(capture, rect)
except Exception:
    log("FATAL " + traceback.format_exc())
finally:
    OUT.close()
    os._exit(0)
