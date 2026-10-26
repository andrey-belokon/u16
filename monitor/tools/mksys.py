#!/usr/bin/env python3
"""mksys.py - kernel file A:/sys/monitor.sys from the monitor image
("CPU16 Monitor: Disk Boot", section 5).

    mksys.py monitor.bin monitor.map monitor.sys

Sector 0 is the header: "CXMS", version 1, entry point (boot_entry), code segment length,
resident segment address and length. Then the code segment (addresses $0000..__image_end)
and the resident segment ($E000..resident_end), each padded with zeros to a whole sector.
"""
import re, struct, sys

def sym(mp, name):
    for line in open(mp, encoding='utf-8'):
        m = re.match(r'\$([0-9A-F]{4})\s+(?:\$[0-9A-F]{4}\s+)?\d+\s+\S+\s+' + re.escape(name) + r'\s', line)
        if m:
            return int(m.group(1), 16)
    raise SystemExit('mksys: no symbol in the map: ' + name)

def pad(b):
    return b + bytes(-len(b) % 512)

def main():
    if len(sys.argv) != 4:
        print(__doc__); return 2
    img = open(sys.argv[1], 'rb').read()
    mp = sys.argv[2]
    iend, rbeg, rend, entry = sym(mp, '__image_end'), 0xE000, sym(mp, 'resident_end'), sym(mp, 'boot_entry')
    if iend > 0x8000 or not (rbeg <= entry < rend):
        raise SystemExit('mksys: bad segment bounds')
    code = img[0:iend]
    res = img[rbeg:rend]
    hdr = b'CXMS' + struct.pack('<HHHHH', 1, entry, len(code), rbeg, len(res))
    open(sys.argv[3], 'wb').write(pad(hdr) + pad(code) + pad(res))
    return 0

sys.exit(main())
