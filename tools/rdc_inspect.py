"""Full state of chosen draws/dispatches/copies in a RenderDoc capture, for mapping the TAA pass.

Run inside RenderDoc's embedded Python (tools/rdc.sh tools/rdc_inspect.py) with:
  P3R_RDC   path to the .rdc capture
  P3R_OUT   output directory
  P3R_EIDS  comma-separated event IDs
  P3R_DISASM optional: "0" to skip the shader disassembly (it is the slow part)
For each event writes eid_<n>/state.txt (bindings with view formats and value ranges, root signature, constant
buffers as reflected and as raw float4/uint4 rows) and eid_<n>/shader_<stage>.txt (disassembly); for a copy, the
source and destination. Then exits the RenderDoc process.
"""
import os
import struct
import traceback

import renderdoc as rd

CAPTURE = os.environ["P3R_RDC"]
OUT_DIR = os.environ["P3R_OUT"]
EIDS = [int(x) for x in os.environ["P3R_EIDS"].split(",") if x.strip()]
DISASM = os.environ.get("P3R_DISASM", "1") != "0"
os.makedirs(OUT_DIR, exist_ok=True)
LOG = open(os.path.join(OUT_DIR, "inspect.log"), "a", encoding="utf-8")


def log(*parts):
    LOG.write(" ".join(str(p) for p in parts) + "\n")
    LOG.flush()


def ok(result):
    return getattr(result, "code", result) == rd.ResultCode.Succeeded


def attrs(obj):
    out = {}
    for name in dir(obj):
        if name.startswith("_") or name in ("this", "thisown"):
            continue
        try:
            value = getattr(obj, name)
        except Exception:
            continue
        if not callable(value):
            out[name] = value
    return out


def fmt_var(var, indent=0):
    pad = "  " * indent
    if var.members:
        lines = ["%s%s:" % (pad, var.name)]
        for member in var.members:
            lines.extend(fmt_var(member, indent + 1))
        return lines
    count = max(1, var.rows) * max(1, var.columns)
    if var.type in (rd.VarType.Float, rd.VarType.Half, rd.VarType.Double):
        values = ["%.6g" % v for v in var.value.f32v[:count]]
    elif var.type in (rd.VarType.SInt, rd.VarType.SShort, rd.VarType.SByte, rd.VarType.SLong):
        values = [str(v) for v in var.value.s32v[:count]]
    else:
        values = [str(v) for v in var.value.u32v[:count]]
    return ["%s%s (%dx%d %s) = %s" % (pad, var.name, var.rows, var.columns, var.type, ", ".join(values))]


def raw_rows(data):
    """One line per 16 bytes: byte offset, four floats, four uints."""
    lines = []
    for off in range(0, len(data) - len(data) % 16, 16):
        f = struct.unpack_from("<4f", data, off)
        u = struct.unpack_from("<4I", data, off)
        lines.append("    +%5d  %14.7g %14.7g %14.7g %14.7g   %08x %08x %08x %08x" % ((off,) + f + u))
    return lines


def texture_stats(controller, rid, view_format):
    try:
        comp = view_format.compType if view_format is not None else rd.CompType.Typeless
        lo, hi = controller.GetMinMax(rid, rd.Subresource(0, 0, 0), comp)
        return "min=(%s) max=(%s)" % (", ".join("%.5g" % v for v in lo.floatValue),
                                      ", ".join("%.5g" % v for v in hi.floatValue))
    except Exception as exc:
        return "minmax failed: %s" % exc


