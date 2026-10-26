#!/usr/bin/env python3
"""mkvol.py - volume specifications for manual checking of fm: out/fmt0.json (the system, utilities, fm, edit,
/data with files and subdirectories) and out/fmt1.json (the second volume)."""
import json, os
D = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
UT = ['ls', 'cat', 'cp', 'mv', 'rm', 'mkdir', 'rmdir', 'attr', 'df', 'mount', 'umount', 'sync', 'date', 'mem', 'fsck', 'touch', 'sysboot']
e = [{"path": "/sys", "dir": True}, {"path": "/bin", "dir": True}, {"path": "/tmp", "dir": True},
     {"path": "/sys/shell.cx", "src": "out/shell.bin"}, {"path": "/bin/fm.cx", "src": "out/fm.bin"},
     {"path": "/bin/edit.cx", "src": "out/edit.bin"}]
e += [{"path": "/bin/%s.cx" % u, "src": "out/%s.bin" % u} for u in UT]
e += [{"path": "/data", "dir": True}, {"path": "/data/hello.txt", "src": "tests/fm/hello.txt"},
      {"path": "/data/big", "size": 70000, "seed": 4},
      {"path": "/data/sub", "dir": True}, {"path": "/data/sub/a.txt", "src": "tests/fm/hello.txt"},
      {"path": "/data/sub/deep", "dir": True}, {"path": "/data/sub/deep/b", "size": 5000, "seed": 2},
      {"path": "/data/ro", "size": 10, "seed": 6, "flags": ["RDONLY"]},
      {"path": "/data/a_very_long_file_name_here.txt", "size": 123, "seed": 3}]
json.dump({"sectors": 2048, "label": "SYSTEM", "entries": e}, open(D + '/out/fmt0.json', 'w'))
json.dump({"sectors": 400, "label": "SECOND", "entries": [{"path": "/x", "size": 10, "seed": 1}]},
          open(D + '/out/fmt1.json', 'w'))
