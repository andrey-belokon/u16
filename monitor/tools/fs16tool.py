#!/usr/bin/env python3
"""fs16tool.py - host utility for FS16 volumes ("FS16 File System").

  fs16tool.py mkfs IMG SECTORS [--label L] [--serial N]
  fs16tool.py build IMG SPEC.json          create a volume and put the SPEC tree on it
  fs16tool.py boot IMG CODE.bin            write boot code to sectors 1-3 (up to 1536 bytes)
  fs16tool.py check IMG [--allow-dirty]    check the volume (section 8), no repairs
  fs16tool.py ls IMG [PATH] [-R]           list a directory
  fs16tool.py clean IMG                    clear DIRTY
  fs16tool.py get IMG PATH                 file contents to stdout
  fs16tool.py sums IMG                     checksums of all files
  fs16tool.py verify IMG SPEC.json         SPEC files on the volume match the generator
  fs16tool.py b16 SPEC.json                table of expected values for a B16 test

SPEC.json: {"sectors": N, "label": "...", "entries": [
   {"path": "/a/b", "dir": true},
   {"path": "/a/f", "size": 2049, "seed": 3, "flags": ["RDONLY"], "hint": 20000},
   {"path": "/sys/shell.cx", "src": "out/tsys.bin"}, ...], "dirty": false}
"src" - contents from a host file; "dirty": true - volume with DIRTY = 1;
"boot": "file" - boot code for sectors 1-3.
Otherwise file contents come from the generator gen_byte(seed, pos), the same as in the B16 test.
"""
import json, struct, sys

def fold(name):
    """Name bytes for comparison: letter case is ignored (FS16 section 7.1): A-Z, Cyrillic capitals, capital Yo."""
    return bytes(c + 0x20 if 0x41 <= c <= 0x5A or 0xC0 <= c <= 0xDF else 0xA3 if c == 0xB3 else c for c in name)


SEC = 512
CLU = 2048
F_DIR, F_RDONLY, F_HIDDEN, F_SYSTEM = 0x8000, 0x4000, 0x2000, 0x1000
FLAGNAMES = {"DIR": F_DIR, "RDONLY": F_RDONLY, "HIDDEN": F_HIDDEN, "SYSTEM": F_SYSTEM}


def gen_byte(seed, pos):
    lo, hi = pos & 0xFFFF, pos >> 16
    v = (lo + (lo >> 8) * 3 + hi * 101 + seed * 17) & 0xFFFF
    return (v ^ (v >> 5)) & 0xFF


def gen_data(seed, size):
    return bytes(gen_byte(seed, p) for p in range(size))


def fsum(data):
    """Checksum: a += byte, b += a, both modulo 2^16."""
    a = b = 0
    for x in data:
        a = (a + x) & 0xFFFF
        b = (b + a) & 0xFFFF
    return a, b


