CC ?= cc
CFLAGS ?= -O2 -Wall
# assembler and emulator CPU16 (built from cpu16asm.cpp and cpu16.cpp)
CPU16ASM ?= cpu16asm
CPU16 ?= cpu16
# sources of the CPU16 tools (tools/cpu16.cpp, cpu16asm.cpp): for tests/monrun -
# running programs under a simulated monitor; without them this run is skipped
CPU16SRC ?= ../cpu16/tools
CXX ?= c++
export CPU16ASM CPU16

b16c: b16c.c
	$(CC) $(CFLAGS) -o $@ $<

# all tests and examples against the reference output tests/expected/*.out
test: b16c
	@if [ -f $(CPU16SRC)/cpu16.cpp ]; then $(MAKE) -s tests/monrun; fi
	@tests/run_tests.sh

tests/monrun: tests/monrun.cpp
	$(CXX) -std=c++17 -O2 -w -I$(CPU16SRC) -o $@ tests/monrun.cpp

# 140 programs with random expressions against a Python reference (40 of them with deep expressions)
fuzz: b16c
	@for s in $$(seq 1 50); do \
	  B16C=./b16c python3 tests/fuzz.py $$s || exit 1; \
	  B16C=./b16c python3 tests/fuzz.py $$s --nodiv || exit 1; \
	done; \
	for s in $$(seq 1 20); do \
	  B16C=./b16c python3 tests/fuzz.py $$s --deep || exit 1; \
	  B16C=./b16c python3 tests/fuzz.py $$s --deep --nodiv || exit 1; \
	done; echo "fuzz: 140 programs, no mismatches"

# differential test of long/ulong against a Python reference: levels 1-3 and 5 (data, calls, arithmetic,
# multiplication and division), ordinary and register variables
fuzz32: b16c
	@for s in $$(seq 1 40); do \
	  for o in "--level 1" "--level 2" "--level 3" "--level 3 --regs" "--level 5" "--level 5 --regs"; do \
	    B16C=./b16c python3 tests/fuzz32.py $$s $$o || exit 1; \
	  done; \
	done; echo "fuzz32: 240 programs, no mismatches"

# deep expressions (register limit): a separate run
fuzz32-deep: b16c
	@for s in $$(seq 1 40); do \
	  for o in "--level 5 --deep" "--level 5 --deep --regs"; do B16C=./b16c python3 tests/fuzz32.py $$s $$o || exit 1; done; \
	done; echo "fuzz32-deep: 80 programs, no mismatches"

# parameter placement: a program built with -fcopy-params and without it must produce the same output
fuzzp: b16c
	@for s in $$(seq 1 100); do B16C=./b16c python3 tests/fuzzp.py $$s || exit 1; done; \
	echo "fuzzp: 100 programs, no mismatches"

.PHONY: test fuzz fuzz32 fuzz32-deep fuzzp clean

clean:
	rm -f b16c tests/monrun examples/*/*.s examples/*/*.bin examples/*/*.lst examples/universal/asmlink/*.s examples/universal/asmlink/*.bin tests/*.s tests/*.bin tests/*.lst
