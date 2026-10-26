#!/bin/sh
# stress.sh [gc|far|both] - verification builds of bsvm on tests and examples (built in out/stress):
#   gc    garbage collection before every allocation (GC_STRESS, rule 12.3)
#   far   extended mode from the start, near heap of 2 KB (FAR_TEST, sect. 12.10)
#   both  both modes together
set -e
cd "$(dirname "$0")/.."
M=${1:-gc}
S=out/stress
rm -rf $S && mkdir -p $S
cp -r vm $S/vm
if [ $M = gc ] || [ $M = both ]; then
    sed -i 's/^const GC_STRESS = 0 /const GC_STRESS = 1 /' $S/vm/heap.b16
    grep -q '^const GC_STRESS = 1 ' $S/vm/heap.b16
fi
if [ $M = far ] || [ $M = both ]; then
    sed -i 's/^const FAR_TEST = 0 /const FAR_TEST = 1 /' $S/vm/heap.b16
    grep -q '^const FAR_TEST = 1 ' $S/vm/heap.b16
fi
export BSVM_SRC=$PWD/$S/vm
python3 tools/mkovl.py stubs $S/ovl_stubs.b16
python3 tools/mkovl.py keep $S/ovl_keep.b16
tools/build.sh $S/vm/bsvm.b16 $S/bsvm.bin -I $S 2>/dev/null
python3 tools/mkovl.py env $S/bsvm.map $S/ovl_env.b16
for o in fs sys gc init; do python3 tools/mkovl.py overlay $o $S/ov_$o.ov $S/bsvm.map >/dev/null; done
python3 tools/mkovl.py pack $S/bsvm.ovl $S/ov_fs.ov $S/ov_sys.ov $S/ov_gc.ov $S/ov_init.ov
export BSOUT=$PWD/$S
# heavy far-memory tests - each in a separate test bench run
# (farchurn with a build that collects before every allocation takes more than 8 billion steps: it is checked in far mode)
HEAVY=farbig,farchurn,bigmod,bigconst
python3 tools/run_tests.py --skip $HEAVY
for t in farbig bigmod bigconst; do python3 tools/run_tests.py --steps 8000000000 $t; done
if [ $M = far ]; then python3 tools/run_tests.py --steps 8000000000 farchurn; fi
python3 tools/run_tests.py --examples
