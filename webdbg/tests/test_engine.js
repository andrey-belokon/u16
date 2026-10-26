// test_engine.js - checking the JS engine against the cpu16.cpp emulator and checking the demo program.
//
// Run: node test_engine.js cases.jsonl [cpu16_debugger.html]
//   cases.jsonl is the output of gen_cases (the reference computed in C++).
// The engine is taken from the built page (between // BEGIN ENGINE and // END ENGINE),
// so exactly the code that runs in the browser is checked.

"use strict";
const fs = require("fs");
const path = require("path");

const casesPath = process.argv[2];
const htmlPath = process.argv[3] || path.join(__dirname, "..", "cpu16_debugger.html");
const html = fs.readFileSync(htmlPath, "utf8");
const code = html.slice(html.indexOf("// BEGIN ENGINE"), html.indexOf("// END ENGINE"));
const { ISA, MACHINE, Cpu16Engine, Cpu16Rtc, Cpu16Assembler } = new Function(code + "\nreturn { ISA, MACHINE, Cpu16Engine, Cpu16Rtc, Cpu16Assembler };")();
const DEMO = JSON.parse(/const DEMO = (\{.*?\});\n/s.exec(html)[1]);

let failed = 0, passed = 0;
function check(cond, what) {
  if (cond) passed++;
  else { failed++; console.log("FAIL:", what); }
}
const fnv = (h, v) => Math.imul(h ^ v, 16777619) >>> 0;
// The test disks are the same as in gen_cases.cpp.
const diskByte = (n, i) => ((((Math.imul(i, 31) >>> 0) ^ (Math.floor(i / 512) * 7)) + n * 17) & 0xFF);
const TEST_DISKS = [[0, 64, false], [1, 16, true], [3, 8, false]];

