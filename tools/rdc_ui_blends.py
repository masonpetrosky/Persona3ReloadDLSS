"""Lists the blend state of every draw from a given event on (the HUD), to see which draws write the back buffer's
alpha as a mask and which read it back through the blend (DstAlpha / InvDstAlpha).

Run inside RenderDoc's embedded Python (tools/rdc.sh tools/rdc_ui_blends.py) with:
  P3R_RDCS   ';'-separated .rdc captures
  P3R_OUT    output text file
  P3R_FROM   optional: first event to list (default 0)
One line per draw: event, render target, color write mask, color and alpha blend equations, shader hashes, and a tag:
MASK for a draw that changes only alpha with an adding or subtracting blend, READS-ALPHA for a draw whose color blend
uses the destination's alpha. Then the counts per shader pair and tag.
"""
import os
import traceback

import renderdoc as rd

OUT = open(os.environ["P3R_OUT"], "w", encoding="utf-8")
FROM = int(os.environ.get("P3R_FROM", "0"))
BM = rd.BlendMultiplier
DST_ALPHA = (BM.DstAlpha, BM.InvDstAlpha)


def log(*parts):
    OUT.write(" ".join(str(p) for p in parts) + "\n")
    OUT.flush()


def shader_hash(refl):
    if refl is None:
        return "-"
    raw = bytes(refl.rawBytes)
    pos = raw.find(b"HASH")
    return (raw[pos + 12:pos + 28].hex() if pos >= 0 else raw[4:20].hex())[:8]


def short(mult):
    return str(mult).split(".")[-1]


def scan(path):
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
    draws = []

    def walk(items):
        for a in items:
            if a.flags & rd.ActionFlags.Drawcall and a.eventId >= FROM:
                draws.append(a)
            walk(a.children)

    walk(controller.GetRootActions())
    counts = {}
    for action in draws:
        controller.SetFrameEvent(action.eventId, False)
        pipe = controller.GetPipelineState()
        targets = [t for t in pipe.GetOutputTargets() if t.resource != rd.ResourceId.Null()]
        if not targets:
            continue
        tex = textures.get(int(targets[0].resource))
        blends = pipe.GetColorBlends()
        b = blends[0] if blends else None
        tag = ""
        if b is not None and b.enabled:
            color_untouched = b.colorBlend.source == BM.Zero and b.colorBlend.destination == BM.One
            alpha_changes = not (b.alphaBlend.source == BM.Zero and b.alphaBlend.destination == BM.One)
            if (color_untouched or (b.writeMask & 0x7) == 0) and alpha_changes:
                tag = "MASK"
            if b.colorBlend.source in DST_ALPHA or b.colorBlend.destination in DST_ALPHA:
                tag = (tag + " READS-ALPHA").strip()
        vs = shader_hash(pipe.GetShaderReflection(rd.ShaderStage.Vertex))
        ps = shader_hash(pipe.GetShaderReflection(rd.ShaderStage.Pixel))
        if b is None:
            eq = "no blend state"
        elif not b.enabled:
            eq = "blend off"
        else:
            eq = "color %s*src %s %s*dst, alpha %s*src %s %s*dst" % (
                short(b.colorBlend.source), short(b.colorBlend.operation), short(b.colorBlend.destination),
                short(b.alphaBlend.source), short(b.alphaBlend.operation), short(b.alphaBlend.destination))
        log("%6d %-10s tex%d %s mask%x  %s  VS %s PS %s  %s" % (
            action.eventId, tag or "-", int(targets[0].resource),
            "%dx%d %s" % (tex.width, tex.height, tex.format.Name()) if tex else "?",
            b.writeMask if b is not None else 0, eq, vs, ps, action.numIndices))
        key = (vs, ps, tag or "-", eq)
        counts[key] = counts.get(key, 0) + 1
    log("--- per shader pair and blend")
    for (vs, ps, tag, eq), n in sorted(counts.items(), key=lambda kv: -kv[1]):
        log("%5d  VS %s PS %s  %-10s %s" % (n, vs, ps, tag, eq))
    controller.Shutdown()
    cap.Shutdown()


try:
    for capture in [p for p in os.environ["P3R_RDCS"].split(";") if p]:
        scan(capture)
except Exception:
    log("FATAL " + traceback.format_exc())
finally:
    OUT.close()
    os._exit(0)
