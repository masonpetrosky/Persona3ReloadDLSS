"""Prints the CRC32 of the shaders bound at chosen events, the hash RenoDX uses to pick the shaders it replaces (its
log lists them as "Registered runtime replacement: 0x..."), and saves their bytecode.

Run inside RenderDoc's embedded Python (tools/rdc.sh tools/rdc_shader_crc.py) with:
  P3R_RDC   the .rdc capture
  P3R_EIDS  ','-separated events
  P3R_OUT   output folder (crc.log and <stage>_<crc>.bin)
"""
import os
import traceback
import zlib

import renderdoc as rd

OUT_DIR = os.environ["P3R_OUT"]
os.makedirs(OUT_DIR, exist_ok=True)
LOG = open(os.path.join(OUT_DIR, "crc.log"), "w", encoding="utf-8")


def log(*parts):
    LOG.write(" ".join(str(p) for p in parts) + "\n")
    LOG.flush()


try:
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(os.environ["P3R_RDC"], "", None) != rd.ResultCode.Succeeded:
        raise RuntimeError("cannot open the capture")
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if result != rd.ResultCode.Succeeded:
        raise RuntimeError("cannot replay: %s" % result)
    for eid in [int(e) for e in os.environ["P3R_EIDS"].split(",") if e.strip()]:
        controller.SetFrameEvent(eid, False)
        pipe = controller.GetPipelineState()
        for stage, name in ((rd.ShaderStage.Vertex, "vs"), (rd.ShaderStage.Pixel, "ps"),
                            (rd.ShaderStage.Compute, "cs")):
            refl = pipe.GetShaderReflection(stage)
            if refl is None:
                continue
            raw = bytes(refl.rawBytes)
            crc = zlib.crc32(raw) & 0xFFFFFFFF
            pos = raw.find(b"HASH")
            dxil = raw[pos + 12:pos + 28].hex() if pos >= 0 else "-"
            with open(os.path.join(OUT_DIR, "%s_%08x.bin" % (name, crc)), "wb") as f:
                f.write(raw)
            log("event %d %s: crc32 0x%08x, %d bytes, DXIL hash %s" % (eid, name, crc, len(raw), dxil))
    controller.Shutdown()
    cap.Shutdown()
except Exception:
    log("FATAL " + traceback.format_exc())
finally:
    LOG.close()
    os._exit(0)
