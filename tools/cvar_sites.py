"""Finds the code that registers or looks up a console variable in P3R.exe and disassembles around it.

  python tools/cvar_sites.py <path to P3R.exe> r.TemporalAASamples [more names]

Looks for `lea rdx, [rip+name]` (48 8D 15) pointing at the exact UTF-16 name in .rdata, in every executable section
(Denuvo renames them: the game's own code is in `.tls$`, not `.text`), as the add-on's FindConsoleVariable does, and
prints the instructions before and after each such load (capstone), so the pattern the add-on relies on can be
checked after a game patch.
"""
import struct
import sys

import capstone


def sections(data):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    count = struct.unpack_from("<H", data, pe + 6)[0]
    opt_size = struct.unpack_from("<H", data, pe + 20)[0]
    image_base = struct.unpack_from("<Q", data, pe + 24 + 24)[0]
    table = pe + 24 + opt_size
    out = {}
    for i in range(count):
        off = table + 40 * i
        name = data[off:off + 8].rstrip(b"\0").decode(errors="replace")
        vsize, va, raw_size, raw_ptr = struct.unpack_from("<IIII", data, off + 8)
        flags = struct.unpack_from("<I", data, off + 36)[0]
        out[name] = (va, vsize, raw_ptr, raw_size, flags)
    return image_base, out


def name_loads(code, code_va, targets):
    """Offsets in `code` of lea rdx, [rip+X] with X in `targets`."""
    hits = []
    pos = code.find(b"\x48\x8d\x15")
    while pos >= 0:
        if pos + 7 <= len(code):
            disp = struct.unpack_from("<i", code, pos + 3)[0]
            if code_va + pos + 7 + disp in targets:
                hits.append(pos)
        pos = code.find(b"\x48\x8d\x15", pos + 1)
    return hits


def main():
    path, names = sys.argv[1], sys.argv[2:]
    data = open(path, "rb").read()
    base, secs = sections(data)
    for k, v in secs.items():
        print("section %-8s va %#10x vsize %#10x raw %#10x rawsize %#10x flags %#010x" % ((k,) + v))
    rdata_va, rdata_vsize, rdata_raw, rdata_rawsize, _ = secs[".rdata"]
    rdata = data[rdata_raw:rdata_raw + min(rdata_vsize, rdata_rawsize)]
    code_sections = [(k, v) for k, v in secs.items() if v[4] & 0x20000000]
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    for name in names:
        needle = (name + "\0").encode("utf-16-le")
        targets = set()
        pos = rdata.find(needle)
        while pos >= 0:
            if pos % 2 == 0:
                targets.add(rdata_va + pos)
            pos = rdata.find(needle, pos + 2)
        print("== %s: string(s) at %s" % (name, [hex(t) for t in sorted(targets)]))
        total = 0
        for sec_name, (va, vsize, raw, rawsize, _) in code_sections:
            code = data[raw:raw + min(vsize, rawsize)]
            for pos in name_loads(code, va, targets):
                total += 1
                start = max(0, pos - 40)
                print("-- load in %s at rva %#x" % (sec_name, va + pos))
                for insn in md.disasm(code[start:pos + 48], base + va + start):
                    mark = "  <==" if insn.address == base + va + pos else ""
                    print("   %#x %-8s %s%s" % (insn.address - base, insn.mnemonic, insn.op_str, mark))
        print("   %d load(s)" % total)


main()
