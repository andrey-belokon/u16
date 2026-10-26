# Makefile - the CPU16 web debugger (webdbg/). Needs ../cpu16 (tools/, lib/, tests/, docs/).
#   make            cpu16_debugger.html from the sources (build.py)
#   make test       checking the engine, disassembler and assembler against C++, FS16 tests, monitor sessions on the engine
#                   (needs ../monitor/dist/system.img)
#   make test-ui    the interface and file manager scenarios (needs playwright with Chromium)
CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
C := ../cpu16
B := build
ASM := $(C)/build/cpu16asm
TOOLS_H := $(wildcard $(C)/tools/*.hpp)

.PHONY: all test test-ui clean
all: cpu16_debugger.html

cpu16_debugger.html: $(wildcard src/*) $(C)/tools/cpu16asm.cpp $(TOOLS_H) $(C)/lib/stdlib.asm build.py
	python3 build.py

$(ASM):
	$(MAKE) -C $(C) tools

test: all $(ASM)
	mkdir -p $(B)
	$(CXX) $(CXXFLAGS) -I$(C)/tools -o $(B)/gen_cases tests/gen_cases.cpp
	$(B)/gen_cases 500 3000 > $(B)/cases.jsonl
	node tests/test_engine.js $(B)/cases.jsonl
	node tests/test_disasm.js $(ASM)
	$(CXX) $(CXXFLAGS) -I$(C)/tools -o $(B)/asm_oracle tests/asm_oracle.cpp
	node tests/test_assembler.js $(CURDIR)/$(B)/asm_oracle
	node tests/test_fs16.js
	node tests/test_sess_engine.js ../monitor/dist/system.img

test-ui: all $(ASM)
	python3 tests/test_ui.py $(ASM)
	python3 tests/test_fs16_ui.py

clean:
	rm -rf $(B)