// ---------------------------------------------------------------- comparison with C++
if (casesPath) {
  const lines = fs.readFileSync(casesPath, "utf8").trim().split("\n");
  let caseFails = 0, totalSteps = 0, irqs = 0, keysFed = 0, screens = 0;
  let diskCmds = 0, diskSectors = 0, diskIrqs = 0, mmuOn = 0, delayed = 0, rtcSet = 0, rtcCmds = 0;
  for (const line of lines) {
    const c = JSON.parse(line);
    const e = new Cpu16Engine();
    for (const [n, sectors, ro] of TEST_DISKS) {
      const data = new Uint8Array(sectors * 512);
      for (let i = 0; i < data.length; i++) data[i] = diskByte(n, i);
      e.attachDisk(n, data, "d" + n, ro);
      e.disk.disks[n].removable = false;     // like MemoryDisk in C++
    }
    e.disk.sectorDelayUs = c.delay;
    if (c.delay) delayed++;
    // coverage statistics: disk commands, sectors transferred, entries into IRQ1, memory manager enables
    const start = e.disk.start.bind(e.disk), unit = e.disk.unit.bind(e.disk), enter = e.enter.bind(e);
    e.disk.start = (x) => { if (x) diskCmds++; start(x); };
    e.disk.unit = () => { if (e.disk.cmd === 1 || e.disk.cmd === 2) diskSectors++; return unit(); };
    const rtcCommand = e.rtc.command.bind(e.rtc);
    e.rtc.command = (x) => { if (x >= 1 && x <= 4) rtcCmds++; rtcCommand(x); };
    e.enter = (n, ip) => { if (n === 1 && (e.levels & 2)) diskIrqs++; enter(n, ip); };
    let sawMmu = false;
    e.loadImage(Buffer.from(c.image, "base64"), 0);
    c.vec.forEach((v, n) => e.writeMem(ISA.VEC_BASE + 2 * n, [v & 0xFF, v >> 8]));
    c.regs.forEach((v, k) => e.setReg(k, v));
    e.mem[0xFF20] = c.port[0]; e.mem[0xFF21] = c.port[1];
    const kb = Buffer.from(c.keys, "base64");
    for (let k = 0; k < kb.length; k += 3) e.feedKey(kb[k], kb[k + 1], kb[k + 2]);
    let clockNow = 0;
    e.dev.clock = () => clockNow;
    e.disk.clock = () => clockNow;
    e.rtc.clock = () => clockNow;
    e.rtc.set(c.rtc[0], c.rtc[1]);
    const rtcFirst = c.rtc[0];
    let ki = 0, ji = 0;
    let h = 2166136261, si = 0;
    const checks = [];
    for (let s = 0; s < c.steps; s++) {
      while (si < c.irq.length && c.irq[si][0] === s) { e.raiseIrq(c.irq[si][1]); si++; irqs++; }
      while (ki < c.keysched.length && c.keysched[ki][0] === s) { e.feedKey(c.keysched[ki][1], c.keysched[ki][2], c.keysched[ki][3]); ki++; keysFed++; }
      clockNow += 10;                                       // the same artificial clock as in gen_cases
      if (ji < c.jumps.length && c.jumps[ji] === s) { clockNow += 400000; ji++; }
      const st = e.step();
      if (e.mmuCtrl) sawMmu = true;
      for (let k = 0; k < 16; k++) h = fnv(h, e.r[k]);
      h = fnv(h, e.pending);
      h = fnv(h, e.levels);
      h = fnv(h, e.waiting ? 1 : 0);
      if (st.idle) e.raiseIrq(0);   // the same rule as in gen_cases.cpp
      if ((s + 1) % 100 === 0) checks.push(h);
    }
    totalSteps += c.steps;
    if (sawMmu) mmuOn++;
    let hm = 2166136261;
    for (let f = 0; f < MACHINE.FRAME_COUNT; f++) {
      const p = e.mem.subarray(f * MACHINE.PAGE_SIZE, (f + 1) * MACHINE.PAGE_SIZE);
      if (!p.some((b) => b !== 0)) continue;
      hm = fnv(hm, f);
      for (let i = 0; i < p.length; i++) hm = fnv(hm, p[i]);
    }
    for (const [n] of TEST_DISKS) for (const b of e.getDiskImage(n)) hm = fnv(hm, b);
    const out = [...e.dev.ttyOut].map((b) => b.toString(16).padStart(2, "0")).join("");
    let hs = 2166136261;
    for (let k = 0; k < MACHINE.COLS * MACHINE.ROWS; k++) { hs = fnv(hs, e.dev.chars[k]); hs = fnv(hs, e.dev.attrs[k]); }
    for (const v of [e.dev.col, e.dev.row, e.dev.ctrl, e.dev.attr, e.dev.timerOn ? 1 : 0, e.dev.keys.length, e.dev.usedDisplay ? 1 : 0, e.dev.mods]) hs = fnv(hs, v);
    const k = e.disk;
    for (const v of [k.status(), k.error, k.disk, k.count, k.addr, k.lba, k.ctrl, e.mmuCtrl]) hs = fnv(hs, v);
    for (const v of e.mmuPage) hs = fnv(hs, v);
    const rtcEnd = e.rtc.value();
    for (const v of [e.rtc.time, e.rtc.date, e.rtc.err ? 1 : 0, rtcEnd & 0xFFFF, rtcEnd >>> 16]) hs = fnv(hs, v);
    if (rtcEnd !== (rtcFirst + Math.floor((clockNow - c.rtc[1]) / 1e6)) % MACHINE.RTC_PERIOD) rtcSet++;
    const firstBad = checks.findIndex((x, i) => x !== c.checks[i]);
    const ok = firstBad < 0 && hm === c.mem && out === c.out && hs === c.screen;
    screens += e.dev.usedDisplay ? 1 : 0;
    if (!ok) {
      caseFails++;
      if (caseFails <= 5)
        console.log(`case ${c.id}: ` + (firstBad >= 0 ? `state differs within steps ${firstBad * 100 + 1}..${firstBad * 100 + 100}` :
                     hm !== c.mem ? "memory differs at the end" : out !== c.out ? "teletype output differs" : "screen or device state differs"));
    }
  }
  check(caseFails === 0, `JS engine matches cpu16.cpp on ${lines.length} random programs`);
  console.log(`engine vs C++: ${lines.length} programs, ${totalSteps} steps, ${irqs} scheduled IRQs, ` +
              `${keysFed} key events during the run, ${screens} programs used the display, ${caseFails} mismatching`);
  console.log(`  devices: ${diskCmds} disk commands, ${diskSectors} sectors transferred, ${diskIrqs} IRQ1 entries, ` +
              `MMU enabled in ${mmuOn} programs, ${delayed} programs with 1 ms per sector`);
  console.log(`  clock: ${rtcCmds} clock commands, the program changed the clock in ${rtcSet} programs`);
}

