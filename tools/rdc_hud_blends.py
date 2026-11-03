"""Finds HUD draws whose blending saturates an 8-bit render target, which a float target would not clamp.

Run inside RenderDoc's embedded Python (tools/rdc.sh tools/rdc_hud_blends.py) with:
  P3R_RDCS  ';'-separated .rdc captures
  P3R_OUT   output text file
  P3R_STEP  optional: grid step in pixels of the back buffer (default 32)
  P3R_RECT  optional: x0,y0,x1,y1 in back buffer pixels to sample (default: the whole screen)
tools/rdc_hud_outputs.py looks at what the shaders write; this looks at the blend. On an 8-bit target the blend result
is clamped to 0..1, so additive layers (or alpha that adds up) stop at 1; RenoDX turns those targets into float
ones (Upgrade_B8G8R8A8_TYPELESS), where the sum goes on and a later pass that uses it can come out too bright. For
each sampled pixel of the swap chain image and of the 8-bit targets of half its size, the pixel history's writes that
ended at 1.0 (or 0.0) in some channel are kept; then each such draw's blend state recomputes the blend without the
clamp, and draws whose unclamped result leaves 0..1 are listed with the shaders, blend state and values.
"""
import math
import os
import traceback

import renderdoc as rd

OUT = open(os.environ["P3R_OUT"], "w", encoding="utf-8")
STEP = int(os.environ.get("P3R_STEP", "32"))
EPS = 1.0 / 512
BM = rd.BlendMultiplier
BO = rd.BlendOperation


def log(*parts):
    OUT.write(" ".join(str(p) for p in parts) + "\n")
    OUT.flush()


def shader_hash(refl):
    if refl is None:
        return "-"
    raw = bytes(refl.rawBytes)
    pos = raw.find(b"HASH")
    return raw[pos + 12:pos + 28].hex() if pos >= 0 else raw[4:20].hex()


def factor(mult, src, dst, channel, blend_factor):
    """The blend factor for one channel (D3D's definitions; alpha factors use the alpha channel)."""
    a_src, a_dst = src[3], dst[3]
    table = {
        BM.Zero: 0.0, BM.One: 1.0,
        BM.SrcCol: src[channel], BM.InvSrcCol: 1.0 - src[channel],
        BM.DstCol: dst[channel], BM.InvDstCol: 1.0 - dst[channel],
        BM.SrcAlpha: a_src, BM.InvSrcAlpha: 1.0 - a_src,
        BM.DstAlpha: a_dst, BM.InvDstAlpha: 1.0 - a_dst,
        BM.SrcAlphaSat: 1.0 if channel == 3 else min(a_src, 1.0 - a_dst),
        BM.FactorRGB: blend_factor[channel], BM.InvFactorRGB: 1.0 - blend_factor[channel],
        BM.FactorAlpha: blend_factor[3], BM.InvFactorAlpha: 1.0 - blend_factor[3],
    }
    return table.get(mult)


def blend(state, src, dst, blend_factor):
    """The blend result without the final clamp, or None for factors or operations not covered here."""
    out = []
    for c in range(4):
        eq = state.alphaBlend if c == 3 else state.colorBlend
        fs = factor(eq.source, src, dst, c, blend_factor)
        fd = factor(eq.destination, src, dst, c, blend_factor)
        if fs is None or fd is None:
            return None
        s, d = src[c] * fs, dst[c] * fd
        op = eq.operation
        if op == BO.Add:
            out.append(s + d)
        elif op == BO.Subtract:
            out.append(s - d)
        elif op == BO.ReversedSubtract:
            out.append(d - s)
        elif op == BO.Minimum:
            out.append(min(src[c], dst[c]))
        elif op == BO.Maximum:
            out.append(max(src[c], dst[c]))
        else:
            return None
    return out


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
    # Phase 1 (the replay stays at the end of the frame, which pixel history needs): writes that ended clamped.
    clamped = {}
    for tex, scale in targets:
        sampled = 0
        for y in range(y0 + STEP // 2, y1, STEP):
            for x in range(x0 + STEP // 2, x1, STEP):
                px, py = x // scale, y // scale
                history = controller.PixelHistory(tex.resourceId, px, py, rd.Subresource(0, 0, 0),
                                                  rd.CompType.Typeless)
                sampled += 1
                for mod in history:
                    post = list(mod.postMod.col.floatValue)
                    if not any(v >= 1.0 - EPS or v <= EPS for v in post):
                        continue
                    clamped.setdefault((int(tex.resourceId), mod.eventId), []).append(
                        ((px, py), list(mod.shaderOut.col.floatValue), list(mod.preMod.col.floatValue), post))
        log("tex%d %dx%d %s: %d pixels sampled" % (int(tex.resourceId), tex.width, tex.height, tex.format.Name(),
                                                   sampled))
    # Phase 2: each candidate draw's blend state, the blend without the clamp.
    found = 0
    for (tid, eid), samples in sorted(clamped.items(), key=lambda kv: kv[0][1]):
        controller.SetFrameEvent(eid, False)
        pipe = controller.GetPipelineState()
        blends = pipe.GetColorBlends()
        if not blends or not blends[0].enabled:
            continue
        state = blends[0]
        blend_factor = list(pipe.GetBlendFactor()) if hasattr(pipe, "GetBlendFactor") else [1.0] * 4
        over = []
        unknown = 0
        for pixel, src, dst, post in samples:
            src_c = [min(max(v, 0.0), 1.0) for v in src]  # UNORM targets clamp the shader output before blending
            value = blend(state, src_c, dst, blend_factor)
            if value is None:
                unknown += 1
                continue
            if any(v > 1.0 + EPS or v < -EPS or math.isnan(v) for v in value):
                over.append((pixel, value, dst, src_c))
        if not over and not unknown:
            continue
        found += 1
        xs, ys = [p[0][0] for p in over], [p[0][1] for p in over]
        maxima = [max(o[1][c] for o in over) for c in range(4)] if over else []
        minima = [min(o[1][c] for o in over) for c in range(4)] if over else []
        vs = pipe.GetShaderReflection(rd.ShaderStage.Vertex)
        ps = pipe.GetShaderReflection(rd.ShaderStage.Pixel)
        log("event %d on tex%d: %d of %d clamped writes would leave 0..1 without the clamp%s%s" % (
            eid, tid, len(over), len(samples),
            (", x %d-%d y %d-%d, unclamped min %s max %s" % (
                min(xs), max(xs), min(ys), max(ys), ["%.3g" % v for v in minima], ["%.3g" % v for v in maxima]))
            if over else "", (", %d with blend factors not covered" % unknown) if unknown else ""))
        log("   VS=shader%d(%s) PS=shader%d(%s) blend color(%s,%s,%s) alpha(%s,%s,%s) mask%x" % (
            int(vs.resourceId) if vs else 0, shader_hash(vs), int(ps.resourceId) if ps else 0, shader_hash(ps),
            state.colorBlend.source, state.colorBlend.destination, state.colorBlend.operation,
            state.alphaBlend.source, state.alphaBlend.destination, state.alphaBlend.operation, state.writeMask))
        for pixel, value, dst, src in over[:3]:
            log("   at %s: src %s dst %s -> unclamped %s" % (pixel, ["%.3g" % v for v in src],
                                                             ["%.3g" % v for v in dst], ["%.3g" % v for v in value]))
    if not found:
        log("no blend left 0..1 at the sampled pixels")
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