def inspect(controller, eid, textures, buffers, names, targets, actions):
    out = os.path.join(OUT_DIR, "eid_%d" % eid)
    os.makedirs(out, exist_ok=True)
    controller.SetFrameEvent(eid, True)
    action = actions.get(eid)
    lines = ["event %d %s" % (eid, action.GetName(controller.GetStructuredFile()) if action else "?")]

    def describe(rid):
        key = int(rid)
        tex = textures.get(key)
        if tex is not None:
            return "tex%d %dx%d %s mips=%d arr=%d '%s'" % (key, tex.width, tex.height, tex.format.Name(),
                                                          tex.mips, tex.arraysize, names.get(key, ""))
        buf = buffers.get(key)
        if buf is not None:
            return "buf%d %d bytes '%s'" % (key, buf.length, names.get(key, ""))
        return "res%d '%s'" % (key, names.get(key, ""))

    if action is not None and action.flags & (rd.ActionFlags.Copy | rd.ActionFlags.Resolve | rd.ActionFlags.Clear):
        lines.append("copy source=%s sub=%s" % (describe(action.copySource), attrs(action.copySourceSubresource)))
        lines.append("copy destination=%s sub=%s" % (describe(action.copyDestination),
                                                     attrs(action.copyDestinationSubresource)))
        with open(os.path.join(out, "state.txt"), "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
        log("inspected copy", eid)
        return

    pipe = controller.GetPipelineState()
    compute = bool(action.flags & rd.ActionFlags.Dispatch) if action is not None else False
    if action is not None and compute:
        lines.append("dispatch dims=%s threads=%s" % (tuple(action.dispatchDimension),
                                                     tuple(action.dispatchThreadsDimension)))
    stages = [rd.ShaderStage.Compute] if compute else [rd.ShaderStage.Vertex, rd.ShaderStage.Pixel]
    pipeline_obj = pipe.GetComputePipelineObject() if compute else pipe.GetGraphicsPipelineObject()
    if not compute:
        try:
            vp = pipe.GetViewport(0)
            lines.append("viewport x=%g y=%g w=%g h=%g" % (vp.x, vp.y, vp.width, vp.height))
        except Exception as exc:
            lines.append("viewport unavailable: %s" % exc)

    root = controller.GetD3D12PipelineState().rootSignature
    lines.append("root signature %s" % root.resourceId)
    for i, param in enumerate(root.parameters):
        fields = attrs(param)
        ranges = fields.pop("tableRanges", None) or []
        lines.append("  param[%d] %s" % (i, {k: str(v)[:120] for k, v in fields.items()}))
        for j, rng in enumerate(ranges):
            lines.append("     range[%d] %s" % (j, {k: str(v) for k, v in attrs(rng).items()}))

    for stage in stages:
        shader = pipe.GetShader(stage)
        refl = pipe.GetShaderReflection(stage)
        entry = pipe.GetShaderEntryPoint(stage)
        lines.append("")
        lines.append("== %s shader%d entry=%s" % (stage, int(shader), entry))
        if refl is not None:
            lines.append("  reflection: %d SRV, %d UAV, %d CB, %d samplers; debug name '%s'" % (
                len(refl.readOnlyResources), len(refl.readWriteResources), len(refl.constantBlocks),
                len(refl.samplers), refl.debugInfo.files[0].filename if refl.debugInfo.files else ""))
            for kind, items in (("SRV", refl.readOnlyResources), ("UAV", refl.readWriteResources)):
                for i, res in enumerate(items):
                    lines.append("    refl %s[%d] %s t/u%d space%d type=%s" % (
                        kind, i, res.name, res.fixedBindNumber, res.fixedBindSetOrSpace, res.textureType))
            for i, block in enumerate(refl.constantBlocks):
                lines.append("    refl CB[%d] %s b%d space%d %d bytes, %d variables" % (
                    i, block.name, block.fixedBindNumber, block.fixedBindSetOrSpace, block.byteSize,
                    len(block.variables)))
        for title, used_list, refl_list in (
                ("SRV", pipe.GetReadOnlyResources(stage, True), refl.readOnlyResources if refl else []),
                ("UAV", pipe.GetReadWriteResources(stage, True), refl.readWriteResources if refl else [])):
            for used in used_list:
                idx = used.access.index
                label = refl_list[idx].name if 0 <= idx < len(refl_list) else "#%d" % idx
                desc = used.descriptor
                rid = desc.resource
                stats = texture_stats(controller, rid, desc.format) if int(rid) in textures else ""
                lines.append("  %s %-16s %s  view=%s mip=%d+%d slice=%d+%d  %s" % (
                    title, label, describe(rid), desc.format.Name(), desc.firstMip, desc.numMips,
                    desc.firstSlice, desc.numSlices, stats))
        if refl is not None:
            for slot, block in enumerate(refl.constantBlocks):
                try:
                    desc = pipe.GetConstantBlock(stage, slot, 0).descriptor
                    lines.append("  CBV %s (slot %d b%d, %s +%d, %d bytes bound, %d reflected)" % (
                        block.name, slot, block.fixedBindNumber, describe(desc.resource), desc.byteOffset,
                        desc.byteSize, block.byteSize))
                    variables = controller.GetCBufferVariableContents(
                        pipeline_obj, shader, stage, entry, slot, desc.resource, desc.byteOffset, desc.byteSize)
                    offsets = {v.name: v.byteOffset for v in block.variables}
                    for var in variables:
                        lines.append("   @%s" % offsets.get(var.name, "?"))
                        lines.extend(fmt_var(var, 2))
                    size = block.byteSize or min(desc.byteSize, 4096)
                    if desc.resource != rd.ResourceId.Null():
                        data = bytes(controller.GetBufferData(desc.resource, desc.byteOffset, size))
                        lines.append("   raw %d bytes:" % len(data))
                        lines.extend(raw_rows(data))
                except Exception as exc:
                    lines.append("  CBV %s (slot %d) failed: %s" % (block.name, slot, exc))
        if DISASM and refl is not None:
            for target in targets:
                try:
                    text = controller.DisassembleShader(pipeline_obj, refl, target)
                    safe = "".join(c if c.isalnum() else "_" for c in target)[:40]
                    with open(os.path.join(out, "shader_%s_%s.txt" % (str(stage).split(".")[-1], safe)), "w",
                              encoding="utf-8") as f:
                        f.write(text)
                except Exception as exc:
                    lines.append("  disassembly (%s) failed: %s" % (target, exc))
    if not compute:
        for i, desc in enumerate(pipe.GetOutputTargets()):
            if desc.resource != rd.ResourceId.Null():
                lines.append("  RT%d %s view=%s" % (i, describe(desc.resource), desc.format.Name()))
        depth = pipe.GetDepthTarget()
        if depth.resource != rd.ResourceId.Null():
            lines.append("  DSV %s view=%s" % (describe(depth.resource), depth.format.Name()))
    with open(os.path.join(out, "state.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    log("inspected", eid)


def main():
    cap = rd.OpenCaptureFile()
    if not ok(cap.OpenFile(CAPTURE, "", None)):
        log("OpenFile failed")
        return
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if not ok(result):
        log("OpenCapture failed:", result)
        return
    textures = {int(t.resourceId): t for t in controller.GetTextures()}
    buffers = {int(b.resourceId): b for b in controller.GetBuffers()}
    names = {int(r.resourceId): r.name for r in controller.GetResources()}
    targets = list(controller.GetDisassemblyTargets(True))
    actions = {}

    def walk(items):
        for a in items:
            actions[a.eventId] = a
            walk(a.children)

    walk(controller.GetRootActions())
    log("capture", CAPTURE, "disassembly targets:", targets)
    for eid in EIDS:
        try:
            inspect(controller, eid, textures, buffers, names, targets, actions)
        except Exception:
            log("event", eid, "failed", traceback.format_exc())
    controller.Shutdown()
    cap.Shutdown()


try:
    main()
except Exception:
    log("FATAL", traceback.format_exc())
finally:
    LOG.close()
    os._exit(0)