// ---------------------------------------------------------------- the demo program
{
  // the demo is embedded as source: we assemble it with the JS assembler, as the page does
  const asm = new Cpu16Assembler(() => null);
  check(asm.assembleText("demo.asm", DEMO.source), "demo: assembles with the JS assembler");
  const e = new Cpu16Engine();
  for (let x = 0; x < 65536; x++) if (asm.owner[x] >= 0) e.mem[x] = asm.image[x];
  const sym = new Map(asm.symbols().map((s) => [s.name, s.value]));
  const word = (n) => e.memWord(sym.get(n));
  let s = e.runChunk(100000, new Set());
  check(s.reason === "waiting", "demo: runs until WFI");
  let out = e.takeConsoleOutput();
  check(out === "Hello 16bit world!\n", `demo: prints the greeting (got ${JSON.stringify(out)})`);
  check(word("result") === 150, "demo: sum of table is 150");
  check(e.state === "waiting" && (e.r[15] & ISA.IF), "demo: waits with interrupts enabled");
  s = e.runChunk(10, new Set());
  check(s.reason === "waiting" && s.idle, "demo: stays idle without a request");
  e.raiseIrq(0);
  check(e.pendingIrqs === 1, "demo: IRQ0 pending");
  s = e.step();
  check(e.r[14] === sym.get("irq0") && /IRQ0 taken/.test(s.message), "demo: IRQ0 enters the handler");
  s = e.runChunk(100000, new Set());
  out = e.takeConsoleOutput();
  check(out === "IRQ0\n" && word("ticks") === 1 && e.r[13] === 0xFF00, "demo: handler prints, counts, restores SP");
  // IRQ0 and IRQ5 at once: first IRQ0 (higher priority), after its IRET - IRQ5. Vector 5 is not set (0),
  // so IRQ5 restarts the program from address 0, and it prints the greeting again.
  e.raiseIrq(0); e.raiseIrq(5);
  e.runChunk(100000, new Set());
  out = e.takeConsoleOutput();
  check(out === "IRQ0\nHello 16bit world!\n" && e.pendingIrqs === 0 && word("ticks") === 2,
        `demo: IRQ priority and unset vector (got ${JSON.stringify(out)})`);
}

