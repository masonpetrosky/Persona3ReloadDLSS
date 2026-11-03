"""The TAA dispatches of one or more captures, with the values DLSS needs, to compare consecutive frames.

Run inside RenderDoc's embedded Python (tools/rdc.sh tools/rdc_taa.py) with:
  P3R_RDCS      ';'-separated .rdc captures
  P3R_OUT       output text file
  P3R_TAA_HASH  optional: ','-separated DXIL hashes (default: the TAAStandalone permutation of docs/recon.md)
  P3R_FIND      optional: "1" matches by bindings instead of by hash, for a TAA permutation whose hash is not known
                yet: every dispatch that reads a velocity-like texture (R16G16B16A16_UNORM, wider than 1 pixel) and
                a D32S8 depth, printed with its hashes
Finds every dispatch of the TAAStandalone compute shader (by its DXIL hash, see docs/recon.md) and prints its
bindings and, from the constant buffers, bCameraCut, the plus weights, the view rect and the View uniform buffer's
TemporalAAJitter, ViewSizeAndInvSize, BufferSizeAndInvSize, PreExposure and ClipToPrevClip. Offsets are UE 4.27's
(the DXIL keeps the buffer layouts; docs/recon.md maps them). The $Globals offsets are those of the permutation in
docs/recon.md; another permutation may lay $Globals out differently (rdc_inspect.py shows it), while View is the same
struct everywhere.
"""
import os
import struct
import traceback

import renderdoc as rd

TAA_HASHES = set(os.environ.get("P3R_TAA_HASH", "203152694ac6f5d3b98e5464598f5669").split(","))
FIND = os.environ.get("P3R_FIND", "0") == "1"
VIEW_SIZE = 5220  # bytes of UE 4.27's FViewUniformShaderParameters in this game
OUT = open(os.environ["P3R_OUT"], "w", encoding="utf-8")


def log(*parts):
    OUT.write(" ".join(str(p) for p in parts) + "\n")
    OUT.flush()


def floats(data, off, n):
    return struct.unpack_from("<%df" % n, data, off)


def ints(data, off, n):
    return struct.unpack_from("<%di" % n, data, off)


def fmt(values):
    return "(" + ", ".join("%.7g" % v for v in values) + ")"


def shader_hash(refl):
    raw = bytes(refl.rawBytes)
    # A DXBC container starts with "DXBC" and its 16-byte digest; DXIL keeps its own hash in the HASH chunk.
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
    log("=== %s" % path)
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
        if FIND:
            read = [textures.get(int(u.descriptor.resource)) for u in
                    pipe.GetReadOnlyResources(rd.ShaderStage.Compute, True)]
            names = [t.format.Name() for t in read if t is not None]
            velocity = any(t is not None and t.width > 1 and t.format.Name() == "R16G16B16A16_UNORM" for t in read)
            if not (velocity and any("D32S8" in n or "R32G8X24" in n for n in names)):
                continue
        elif not (TAA_HASHES & hashes[key]):
            continue
        log("-- event %d dims=%s shader%d hashes %s" % (action.eventId, tuple(action.dispatchDimension), key,
                                                         sorted(hashes[key])))
        for used in pipe.GetReadOnlyResources(rd.ShaderStage.Compute, True):
            rid = int(used.descriptor.resource)
            tex = textures.get(rid)
            log("   t%d tex%d %s" % (used.access.index, rid,
                                    "%dx%d %s" % (tex.width, tex.height, tex.format.Name()) if tex else "?"))
        for used in pipe.GetReadWriteResources(rd.ShaderStage.Compute, True):
            rid = int(used.descriptor.resource)
            tex = textures.get(rid)
            log("   u%d tex%d %s" % (used.access.index, rid,
                                    "%dx%d %s" % (tex.width, tex.height, tex.format.Name()) if tex else "?"))
        blocks = {}
        for slot in range(len(refl.constantBlocks)):
            desc = pipe.GetConstantBlock(rd.ShaderStage.Compute, slot, 0).descriptor
            size = refl.constantBlocks[slot].byteSize
            blocks[refl.constantBlocks[slot].fixedBindNumber] = (
                bytes(controller.GetBufferData(desc.resource, desc.byteOffset, size)),
                "buf%d+%d" % (int(desc.resource), desc.byteOffset))
        for slot in range(len(refl.constantBlocks)):
            block = refl.constantBlocks[slot]
            log("   b%d %s: %d bytes, %d variables" % (block.fixedBindNumber, block.name, block.byteSize,
                                                     len(block.variables)))
        if 1 not in blocks or len(blocks[1][0]) < VIEW_SIZE:
            continue  # no View uniform buffer at b1: not a TAA pass
        if 0 in blocks and len(blocks[0][0]) >= 400:
            g, gwhere = blocks[0]
            log("   $Globals %s: HistoryPreExposureCorrection=%.7g CurrentFrameWeight=%.7g bCameraCut=%d" % (
                (gwhere,) + floats(g, 0, 2) + ints(g, 8, 1)))
            log("   PlusWeights=%s" % fmt([floats(g, 160 + 16 * i, 1)[0] for i in range(5)]))
            log("   ViewportUVToInputBufferUV=%s OutputViewportSize=%s OutputViewportRect=%s" % (
                fmt(floats(g, 240, 4)), fmt(floats(g, 368, 4)), fmt(floats(g, 384, 4))))
        v, vwhere = blocks[1]
        jitter = floats(v, 2016, 4)
        size = floats(v, 2080, 4)
        log("   View %s: TemporalAAJitter=%s -> pixels cur (%.4f, %.4f) prev (%.4f, %.4f)" % (
            vwhere, fmt(jitter), jitter[0] * size[0] * 0.5, -jitter[1] * size[1] * 0.5,
            jitter[2] * size[0] * 0.5, -jitter[3] * size[1] * 0.5))
        log("   ViewSizeAndInvSize=%s BufferSizeAndInvSize=%s ViewRectMin=%s" % (
            fmt(size), fmt(floats(v, 2112, 4)), fmt(floats(v, 2064, 4))))
        log("   NumSceneColorMSAASamples=%d PreExposure=%.7g OneOverPreExposure=%.7g" % (
            ints(v, 2160, 1) + floats(v, 2164, 2)))
        log("   ViewToClip row2=%s row3=%s" % (fmt(floats(v, 448 + 32, 4)), fmt(floats(v, 448 + 48, 4))))
        for r in range(4):
            log("   ClipToPrevClip[%d]=%s" % (r, fmt(floats(v, 1952 + 16 * r, 4))))
    controller.Shutdown()
    cap.Shutdown()


try:
    for capture in [p for p in os.environ["P3R_RDCS"].split(";") if p]:
        dump_capture(capture)
except Exception:
    log("FATAL " + traceback.format_exc())
finally:
    OUT.close()
    os._exit(0)
