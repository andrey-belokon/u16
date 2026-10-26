# Makefile - BScript: the bsc compiler, the bsvm virtual machine, the bs loader.
#   make            out/bsc.bin out/bsvm.bin out/bs.bin
#   make stress     tests and examples with bsvm: garbage collection before every allocation; extended mode from the start
#   make examples   examples/*.bs in the emulator against the references in examples/expected
#   make test       tests/*.bs in the emulator (tools/run_tests.py); builds ../monitor (make system)
COMMON := $(wildcard common/*.b16)
OVS := fs sys gc init
all: out/bsc.bin out/bsvm.bin out/bsvm.ovl out/bs.bin
# test bench, monitor, shell and utilities for the tests (../monitor/out)
monitor:
	$(MAKE) -C ../monitor system
out/bsc.bin: $(wildcard bsc/*.b16) $(COMMON)
	tools/build.sh bsc/bsc.b16 $@
# bsvm: resident part and overlays (vm/ovl.b16, tools/mkovl.py); the window is page 6, hence
# max_top = $C000 (option max_top in vm/bsvm.b16)
out/ovl_stubs.b16: $(wildcard vm/ov_*.b16) tools/mkovl.py
	mkdir -p out && python3 tools/mkovl.py stubs $@
out/ovl_keep.b16: $(wildcard vm/*.b16) $(COMMON) out/ovl_stubs.b16 tools/mkovl.py
	python3 tools/mkovl.py keep $@
out/bsvm.bin: $(wildcard vm/*.b16) $(COMMON) out/ovl_stubs.b16 out/ovl_keep.b16
	tools/build.sh vm/bsvm.b16 $@ -I out
out/ovl_env.b16: out/bsvm.bin tools/mkovl.py
	python3 tools/mkovl.py env out/bsvm.map $@
out/ov_%.ov: vm/ov_%.b16 vm/ov_common.b16 out/ovl_env.b16
	python3 tools/mkovl.py overlay $* $@ out/bsvm.map
out/bsvm.ovl: $(patsubst %,out/ov_%.ov,$(OVS))
	python3 tools/mkovl.py pack $@ $^
out/bs.bin: bs/bs.b16 $(COMMON)
	tools/build.sh bs/bs.b16 $@
test: all monitor
	python3 tools/run_tests.py
# rule 12.3: garbage collection before every allocation (tools/stress.sh)
stress: all monitor
	tools/stress.sh gc
	tools/stress.sh far
examples: all monitor
	python3 tools/run_tests.py --examples
clean:
	rm -rf out
.PHONY: all test examples stress clean monitor