// ---------------------------------------------------------------- individual cases
{
  const e = new Cpu16Engine();
  // ERRCMD: the reserved opcode 7, vector 8 points to $0100
  e.writeMem(0, [0x00, 0x38]);                 // the word $3800: opcode 00111
  e.writeMem(ISA.VEC_BASE + 16, [0x00, 0x01]);
  e.setReg(13, 0x8000);
  const s = e.step();
  check(e.r[14] === 0x100 && /ERRCMD at \$0000/.test(s.message) && /reserved opcode/.test(s.message), "ERRCMD message and vector");
  check(e.memWord(0x7FFE) === 0 && e.r[13] === 0x7FFC, "ERRCMD saves the address of the faulting instruction");
}
{
  // input $FF20: a byte or 0; the debugger windows (readMem, the disassembler) do not take input
  const e = new Cpu16Engine();
  e.feedInput([65, 66]);
  e.readMem(0xFF20, 4); e.disassemble(0xFF20, 2, null); e.memWord(0xFF20);
  check(e.pendingInput === 2, "reading memory views does not consume input");
  check(e.rd8(0xFF20) === 65 && e.rd8(0xFF20) === 66 && e.rd8(0xFF20) === 0 && e.pendingInput === 0, "port returns input bytes, then 0");
  e.feedInput([1]); e.clearInput();
  check(e.pendingInput === 0 && e.rd8(0xFF20) === 0, "clearInput empties the queue");
}
{
  // SCRD (the machine, revision 9): reading the screen; a word shifts the position as in coordinate mode, a byte does not;
  // the debugger windows do not touch the position
  const e = new Cpu16Engine();
  e.wr8(0xFF22, 0); e.wr8(0xFF23, 0x1E); e.wr8(0xFF24, 79); e.wr8(0xFF25, 24);
  e.wr8(0xFF20, 0x5A);                                  // (79, 24) -> position (0, 0)
  e.wr8(0xFF24, 79); e.wr8(0xFF25, 24);
  check(e.rd8(0xFF50) === 0x5A && e.rd8(0xFF51) === 0x1E && e.rd8(0xFF24) === 79, "SCRD: bytes - char and attribute, position unchanged");
  e.readMem(0xFF50, 2); e.memWord(0xFF50);
  check(e.rd8(0xFF24) === 79 && e.rd8(0xFF25) === 24, "SCRD: memory views do not move the position");
  check(e.rd16(0xFF50) === 0x1E5A && e.rd8(0xFF24) === 0 && e.rd8(0xFF25) === 0, "SCRD: word - cell, position wraps to (0, 0)");
  check(e.rd16(0xFF50) === 0x0720 && e.rd8(0xFF24) === 1, "SCRD: after reset - space, $07; position advances");
  e.wr8(0xFF50, 0x41);
  check(e.rd8(0xFF50) === 0x20, "SCRD: writes are ignored");
}
{
  // teletype output: bytes in the machine encoding -> text for the console log
  const e = new Cpu16Engine();
  for (const b of [0xC6, 0xA0, 0x1A, 0x41, 0x08, 0x0D, 0x0A]) e.wr8(0xFF20, b);
  check(e.takeConsoleOutput() === "Ж═→A\b\n", "teletype output decodes the machine character set");
  e.feedText("Ёж€\n");
  const got = [];
  while (e.pendingInput) got.push(e.rd8(0xFF20));
  check(JSON.stringify(got) === JSON.stringify([0xB3, 0xE6, 0x0A]), `feedText encodes text, skipping unknown characters: ${got}`);
}
{
  // the device window: an address without a device is 0, a write is ignored; the debugger windows show registers
  // without side effects; writeMem does not write into the window
  const e = new Cpu16Engine();
  e.wr8(0xFF2F, 0x55); e.wr16(0xFFF0, 0x1234); e.wr8(0xFF50, 1);
  check(e.rd8(0xFF2F) === 0 && e.rd16(0xFFF0) === 0 && e.mem.subarray(0xFF20).every((b) => b === 0), "I/O window: empty addresses read 0, writes ignored");
  e.feedKey(0x41, 0x1E);
  check(e.readMem(0xFF20, 2)[0] === 0x41 && e.readMem(0xFF21, 1)[0] === 0x1E && e.pendingInput === 1 && e.readMem(0xFF27, 1)[0] === 80,
        "readMem shows device registers without consuming input");
  e.writeMem(0xFF1E, [1, 2, 3, 4]);
  check(e.rd8(0xFF1E) === 1 && e.rd8(0xFF1F) === 2 && e.pendingInput === 1, "writeMem: memory below the window, nothing into it");
}
{
  // the memory manager in JS: registers, logical and physical windows, reset
  const e = new Cpu16Engine();
  e.wr16(MACHINE.MMU_PAGE0 + 4, (5 << 8) | 40);   // PAGE4 = 40, PAGE5 = 5
  e.wr8(MACHINE.MMU_CTRL, 1);
  e.wr16(0x8002, 0xBEEF);
  const m = e.getMmu();
  check(m.enabled && m.pages[4] === 40 && m.phys[4] === 40 * 0x2000 && e.mem[40 * 0x2000 + 2] === 0xEF && e.readPhys(40 * 0x2000 + 2, 2)[1] === 0xBE,
        "MMU: logical page 4 maps to frame 40");
  e.writePhys(40 * 0x2000 + 4, [7]);
  check(e.readMem(0x8004, 1)[0] === 7 && e.memWord(0x8002) === 0xBEEF, "MMU: physical writes are seen through the mapping");
  e.setMmuPage(4, 41); e.setMmuEnabled(false);
  check(e.rd8(MACHINE.MMU_PAGE0 + 4) === 41 && e.rd8(MACHINE.MMU_CTRL) === 0 && e.rd16(0x8002) === 0, "MMU: panel edits");
  e.wr8(MACHINE.MMU_CTRL, 1); e.reset();
  check(!e.getMmu().enabled && e.getMmu().pages.every((p) => p === 0), "reset: MMU off, PAGE registers 0");
}
{
  // disks in the debugger: attaching, the label, RO, a 1 ms per sector delay by the clock, pause, detaching during a command
  const e = new Cpu16Engine();
  let now = 0;
  e.disk.clock = () => now;
  check(e.diskDelayMs === 1, "default disk delay is 1 ms per sector");
  const img = new Uint8Array(4 * 512); img[512] = 0x42;
  e.attachDisk(2, img, "C:\\images\\Мой диск €.img");
  const d = e.getDisks()[2];
  check(d && d.sectors === 4 && d.label === "Мой диск ?.img" && e.rd8(MACHINE.DK_DRIVES) === 4, `attach: label from the file name (${d && d.label})`);
  let threw = false;
  try { e.attachDisk(3, new Uint8Array(100), "bad.img"); } catch (x) { threw = true; }
  check(threw && !e.getDisks()[3], "attach: size must be a multiple of 512");
  e.wr16(MACHINE.DK_DISK, (2 << 8) | 2);          // COUNT = 2, DISK = 2
  e.wr16(MACHINE.DK_ADDR, 0x1000);
  e.wr8(MACHINE.DK_CTRL, 1);
  e.wr8(MACHINE.DK_CMD, MACHINE.DKC_READ);
  check(e.diskBusy && e.canWake && (e.rd8(MACHINE.DK_STATUS) & 1), "READ with a delay: BUSY");
  now = 1000; e.pollDevices();
  check(e.rd8(MACHINE.DK_COUNT) === 1 && e.rd16(MACHINE.DK_ADDR) === 0x1200, "one sector after 1 ms");
  e.setClockFrozen(true); now = 9000; e.pollDevices();
  check(e.rd8(MACHINE.DK_COUNT) === 1, "paused: the transfer stands");
  e.setClockFrozen(false); now = 9999; e.pollDevices();
  check(e.rd8(MACHINE.DK_COUNT) === 1, "resume: the sector time starts again");
  e.detachDisk(2);
  check(!e.diskBusy && e.rd8(MACHINE.DK_ERROR) === MACHINE.DKE_IO && e.rd8(MACHINE.DK_COUNT) === 1 && e.pendingIrqs === 2,
        "detach during a command: IO error, IRQ1 raised");
  e.attachDisk(0, new Uint8Array(512), "x.img", true);
  e.wr16(MACHINE.DK_DISK, (1 << 8) | 0); e.wr8(MACHINE.DK_CMD, MACHINE.DKC_WRITE);
  check(e.rd8(MACHINE.DK_ERROR) === MACHINE.DKE_READONLY && (e.rd8(MACHINE.DK_STATUS) & MACHINE.DKS_RO), "read-only disk");
  e.setDiskReadOnly(0, false); e.wr8(MACHINE.DK_CMD, MACHINE.DKC_INFO);
  now = 20000; e.pollDevices();
  check(e.readMem(0x1200, 5)[4] === (MACHINE.DKI_REMOVABLE | MACHINE.DKI_SMALL) && e.getDisks()[0].readOnly === false,
        "INFO: debugger disks are removable");
  e.reset();
  check(e.getDisks()[0] && e.rd8(MACHINE.DK_DRIVES) === 1 && e.rd8(MACHINE.DK_STATUS) === MACHINE.DKS_PRESENT && e.pendingIrqs === 0,
        "reset keeps disks, clears the controller");
}
{
  // the clock: the initial value is the host time, the getRtc / setRtc / setRtcHostTime contract, reset, pause
  const e = new Cpu16Engine();
  const host = new Date();
  const r0 = e.getRtc();
  check(Math.abs(r0.civil.year - host.getFullYear()) <= 1 || host.getFullYear() < 2025 || host.getFullYear() > 2088,
        `clock starts at host time: ${r0.text}`);
  let now = 5000000;
  e.rtc.clock = () => now;
  check(e.setRtc("28.02.2028 23:59:59.5") === false && e.setRtc("2028-02-28 23:59:59") === false && e.setRtc("29.02.2027 10:00:00") === false &&
        e.setRtc("31.12.2024 23:59:59") === false && e.setRtc("01.13.2026 00:00:00") === false && e.setRtc("") === false,
        "setRtc rejects bad text and invalid dates");
  e.wr16(MACHINE.RTC_TIME, 0x1234); e.wr16(MACHINE.RTC_DATE, 0); e.wr8(MACHINE.RTC_CMD, MACHINE.RTCC_WRITE);
  check(e.setRtc("28.02.2028 23:59:59") === true, "setRtc accepts a valid value");
  let r = e.getRtc();
  check(r.text === "28.02.2028 23:59:59" && r.time === 0x1234 && r.date === 0 && r.err === true && r.msToNextSecond === 1000,
        "setRtc sets the clock only: TIME, DATE and ERR are the program's");
  e.setClockFrozen(true); now += 1000000;
  check(e.getRtc().text === "29.02.2028 00:00:00", "the clock runs while the debugger is paused");
  e.setClockFrozen(false);
  e.reset();
  r = e.getRtc();
  check(r.time === 0 && r.date === 0 && r.err === false && r.text === "29.02.2028 00:00:00", "reset clears the registers, keeps the clock");
  e.wr8(MACHINE.RTC_CMD, MACHINE.RTCC_READ);
  const t = Cpu16Rtc.fromSeconds(Cpu16Rtc.parse("29.02.2028 00:00:00"));
  check(e.rd16(MACHINE.RTC_TIME) === t.time && e.rd16(MACHINE.RTC_DATE) === t.date && e.readMem(MACHINE.RTC_TIME, 4).join() ===
        [t.time & 255, t.time >> 8, t.date & 255, t.date >> 8].join(), "READ: registers, also in the memory view");
  check(e.setRtc("31.12.2088 23:59:59") && (now += 1000000, e.getRtc().text === "01.01.2025 00:00:00"), "the clock wraps 2088 -> 2025");
  now += 300000;
  check(e.getRtc().msToNextSecond === 700, "time to the next second");
  e.setRtcHostTime();
  const h = new Date(), hr = e.getRtc().civil;
  check(h.getFullYear() < 2025 || h.getFullYear() > 2088 || (hr.year === h.getFullYear() && hr.month === h.getMonth() + 1 && hr.day === h.getDate()),
        "Host time sets the host's local date");
  check(e.rtc.anchor <= now && now - e.rtc.anchor < 1000000, "Host time: the phase follows the host's second");
}
console.log(`${failed ? "FAILED" : "OK"}: ${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
