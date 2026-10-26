// test_disasm.js - a round-trip test of the disassembler: parse -> assemble with cpu16asm -> compare.
//
// Run: node test_disasm.js path/to/cpu16asm [cpu16_debugger.html]
//
// For every valid word (all 32849) with a random second word:
//   1. the disassembler gives text in the reference's syntax;
//   2. cpu16asm assembles this text at the same address;
//   3. the words must match. The exception is forms the assembler may choose itself
//      (Jxx with MOD=1 -> JR, a long relative jump -> JR, a long constant 0..15 ->
//      short). For them it is required that re-parsing gives the same text.
// Forms that are not in the language are not checked: TRAP except #n and Rn, a shift with a long constant.

"use strict";
const fs = require("fs");
const os = require("os");
const path = require("path");
const { execFileSync } = require("child_process");

const asmPath = process.argv[2];
const htmlPath = process.argv[3] || path.join(__dirname, "..", "cpu16_debugger.html");
if (!asmPath) { console.error("usage: node test_disasm.js path/to/cpu16asm [cpu16_debugger.html]"); process.exit(2); }
const html = fs.readFileSync(htmlPath, "utf8");
const code = html.slice(html.indexOf("// BEGIN ENGINE"), html.indexOf("// END ENGINE"));
const { ISA, Cpu16Engine } = new Function(code + "\nreturn { ISA, Cpu16Engine };")();

let seed = 12345;
const rnd16 = () => { seed = (Math.imul(seed, 1103515245) + 12345) >>> 0; return (seed >>> 8) & 0xFFFF; };

// All valid words, except forms outside the language.
const words = [];
for (let w = 0; w < 65536; w++) {
  if (!ISA.valid(w)) continue;
  const op = w >> 11, r2 = (w >> 3) & 15, mod = w & 7, cls = ISA.opInfo[op].cls;
  if (cls === "trap" && mod > 1) continue;
  if (cls === "shift" && mod === 3 && r2 === 14) continue;
  words.push(w);
}

const SLOT = 8, PER_BATCH = 8000;
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "cpu16dis-"));
let exact = 0, canonical = 0, fails = 0, skippedLang = 65536 - words.length;
const failSamples = [];

function textOf(row) { return row.operands ? `${row.mnemonic} ${row.operands}` : row.mnemonic; }
function isCanonicalKind(w, w2) {
  const op = w >> 11, r2 = (w >> 3) & 15, mod = w & 7, cls = ISA.opInfo[op].cls;
  if (cls === "branch" && (mod === 1 || (mod === 7 && r2 === 14))) return true;
  if ((cls === "load" || cls === "alu") && mod === 3 && r2 === 14 && w2 <= 15) return true;
  return false;
}

for (let b = 0; b < words.length; b += PER_BATCH) {
  const batch = words.slice(b, b + PER_BATCH);
  const e = new Cpu16Engine();
  const items = [];
  let src = "";
  batch.forEach((w, i) => {
    const a = 0x0100 + i * SLOT;
    const w2 = rnd16();
    e.writeMem(a, [w & 0xFF, w >> 8, w2 & 0xFF, w2 >> 8]);
    const row = e.decodeAt(a, null);
    const text = textOf(row);
    items.push({ a, w, w2, len: row.words.length, text });
    src += `        .org $${a.toString(16).toUpperCase()}\n        ${text}\n`;
  });
  const asmFile = path.join(tmp, `b${b}.asm`), binFile = path.join(tmp, `b${b}.bin`);
  fs.writeFileSync(asmFile, src);
  try {
    execFileSync(asmPath, [asmFile, "-o", binFile], { stdio: "pipe" });
  } catch (err) {
    const msg = String(err.stderr).split("\n").slice(0, 5).join("\n");
    console.log(`batch ${b}: cpu16asm reported errors:\n${msg}`);
    fails += batch.length;
    continue;
  }
  const bin = fs.readFileSync(binFile);
  const g = new Cpu16Engine();
  g.loadImage(bin, 0);
  for (const it of items) {
    const gw = g.memWord(it.a), glen = g.instrLen(gw);
    const same = glen === it.len && gw === it.w && (it.len === 1 || g.memWord(it.a + 2) === it.w2);
    if (same) { exact++; continue; }
    const again = textOf(g.decodeAt(it.a, null));
    if (again === it.text && isCanonicalKind(it.w, it.w2)) { canonical++; continue; }
    fails++;
    if (failSamples.length < 10)
      failSamples.push(`$${it.a.toString(16)}: ${it.w.toString(16).padStart(4, "0")} ${it.w2.toString(16).padStart(4, "0")} "${it.text}" -> ` +
                       `${gw.toString(16).padStart(4, "0")} "${again}"`);
  }
}
fs.rmSync(tmp, { recursive: true, force: true });
for (const s of failSamples) console.log("FAIL:", s);
console.log(`disassembler round trip: ${words.length} valid words (${skippedLang} invalid or outside the language), ` +
            `${exact} identical, ${canonical} re-encoded by the assembler's own choice of form, ${fails} failed`);
console.log(`${fails ? "FAILED" : "OK"}`);
process.exit(fails ? 1 : 0);
