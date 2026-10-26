# Makefile - build and tests for CPU16 (needs g++ with C++17, Python 3, Node.js; Playwright for test-ui).
#
#   make            emulator, assembler and web debugger (../webdbg)
#   make tools      build/cpu16 and build/cpu16asm
#   make debugger   ../webdbg/cpu16_debugger.html (built in the web debugger directory)
#   make examples   build all examples into build/examples/
#   make test       all tests except the UI scenarios (MACHINE_DOC=path - check against the "CPU16 Machine" document)
#   make test-ui    web debugger scenarios in headless Chromium (Playwright)
#   make clean      remove build/

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
B        := build
ASM      := $(CURDIR)/$(B)/cpu16asm
EMU      := $(CURDIR)/$(B)/cpu16
TOOLS_H  := tools/cpu16_isa.hpp tools/cpu16_machine.hpp
EXAMPLES := $(wildcard examples/*.asm)
# machine_test.py checks the fragments embedded in it against the "CPU16 Machine" document.
MACHINE_DOC ?= docs/cpu16_machine_en.md

.PHONY: all tools debugger examples test test-tools test-lib test-debugger test-ui clean

all: tools debugger

tools: $(B)/cpu16 $(B)/cpu16asm

$(B):
	mkdir -p $(B) $(B)/examples

$(B)/cpu16: tools/cpu16.cpp $(TOOLS_H) | $(B)
	$(CXX) $(CXXFLAGS) -o $@ tools/cpu16.cpp

$(B)/cpu16asm: tools/cpu16asm.cpp $(TOOLS_H) | $(B)
	$(CXX) $(CXXFLAGS) -o $@ tools/cpu16asm.cpp

# the web debugger lives at the top level of the project, in ../webdbg (with its own Makefile)
debugger: tools
	$(MAKE) -C ../webdbg

examples: $(B)/cpu16asm
	@for f in $(EXAMPLES); do \
		case $$f in *_exec.asm) opt=--exec;; *) opt=;; esac; \
		$(B)/cpu16asm $$opt $$f -o $(B)/examples/$$(basename $$f .asm).bin || exit 1; \
	done

test: test-tools test-lib test-debugger

test-tools: tools
	$(B)/cpu16 --selftest < /dev/null
	$(CXX) $(CXXFLAGS) -Wno-unused-function -Itools -o $(B)/cpu16asm_tests tests/cpu16asm_tests.cpp
	$(B)/cpu16asm_tests

test-lib: tools
	python3 tests/stdlib_test.py $(ASM) $(EMU)
	python3 tests/intro_test.py $(ASM) $(EMU)
	python3 tests/machine_test.py $(ASM) $(EMU) $(MACHINE_DOC)

test-debugger: tools
	$(MAKE) -C ../webdbg test

test-ui: tools
	$(MAKE) -C ../webdbg test-ui

clean:
	rm -rf $(B)