class Vol:
    def __init__(self, path):
        self.path = path
        self.img = bytearray(open(path, "rb").read())
        self.sectors = len(self.img) // SEC
        sb = self.sec(0)
        self.magic = bytes(sb[0:4])
        self.version, self.n, self.b, self.free, self.hint, self.flags = struct.unpack_from("<6H", sb, 4)

    # ---- low level
    def sec(self, lba):
        return memoryview(self.img)[lba * SEC:(lba + 1) * SEC]

    def clu(self, c):
        return memoryview(self.img)[c * CLU:(c + 1) * CLU]

    def w16(self, off, v):
        struct.pack_into("<H", self.img, off, v)

    def r16(self, off):
        return struct.unpack_from("<H", self.img, off)[0]

    def save(self):
        open(self.path, "wb").write(self.img)

    def bit(self, c):
        k = c >> 3
        return (self.img[4 * SEC + k] >> (c & 7)) & 1

    def setbit(self, c, v):
        k = 4 * SEC + (c >> 3)
        if v:
            self.img[k] |= 1 << (c & 7)
        else:
            self.img[k] &= ~(1 << (c & 7)) & 0xFF

    def fix_checksum(self):
        self.w16(510, 0)
        s = sum(struct.unpack_from("<256H", self.img, 0)) & 0xFFFF
        self.w16(510, (-s) & 0xFFFF)

    def write_super(self):
        struct.pack_into("<6H", self.img, 4, self.version, self.n, self.b, self.free, self.hint, self.flags)
        self.fix_checksum()

    # ---- entries
    @staticmethod
    def ent_off(clu, idx):
        return 36 if clu == 0 else clu * CLU + idx * 64

    def ent(self, clu, idx):
        o = self.ent_off(clu, idx)
        raw = bytes(self.img[o:o + 64])
        name = raw[0:32].rstrip(b"\0")
        size = struct.unpack_from("<I", raw, 32)[0]
        mt = struct.unpack_from("<I", raw, 36)[0]
        addr = list(struct.unpack_from("<12H", raw, 40))
        return {"raw": raw, "name": name, "size": size & 0x07FFFFFF, "flags": (size >> 16) & 0xF800,
                "mtime": mt, "addr": addr, "clu": clu, "idx": idx}

    def put_ent(self, clu, idx, name, size, flags, mtime, addr):
        o = self.ent_off(clu, idx)
        nb = name.ljust(32, b"\0")
        assert len(nb) == 32
        self.img[o:o + 32] = nb
        struct.pack_into("<II", self.img, o + 32, size | (flags << 16), mtime)
        struct.pack_into("<12H", self.img, o + 40, *addr)
        if clu == 0:
            self.fix_checksum()

    def idx(self, c, i):
        return self.r16(c * CLU + i * 2)

    def bmap(self, e, k):
        a = e["addr"]
        if k < 10:
            return a[k]
        if k < 1034:
            return self.idx(a[10], k - 10) if a[10] else 0
        j = k - 1034
        if not a[11]:
            return 0
        t = self.idx(a[11], j >> 10)
        return self.idx(t, j & 1023) if t else 0

    def read_file(self, e):
        out = bytearray()
        size = e["size"]
        k = 0
        while len(out) < size:
            c = self.bmap(e, k)
            out += self.clu(c)[:min(CLU, size - len(out))]
            k += 1
        return bytes(out)

    def entries(self, d):
        """The occupied directory entries d (by entry descriptor)."""
        res = []
        for k in range((d["size"] + CLU - 1) // CLU):
            c = self.bmap(d, k)
            for i in range(32):
                if self.img[c * CLU + i * 64] != 0:
                    res.append(self.ent(c, i))
        return res

    def root(self):
        return self.ent(0, 0)

    def lookup(self, path):
        e = self.root()
        for part in [p for p in path.split("/") if p]:
            if not e["flags"] & F_DIR:
                raise KeyError(path)
            for x in self.entries(e):
                if fold(x["name"]) == fold(part.encode("cp1251")):
                    e = x
                    break
            else:
                raise KeyError(path)
        return e

    # ---- writing (for build)
    def alloc(self):
        first = self.b + 1
        h = self.hint if first <= self.hint < self.n else first
        for rng in (range(h, self.n), range(first, h)):
            for c in rng:
                if not self.bit(c):
                    self.setbit(c, 1)
                    self.free -= 1
                    self.hint = c + 1
                    return c
        raise RuntimeError("disk full")

    def set_idx(self, c, i, v):
        self.w16(c * CLU + i * 2, v)

    def zero_clu(self, c):
        self.img[c * CLU:(c + 1) * CLU] = bytes(CLU)

    def append_clu(self, e):
        """Append cluster k = C to the end (the descriptor e changes, the entry is not written)."""
        k = (e["size"] + CLU - 1) // CLU
        c = self.alloc()
        a = e["addr"]
        if k < 10:
            a[k] = c
        elif k < 1034:
            if k == 10:
                a[10] = self.alloc()
                self.zero_clu(a[10])
            self.set_idx(a[10], k - 10, c)
        else:
            j = k - 1034
            if k == 1034:
                a[11] = self.alloc()
                self.zero_clu(a[11])
            if j & 1023 == 0:
                t = self.alloc()
                self.zero_clu(t)
                self.set_idx(a[11], j >> 10, t)
            t = self.idx(a[11], j >> 10)
            self.set_idx(t, j & 1023, c)
        return c

    def store(self, e):
        self.put_ent(e["clu"], e["idx"], e["name"], e["size"], e["flags"], e["mtime"], e["addr"])

    def create(self, path, flags, mtime=0):
        parts = [p for p in path.split("/") if p]
        d = self.lookup("/".join(parts[:-1]))
        name = parts[-1].encode("cp1251")
        if any(fold(x["name"]) == fold(name) for x in self.entries(d)):
            raise RuntimeError("%s: already exists (names ignore letter case)" % path)
        slot = None
        for k in range(d["size"] // CLU):
            c = self.bmap(d, k)
            for i in range(32):
                if self.img[c * CLU + i * 64] == 0:
                    slot = (c, i)
                    break
            if slot:
                break
        if not slot:
            c = self.append_clu(d)
            self.zero_clu(c)
            d["size"] += CLU
            self.store(d)
            slot = (c, 0)
        self.put_ent(slot[0], slot[1], name, 0, flags, mtime, [0] * 12)
        return self.ent(*slot)

    def write_file(self, e, data):
        pos = 0
        while pos < len(data):
            c = self.append_clu(e)
            chunk = data[pos:pos + CLU]
            self.img[c * CLU:c * CLU + len(chunk)] = chunk
            pos += len(chunk)
            e["size"] = pos
        self.store(e)


def mkfs(path, sectors, label="FS16", serial=0x12345678):
    n = min(sectors // 4, 65535)
    b = (n + 16383) // 16384
    img = bytearray(sectors * SEC)
    struct.pack_into("<4s6HI16s", img, 0, b"FS16", 1, n, b, 0, b + 1, 0, serial, label.encode().ljust(16, b"\0"))
    # root: name - zeros, DIR flag, size 0
    struct.pack_into("<I", img, 36 + 32, F_DIR << 16)
    used = 1 + b
    for c in list(range(0, 1 + b)) + list(range(n, 16384 * b)):
        img[4 * SEC + (c >> 3)] |= 1 << (c & 7)
    free = n - used
    struct.pack_into("<H", img, 10, free)
    open(path, "wb").write(img)
    v = Vol(path)
    v.fix_checksum()
    v.save()


def build(path, spec):
    mkfs(path, spec["sectors"], spec.get("label", "FS16"))
    v = Vol(path)
    for e in spec["entries"]:
        flags = 0
        for f in e.get("flags", []):
            flags |= FLAGNAMES[f]
        if "hint" in e:
            v.hint = e["hint"]
        if e.get("dir"):
            v.create(e["path"], flags | F_DIR, e.get("mtime", 0))
        else:
            x = v.create(e["path"], flags, e.get("mtime", 0))
            if "src" in e:
                v.write_file(x, open(e["src"], "rb").read())
            else:
                v.write_file(x, gen_data(e.get("seed", 0), e["size"]))
    if spec.get("dirty"):
        v.flags |= 1
    v.write_super()
    v.save()
    if "boot" in spec:
        write_boot(path, spec["boot"])


def write_boot(path, code_path):
    code = open(code_path, "rb").read()
    if len(code) > 3 * SEC:
        raise SystemExit("fs16tool: boot code is longer than 1536 bytes")
    img = bytearray(open(path, "rb").read())
    img[SEC:4 * SEC] = code + bytes(3 * SEC - len(code))
    open(path, "wb").write(img)


def check(path, allow_dirty=False):
    """Check against sect. 3 and 8. Returns the list of errors."""
    errs = []
    v = Vol(path)
    if v.magic != b"FS16" or v.version != 1:
        return ["superblock: MAGIC or VERSION"], 0
    if sum(struct.unpack_from("<256H", v.img, 0)) & 0xFFFF:
        errs.append("superblock: CHECKSUM")
    if 4 * v.n > v.sectors or v.b != (v.n + 16383) // 16384 or v.n < 3:
        errs.append("superblock: CLUSTERS/MAPLEN")
    if v.flags & 1 and not allow_dirty:
        errs.append("superblock: DIRTY = 1")
    root = v.root()
    if not root["flags"] & F_DIR or root["name"]:
        errs.append("ROOT: no DIR flag or name not empty")
    ref = {}

    def mark(c, who):
        if not (v.b + 1 <= c < v.n):
            errs.append(f"{who}: address {c} outside the data area")
        elif c in ref:
            errs.append(f"{who}: cluster {c} already used ({ref[c]})")
        else:
            ref[c] = who

    def walk_node(e, who):
        size = e["size"]
        C = (size + CLU - 1) // CLU
        a = e["addr"]
        if e["flags"] & F_DIR and size % CLU:
            errs.append(f"{who}: directory size is not a multiple of 2048")
        for k in range(10):
            if (k < C) != (a[k] != 0):
                errs.append(f"{who}: DIRECT[{k}] = {a[k]} with C = {C}")
            if a[k]:
                mark(a[k], who)
        if (C > 10) != (a[10] != 0):
            errs.append(f"{who}: IND = {a[10]} with C = {C}")
        if a[10]:
            mark(a[10], who + "/IND")
            for i in range(1024):
                x = v.idx(a[10], i)
                if (10 + i < C) != (x != 0):
                    errs.append(f"{who}: IND[{i}] = {x} with C = {C}")
                    break
            for i in range(min(1024, max(0, C - 10))):
                x = v.idx(a[10], i)
                if x:
                    mark(x, who)
        if (C > 1034) != (a[11] != 0):
            errs.append(f"{who}: DIND = {a[11]} with C = {C}")
        if a[11]:
            mark(a[11], who + "/DIND")
            nl2 = (C - 1034 + 1023) // 1024
            for j in range(1024):
                t = v.idx(a[11], j)
                if (j < nl2) != (t != 0):
                    errs.append(f"{who}: DIND[{j}] = {t}, {nl2} entries needed")
                if t and j < nl2:
                    mark(t, who + f"/L2[{j}]")
                    for i in range(1024):
                        x = v.idx(t, i)
                        kk = 1034 + j * 1024 + i
                        if (kk < C) != (x != 0):
                            errs.append(f"{who}: L2[{j}][{i}] = {x} with C = {C}")
                            break
                        if x:
                            mark(x, who)
        if e["flags"] & F_DIR:
            names = set()
            for k in range(C):
                c = v.bmap(e, k)
                if not c:
                    continue
                for i in range(32):
                    raw = bytes(v.img[c * CLU + i * 64:c * CLU + i * 64 + 64])
                    if raw[0] == 0:
                        if any(raw):
                            errs.append(f"{who}: free entry {c}:{i} is not zero")
                        continue
                    x = v.ent(c, i)
                    nm = x["name"]
                    if (len(nm) < 1 or any(ch < 0x20 or ch == 0x2F for ch in nm) or nm in (b".", b"..")
                            or b"\0" in raw[:32].rstrip(b"\0")):
                        errs.append(f"{who}: invalid name {nm!r}")
                    if fold(nm) in names:
                        errs.append(f"{who}: duplicate name {nm!r} (names ignore letter case)")
                    names.add(fold(nm))
                    if (x["flags"] >> 11) & 1:
                        errs.append(f"{who}/{nm.decode('cp1251')}: SIZE bit 27 is not 0")
                    stack.append((x, who.rstrip("/") + "/" + nm.decode("cp1251", "replace")))

    stack = [(root, "/")]
    while stack:
        e, who = stack.pop()
        walk_node(e, who)
    nfree = 0
    for c in range(16384 * v.b):
        must = c <= v.b or c >= v.n or c in ref
        b = v.bit(c)
        if not b:
            nfree += 1
        if b != must:
            errs.append(f"map: cluster {c} bit {b}, must be {int(must)}"
                        + (" (leak)" if b else " (used, but free in the map)"))
            if len(errs) > 50:
                break
    if nfree != v.free:
        msg = f"FREE = {v.free}, free {nfree}"
        if v.flags & 1 and allow_dirty:
            print("  (hint is stale, volume was not unmounted) " + msg)
        else:
            errs.append(msg)
    return errs, nfree


def tree(v, e, prefix, rec, out):
    for x in sorted(v.entries(e), key=lambda x: x["name"]):
        nm = x["name"].decode("cp1251", "replace")
        fl = "".join(ch for ch, m in (("d", F_DIR), ("r", F_RDONLY), ("h", F_HIDDEN), ("s", F_SYSTEM))
                     if x["flags"] & m)
        out.append(f"{fl:4} {x['size']:9}  {prefix}{nm}")
        if rec and x["flags"] & F_DIR:
            tree(v, x, prefix + nm + "/", rec, out)


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__)
        return 2
    cmd = a[0]
    if cmd == "mkfs":
        lab = a[a.index("--label") + 1] if "--label" in a else "FS16"
        mkfs(a[1], int(a[2]), lab)
    elif cmd == "build":
        build(a[1], json.load(open(a[2])))
    elif cmd == "boot":
        write_boot(a[1], a[2])
    elif cmd == "check":
        errs, _ = check(a[1], "--allow-dirty" in a)
        v = Vol(a[1])
        for e in errs:
            print("  " + e)
        print(f"{a[1]}: N={v.n} B={v.b} FREE={v.free} HINT={v.hint} DIRTY={v.flags & 1}: "
              + ("consistent" if not errs else f"{len(errs)} errors"))
        return 1 if errs else 0
    elif cmd == "ls":
        v = Vol(a[1])
        p = a[2] if len(a) > 2 and not a[2].startswith("-") else "/"
        out = []
        tree(v, v.lookup(p), "", "-R" in a, out)
        print("\n".join(out))
    elif cmd == "clean":                      # clear DIRTY: check, recompute FREE (sect. 10, step 4)
        errs, nfree = check(a[1], True)
        if errs:
            print("\n".join(errs))
            return 1
        v = Vol(a[1])
        v.free = nfree
        v.flags &= ~1
        v.write_super()
        v.save()
    elif cmd == "get":
        v = Vol(a[1])
        sys.stdout.buffer.write(v.read_file(v.lookup(a[2])))
    elif cmd == "sums":
        v = Vol(a[1])
        stack = [("", v.root())]
        while stack:
            p, d = stack.pop()
            for x in v.entries(d):
                nm = p + "/" + x["name"].decode("cp1251")
                if x["flags"] & F_DIR:
                    stack.append((nm, x))
                else:
                    s = fsum(v.read_file(x))
                    print(f"{x['size']:9} {s[0]:04X} {s[1]:04X}  {nm}")
    elif cmd == "verify":
        v = Vol(a[1])
        spec = json.load(open(a[2]))
        bad = 0
        for e in spec["entries"]:
            if e.get("dir"):
                continue
            try:
                x = v.lookup(e["path"])
            except KeyError:
                print("no file", e["path"])
                bad += 1
                continue
            data = v.read_file(x)
            if data != gen_data(e.get("seed", 0), e["size"]):
                print("contents differ:", e["path"], len(data), e["size"])
                bad += 1
        print(f"verify: {len(spec['entries'])} entries, {bad} mismatches")
        return 1 if bad else 0
    elif cmd == "b16":
        spec = json.load(open(a[1]))
        files = [e for e in spec["entries"] if not e.get("dir")]
        print("// generated by fs16tool.py b16 - expected files of the test volume")
        print(f"const NEXP = {len(files)}")
        print("var exp_tab: [NEXP]Exp = {")
        for e in files:
            s = fsum(gen_data(e.get("seed", 0), e["size"]))
            p = e["path"].replace("\\", "\\\\").replace('"', '\\"')
            print(f'    {{ "{p}", {e["size"]}, '
                  f'{e.get("seed", 0)}, ${s[0]:04X}, ${s[1]:04X} }},')
        print("}")
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
