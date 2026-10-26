# Makefile - CPU16 monitor: kernel, shell, utilities, boot from disk.
#   make            tools, monitor, test bench
#   make test       all scenarios (tools/run_tests.py), then the same with -fcopy-params
#   make system     bootable volume out/system.img
#   make dist       ready-made kit dist/: system.img and rom.bin
#   make clean
# B16FLAGS - extra b16c options for the kernel and programs (diagnostics, -fcopy-params);
# image parameters (target, addresses, max_top, min_free) and assembler modules are set in the sources.
CXX ?= c++
D := $(CURDIR)
O := $(D)/out
ASM := $(D)/../cpu16/build/cpu16asm
B16C := $(D)/../b16c/b16c
K := kernel
KSRC := $(wildcard $(K)/*.b16) $(wildcard $(K)/*.asm)

.PHONY: all tools monitor kmon test clean system boot dist
all: monitor kmon

tools: $(ASM) $(B16C)
$(ASM): ../cpu16/tools/cpu16asm.cpp
	$(MAKE) -C ../cpu16 tools
$(B16C): ../b16c/b16c.c
	$(MAKE) -C ../b16c b16c

monitor: $(O)/monitor.bin
$(O)/monitor.bin: $(KSRC) | tools
	mkdir -p $(O)
	$(B16C) -Werror $(B16FLAGS) $(K)/kernel.b16 -o $(O)/kernel.s
	cp $(K)/boot.asm $(K)/resident.asm $(K)/monitor.asm $(O)/
	cd $(O) && $(ASM) -o monitor.bin -l monitor.lst -m monitor.map monitor.asm

kmon: $(O)/kmon
$(O)/kmon: tools/kmon.cpp ../cpu16/tools/cpu16.cpp | tools
	mkdir -p $(O)
	$(CXX) -std=c++17 -O2 -w -I../cpu16/tools -o $@ tools/kmon.cpp

test: all
	python3 tools/run_tests.py
	rm -f $(O)/monitor.bin
	python3 tools/run_tests.py --copy-params
	rm -f $(O)/monitor.bin

# BScript: compiler, virtual machine, script loader (../bscript)
BSTOOLS := bsc bsvm bs
UTILS := ls cat cp mv rm mkdir rmdir attr df mount umount sync date mem fsck touch sysboot

# boot from disk: ROM, volume boot code, kernel file
boot: $(O)/rom.bin $(O)/fsboot.bin $(O)/monitor.sys
$(O)/rom.bin: boot/rom.asm | tools
	mkdir -p $(O)
	$(ASM) -o $@ -l $(O)/rom.lst boot/rom.asm
$(O)/fsboot.bin: boot/fsboot.asm | tools
	mkdir -p $(O)
	$(ASM) --base '$$C000' -o $@ -l $(O)/fsboot.lst boot/fsboot.asm
$(O)/monitor.sys: $(O)/monitor.bin tools/mksys.py
	python3 tools/mksys.py $(O)/monitor.bin $(O)/monitor.map $@

# bootable system volume out/system.img: kernel, shell, utilities, fm, edit, BScript, boot code
system: all boot
	tools/b16build.sh user/shell/shell.b16
	for u in $(UTILS); do tools/b16build.sh user/bin/$$u.b16 || exit 1; done
	tools/b16build.sh user/fm/fm.b16
	tools/b16build.sh user/edit/edit.b16
	$(MAKE) -C ../bscript all
	for t in $(BSTOOLS); do cp ../bscript/out/$$t.bin $(O)/$$t.bin || exit 1; done
	cp ../bscript/out/bsvm.ovl $(O)/bsvm.ovl
	python3 tools/mksysvol.py $(O)/system.json $(UTILS) fm edit $(BSTOOLS) bsvm.ovl=$(O)/bsvm.ovl
	python3 tools/fs16tool.py build $(O)/system.img $(O)/system.json

# ready-made kit: bootable volume and ROM image in dist/
dist: system
	mkdir -p dist
	cp $(O)/system.img $(O)/rom.bin dist/
	python3 tools/fs16tool.py check dist/system.img

clean:
	rm -rf out
