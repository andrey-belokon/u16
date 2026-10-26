// Standalone boot-chain regression: the real, embedded ROM executes against an FS16 image.
// Run: node tests/test_boot_engine.js [disk.img/system.img]
"use strict";
const fs = require("fs"), path = require("path"), assert = require("assert");
const base = path.join(__dirname, "..");
const html = fs.readFileSync(path.join(base, "cpu16_debugger.html"), "utf8");
const code = html.slice(html.indexOf("// BEGIN ENGINE"), html.indexOf("// END ENGINE"));
const { MACHINE, Cpu16Engine } = new Function(code + "\nreturn { MACHINE, Cpu16Engine };")();
const rom = Uint8Array.from(JSON.parse(/const BOOT_ROM = new Uint8Array\((\[.*?\])\);/.exec(html)[1]));
assert.deepStrictEqual(Buffer.from(rom), fs.readFileSync(path.join(base, "src/rom.bin")), "embedded ROM equals original");
const bootErrors = ["ROM: no disk 0", "ROM: disk 0 read error", "ROM: disk 0 is not bootable"];
function testDisk(name, data, expected) {
  const e = new Cpu16Engine();
  if (data) e.attachDisk(0, Uint8Array.from(data), name);
  e.diskDelayMs = 0;
  e.clearMemory(); e.loadImage(rom, 0); e.reset();
  e.setClockFrozen(false);
  let s;
  for (let i=0; i<8000; i++) {
    s = e.runChunk(4096, new Set());
    if (s.reason !== 'limit') break;
  }
  const out = e.takeConsoleOutput();
  if (expected) assert(out.includes(expected), name + ': expected ' + expected + ', got: ' + JSON.stringify(out));
  console.log(name, 'reason', s.reason, 'steps', e.steps, 'ip', e.getRegs()[14].toString(16), 'output', JSON.stringify(out.slice(0, 400)));
  return {e,s,out};
}
const imgPath = process.argv[2] || path.join(base, "..", "disk.img", "system.img");
testDisk('no disk', null, bootErrors[0]);
testDisk('short disk', Buffer.alloc(1024), bootErrors[1]);
testDisk('blank disk', Buffer.alloc(2048), bootErrors[2]);
const img = fs.readFileSync(imgPath);
const result=testDisk('system disk', img);
assert(!bootErrors.some(m => result.out.includes(m)), 'system disk should boot');
assert(result.e.steps > 500, 'ROM should pass control to boot sector');
assert(result.out.includes('A:/> '), 'system disk should load monitor and shell prompt (drive letter A:)');
assert(result.e.getMmu().enabled, 'monitor must enable MMU');
console.log('OK: ROM boot-chain test');
