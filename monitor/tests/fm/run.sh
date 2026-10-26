#!/bin/sh
# tests/fm/run.sh SCENARIO [kmon options] - build fm, the volumes out/fmt0.img, out/fmt1.img and run the
# scenario under the test bench. Console output and screen dumps go to stdout; dumps only:
#   tests/fm/run.sh SCENARIO 2>/dev/null | python3 tests/fm/show.py
set -e
D=$(cd "$(dirname "$0")/../.." && pwd)
cd "$D"
make -s all >/dev/null
tools/b16build.sh user/shell/shell.b16 >/dev/null
for u in ls cat cp mv rm mkdir rmdir attr df mount umount sync date mem fsck touch sysboot; do
    tools/b16build.sh user/bin/$u.b16 >/dev/null
done
tools/b16build.sh user/fm/fm.b16 >/dev/null
tools/b16build.sh user/edit/edit.b16 >/dev/null
python3 tests/fm/mkvol.py
python3 tools/fs16tool.py build out/fmt0.img out/fmt0.json >/dev/null
python3 tools/fs16tool.py build out/fmt1.img out/fmt1.json >/dev/null
SCN=$1; shift
out/kmon out/monitor.bin --map out/monitor.map --disk out/fmt0.img --disk out/fmt1.img \
    --tick-every 5000 --script "$SCN" --strict-align "$@"
