# Scan a binary for ASCII and UTF-16LE strings matching a regex. Usage: strings_scan.py <file> <regex> [minlen]
import re, sys
path, pat = sys.argv[1], sys.argv[2]
minlen = int(sys.argv[3]) if len(sys.argv) > 3 else 6
sys.stdout.reconfigure(encoding='utf-8')
data = open(path, 'rb').read()
rx = re.compile(pat, re.I)
seen = set()
for m in re.finditer(rb'(?:[\x20-\x7e]\x00){%d,}' % minlen, data):
    s = m.group().decode('utf-16le')
    if rx.search(s) and s not in seen:
        seen.add(s); print('W %08x %s' % (m.start(), s))
for m in re.finditer(rb'[\x20-\x7e]{%d,}' % minlen, data):
    s = m.group().decode('ascii')
    if rx.search(s) and s not in seen:
        seen.add(s); print('A %08x %s' % (m.start(), s))
