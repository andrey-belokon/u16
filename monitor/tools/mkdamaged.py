#!/usr/bin/env python3
"""mkdamaged.py IMG - corrupt a volume the way a failure could (for testing fsck):
DIRTY = 1; a leaked cluster; a duplicate entry after an interrupted move; a size greater than
the allocated clusters; a non-zero free entry; a garbage address past the size in IND; names
that coincide when case is ignored (in /tmp: readme, ReadMe, README and readme2)."""
import struct, sys
sys.path.insert(0, __file__.rsplit('/', 1)[0])
import fs16tool as t

v = t.Vol(sys.argv[1])
# leaked cluster: the last cluster of the volume is marked as occupied
v.setbit(v.n - 1, 1)
v.free -= 1
# duplicate: a copy of the entry /data/hello.txt in a free entry of /tmp under a different name
src = v.lookup("/data/hello.txt")
tmp = v.lookup("/tmp")
c = v.bmap(tmp, 0) if tmp["size"] else None
if c is None:
    c = v.append_clu(tmp); v.zero_clu(c); tmp["size"] += t.CLU; v.store(tmp)
v.put_ent(c, 5, b"dup", src["size"], src["flags"], 0, src["addr"])
# names that differ only in case (FS16, sect. 7.1): fsck will rename ReadMe to ReadMe3
# (ReadMe2 is taken by the entry readme2 when case is ignored), README to README4
for k, nm in ((8, b"readme"), (9, b"ReadMe"), (10, b"README"), (11, b"readme2")):
    v.put_ent(c, k, nm, 0, 0, 0, [0] * 12)
# non-zero free entry
o = c * t.CLU + 7 * 64
v.img[o + 40] = 0x55
# size greater than allocated: /data/big - cluster 3 is not allocated
b = v.lookup("/data/big")
a = list(b["addr"]); lost = a[3]; a[3] = 0
v.put_ent(b["clu"], b["idx"], b["name"], b["size"], b["flags"], b["mtime"], a)
# garbage past the size in IND: /data/ind
i = v.lookup("/data/ind")
C = (i["size"] + t.CLU - 1) // t.CLU
v.w16(i["addr"][10] * t.CLU + (C - 10 + 3) * 2, 77)
v.flags |= 1
v.write_super()
v.save()
