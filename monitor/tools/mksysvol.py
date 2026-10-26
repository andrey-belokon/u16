#!/usr/bin/env python3
"""mksysvol.py OUT.json UTIL ... - specification of the bootable system volume for fs16tool.py:
/sys/monitor.sys, /sys/shell.cx, /sys/fsboot.bin, /bin/UTIL.cx, boot code out/fsboot.bin, the directory /tmp.
An argument of the form NAME=FILE puts the file NAME into /bin as is (for example, bsvm.ovl=out/bsvm.ovl)."""
import json, sys, time

out, utils = sys.argv[1], sys.argv[2:]

# FS16 timestamp (sect. 5.1) - the moment the volume is built
t = time.localtime()
h = t.tm_hour
mtime = (((t.tm_year - 2025) << 10 | t.tm_mon << 6 | t.tm_mday << 1 | (1 if h >= 12 else 0)) << 16
         | (h % 12) << 12 | t.tm_min << 6 | t.tm_sec)
e = [{"path": "/sys", "dir": True}, {"path": "/bin", "dir": True}, {"path": "/tmp", "dir": True},
     {"path": "/sys/monitor.sys", "src": "out/monitor.sys", "flags": ["SYSTEM", "RDONLY"]},
     {"path": "/sys/shell.cx", "src": "out/shell.bin"},
     {"path": "/sys/fsboot.bin", "src": "out/fsboot.bin", "flags": ["SYSTEM"]}]
e += [{"path": "/bin/%s.cx" % u, "src": "out/%s.bin" % u} for u in utils if "=" not in u]
e += [{"path": "/bin/" + u.split("=", 1)[0], "src": u.split("=", 1)[1]} for u in utils if "=" in u]
for x in e:
    x["mtime"] = mtime
json.dump({"sectors": 4096, "label": "SYSTEM", "boot": "out/fsboot.bin", "entries": e}, open(out, "w"), indent=1)
