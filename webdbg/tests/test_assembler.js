// test_assembler.js - checking the JS assembler against cpu16asm.cpp.
//
// Run: node test_assembler.js path/to/asm_oracle [cpu16_debugger.html]
//
// The source corpus:
//   - all string literals from cpu16asm_tests.cpp (including deliberately erroneous ones);
//   - code blocks from the assembler reference and demo.asm;
//   - programs of thousands of instructions produced by the disassembler;
//   - random expressions, random structured programs (labels, jumps,
//     data, .org, alignment), random "garbage" made of tokens;
//   - cases with .include (nesting, a missing file, exceeding the depth);
//   - .reserve and CX16 executable file headers (correct and with violations).
// For each case it compares: success, diagnostic texts (byte by byte), the occupied bytes
// of the image (a hash), minAddr/maxAddr, the listing, the symbol map and the header check messages (--exec).

"use strict";
const fs = require("fs");
const os = require("os");
const path = require("path");
const { execFileSync } = require("child_process");

const oracle = process.argv[2];
const htmlPath = process.argv[3] || path.join(__dirname, "..", "cpu16_debugger.html");
if (!oracle) { console.error("usage: node test_assembler.js path/to/asm_oracle [cpu16_debugger.html]"); process.exit(2); }
const html = fs.readFileSync(htmlPath, "utf8");
const code = html.slice(html.indexOf("// BEGIN ENGINE"), html.indexOf("// END ENGINE"));
const { ISA, Cpu16Engine, Cpu16Assembler } = new Function(code + "\nreturn { ISA, Cpu16Engine, Cpu16Assembler };")();
const ROOT = path.join(__dirname, "..", "..", "cpu16");   // the project's cpu16/ directory

let seed = 20240607;
const rnd = (n) => { seed = (Math.imul(seed, 1103515245) + 12345) >>> 0; return (seed >>> 8) % n; };
const pick = (a) => a[rnd(a.length)];

const corpus = [];   // { name, src, files? }
const add = (src, name = "test.asm", files = null, binary = false) => corpus.push({ name, src, files, binary });

// ---- 1. literals from the C++ tests
{
  const t = fs.readFileSync(path.join(ROOT, "tests", "cpu16asm_tests.cpp"), "utf8");
  for (const m of t.matchAll(/R"\(([\s\S]*?)\)"/g)) add(m[1]);
  const unesc = (s) => s.replace(/\\(x[0-9a-fA-F]{2}|.)/g, (_, c) =>
    c[0] === "x" && c.length === 3 ? String.fromCharCode(parseInt(c.slice(1), 16)) :
    ({ n: "\n", t: "\t", r: "\r", 0: "\0", "\\": "\\", '"': '"', "'": "'" })[c] ?? c);
  for (const m of t.matchAll(/(?<![R\w])"((?:[^"\\\n]|\\.)*)"/g)) add(unesc(m[1]));
}
// ---- 2. the reference examples and the demo
{
  const md = fs.readFileSync(path.join(ROOT, "docs", "cpu16_asm_reference_en.md"), "utf8");
  for (const m of md.matchAll(/```\n([\s\S]*?)```/g)) add(m[1]);
  add(fs.readFileSync(path.join(__dirname, "..", "src", "demo.asm"), "utf8"), "demo.asm");
  add(fs.readFileSync(path.join(ROOT, "lib", "stdlib.asm"), "utf8"), "stdlib.asm");
}
// ---- 3. programs from the disassembler
{
  const e = new Cpu16Engine();
  for (let b = 0; b < 3; b++) {
    let src = "";
    for (let i = 0; i < 2500; i++) {
      let w;
      do { w = rnd(65536); } while (!ISA.valid(w));
      const a = 0x0200 + i * 8;
      e.writeMem(a, [w & 0xFF, w >> 8, rnd(256), rnd(256)]);
      const row = e.decodeAt(a, null);
      src += `        .org $${a.toString(16)}\n        ${row.operands ? row.mnemonic + " " + row.operands : row.mnemonic}\n`;
    }
    add(src);
  }
}
// ---- 4. random expressions
{
  const atoms = ["0", "1", "15", "16", "-1", "$FFFF", "$10000", "0xFFFFFFFF", "%1010", "0b11", "'A'", "'\\n'", "$", "lbl", "K",
                 "undef", "@loc", "lo($1234)", "hi($1234)", "40", "31", "32", "65535", "-32768", "-32769", "4294967295"];
  const ops = ["+", "-", "*", "/", "<<", ">>", "&", "|", "^"];
  const expr = (d) => {
    if (d <= 0 || rnd(3) === 0) return (rnd(4) === 0 ? pick(["-", "~", "+", "--"]) : "") + pick(atoms);
    const s = expr(d - 1) + " " + pick(ops) + " " + expr(d - 1);
    return rnd(3) === 0 ? "(" + s + ")" : s;
  };
  for (let i = 0; i < 1500; i++) {
    let src = ".equ K, " + expr(2) + "\nlbl: NOP\n@loc: NOP\n";
    for (let j = 0; j < 4; j++) src += pick([".word ", ".byte ", "LD R0, #", "ADD R1, #", "SHL R2, #", "JMP ", "LD R3, [",
                                              ".org ", ".space ", ".align ", "TRAP #", "CALL "]) + expr(3) + "\n";
    add(src);
  }
}
// ---- 4b. boundary expressions: the 2^40 limit, 64-bit overflow, division, shifts
{
  const edge = ["(1<<20)*(1<<20)", "(1<<20)*(1<<20)+1", "-(1<<20)*(1<<20)", "-(1<<20)*(1<<20)-1",
                "$FFFFFFFF*$FFFFFFFF", "$FFFFFFFF*$FFFFFFFF*$FFFFFFFF", "(0-$FFFFFFFF)*$FFFFFFFF", "1<<31<<9", "1<<31<<10",
                "-1>>31", "-7/2", "7/-2", "-7/-2", "1/0", "5<<32", "5<<-1", "~0", "--5", "-(-(1<<40))", "$7FFFFFFF+1",
                "(1<<30)*(1<<30)/(1<<20)", "lo(-1)", "hi(-1)", "hi($10000)", "65535+1", "-32768", "-32769"];
  for (const e of edge) add(`.word ${e}\n.byte ${e}\nLD R0, #${e}\n.equ Q, ${e}\n.word Q\n`);
}
// ---- 5. structured programs: labels, jumps, data, .org
{
  const regs = ["R0", "R1", "R2", "R7", "R11", "LR", "SP", "IP", "FLAGS", "r3"];
  for (let i = 0; i < 600; i++) {
    const n = 5 + rnd(60), labels = [];
    for (let k = 0; k < 8; k++) labels.push(k < 6 ? "L" + k : ["L.x" + k, "mod.f_" + k + ".a1"][k - 6]);   // and with a dot inside
    let src = "";
    for (let j = 0; j < n; j++) {
      if (rnd(5) === 0) src += pick(labels) + ":\n";
      if (rnd(7) === 0) src += "@l" + rnd(3) + ":";
      const k = rnd(22);
      if (k < 4) src += `  ${pick(["JMP", "JZ", "JNE", "JB", "JGE", "CALL"])} ${pick([...labels, "@l0", "@l1", "$+4", "$", "$-200", "$+300"])}\n`;
      else if (k < 6) src += `  LD ${pick(regs)}, #${pick([...labels, "5", "300", "-3", "L0+8"])}\n`;
      else if (k < 8) src += `  ${pick(["ADD", "SUB", "CMP", "XOR", "AND", "MUL", "MULHU", "DIVU", "REMU"])} ${pick(regs)}, ${pick(["#1", "#$1234", "[R1]", "[R2++]", "[--R3]", "[$40]", "[R4+6]", "R5-2", "R6"])}\n`;
      else if (k < 9) src += `  ${pick(["ST", "STB", "XCHB"])} ${pick(["[R1]", "[SP++]", "[--SP]", "[L1]", "[R4-2]", "[IP]"])}, ${pick(regs)}\n`;
      else if (k < 10) src += `  .byte ${rnd(300) - 20}, ${pick(labels)}\n`;
      else if (k < 11) src += `  .word ${pick(labels)}, ${rnd(70000)}\n`;
      else if (k < 12) src += `  .ascii "${pick(["ab", "x\\n", "Ж", "\\x41\\0", ""])}"\n`;
      else if (k < 13) src += `  .align ${pick(["2", "4", "16", "3", "0"])}\n`;
      else if (k < 14) src += `  .org ${pick(["$100", "$FFF0", "0", "$20", "L1"])}\n`;
      else if (k < 15) src += rnd(3) ? `  .space ${pick(["3", "64", "0", "$FFFF"])}${rnd(2) ? ", $AA" : ""}\n`
                                     : `  .reserve ${pick(["3", "64", "0", "$FFFF", "L1", "-1", "$10001"])}${rnd(4) ? "" : ", 1"}\n`;
      else if (k < 16) src += `  ${pick(["PUSH", "POP", "INC", "DEC", "NEG", "NOT", "BSX"])} ${pick(regs)}\n`;
      else if (k < 17) src += `  ${pick(["NOP", "RET", "STI", "CLI", "STC", "CLC", "CMC", "IRET", "WFI"])}\n`;
      else if (k < 18) src += `  ${pick(["SHL", "SHR", "SAR", "ROR", "LDZX", "LDSX"])} ${pick(regs)}, ${pick(["#3", "#16", "R1", "[R2]", "R3+1", "[SP++]"])}\n`;
      else if (k < 19) src += `  LEA ${pick(regs)}, ${pick(["R1+4", "[R2-8]", "IP+(L1-$-4)", "R3", "#1"])}\n`;
      else if (k < 20) src += `  TRAP ${pick(["#9", "#16", "R1", "[R0]"])}\n`;
      else if (k < 21) src += `  .equ C${rnd(4)}, ${pick(["1", "L2", "C1+1", "$"])}\n`;
      else src += `  JMP #${pick(labels)}\n`;
    }
    for (const l of labels) if (rnd(4)) src += l + ": NOP\n";
    add(src);
  }
}
// ---- 5b. correct programs: checking the success path (encoding, form selection, the listing)
{
  const regs = ["R0", "R1", "R2", "R7", "R11", "LR", "r3", "R10"];
  for (let i = 0; i < 800; i++) {
    const nl = 3 + rnd(10), labels = [];
    for (let k = 0; k < nl; k++) labels.push(k % 4 === 3 ? "lab." + k : "lab" + k);
    const lines = [];
    if (rnd(3) === 0) lines.push(`        .org $${(rnd(0x400) * 2).toString(16)}`);
    lines.push(`        .equ K${i % 7}, ${rnd(40)}`);
    let li = 0;
    const n = 10 + rnd(120);
    for (let j = 0; j < n; j++) {
      if (li < labels.length && rnd(6) === 0) lines.push(`${labels[li++]}:`);
      if (rnd(9) === 0) lines.push(`@m${j}:`);
      const k = rnd(16);
      if (k < 4) lines.push(`        ${pick(["JMP", "JZ", "JNZ", "JC", "JA", "JL", "JGE", "JLE", "CALL"])} ${pick(labels)}`);
      else if (k < 6) lines.push(`        LD ${pick(regs)}, #${pick([...labels, "5", "15", "16", "300", "-3", "K" + (i % 7)])}`);
      else if (k < 8) lines.push(`        ${pick(["ADD", "SUB", "CMP", "XOR", "AND", "OR", "ADC", "SBC", "TEST", "MUL", "MULHU", "DIVU", "REMU"])} ${pick(regs)}, ${pick(["#1", "#10", "#0", "#$1234", "[R1]", "[R2++]", "[--R3]", "[$40]", "[R4+6]", "R5-2", "R6", "R1+" + pick(labels)])}`);
      else if (k < 9) lines.push(`        ${pick(["ST", "STB", "XCHB"])} ${pick(["[R1]", "[R4-2]", "[" + pick(labels) + "]", "[R2++]"])}, ${pick(regs)}`);
      else if (k < 10) lines.push(`        .byte ${rnd(256)}, ${rnd(100)}`);
      else if (k < 11) lines.push(`        .word ${pick(labels)}, ${rnd(65536)}`);
      else if (k < 12) lines.push(`        .ascii "${pick(["ab", "xyz", "Ж", "\\x41"])}"`);
      else if (k < 13) lines.push(`        ${pick(["PUSH", "POP", "INC", "DEC", "NEG", "NOT", "BSX"])} ${pick(regs)}`);
      else if (k < 14) lines.push(`        ${pick(["NOP", "RET", "STI", "CLI", "STC", "CLC", "CMC", "IRET", "WFI"])}`);
      else if (k < 15) lines.push(`        ${pick(["SHL", "SHR", "SAR", "ROR"])} ${pick(regs)}, ${pick(["#3", "#15", "R1", "[R2]"])}`);
      else lines.push(`        LEA ${pick(regs)}, ${pick(["R1+4", "[R2-8]", "IP+(" + pick(labels) + "-$-4)"])}`);
    }
    while (li < labels.length) lines.push(`${labels[li++]}: NOP`);
    add(lines.join("\n") + "\n");
  }
}
// ---- 6. token "garbage": the error paths of the lexer and parser
{
  const vocab = ["LD", "ld", "Mov", "JMP", "jz", "CALL", "PUSH", "nop", "R0", "r15", "R16", "R05", "SP", "flags", "IP",
                 "#", "#5", "[", "]", "(", ")", ",", ":", "+", "-", "++", "--", "*", "/", "<<", ">>", "&", "|", "^", "~",
                 "$", "$1F", "0x", "0x1G", "%", "%2", "0b", "12ab", "'", "'a'", "''", "'\\q'", "\"str\"", "\"open",
                 ".word", ".BYTE", ".org", ".equ", ".include", ".foo", ".", "@", "@x", "lbl:", "x", "lo", "hi(", ";c",
                 "a.b", "x.", "@l.p", "@l.", "a..b", "a.b.c", "1.5", "R1.x", "lo.hi", ".word.x", "..", "Ж", "\t", "\\x", "\"\\x4\"", "4294967296", "LEA", "TRAP", "WFI", "XCHB", "STB",
                 "MUL", "mulhu", "DIVU", "REMU", "MISC", "NEG", "not", "BSX", "IRET"];
  for (let i = 0; i < 3000; i++) {
    let src = "";
    const n = 1 + rnd(4);
    for (let j = 0; j < n; j++) {
      const m = 1 + rnd(6);
      const toks = [];
      for (let k = 0; k < m; k++) toks.push(pick(vocab));
      src += toks.join(rnd(3) ? " " : "") + "\n";
    }
    add(src);
  }
}
// ---- 6b. recoding strings: Cyrillic, box-drawing characters, icons, characters outside the encoding, invalid UTF-8
{
  const pieces = ["Привет", "ёЁ", "═║╔╗╚╝", "─│┌┐", "→☺♥", "€", "«»", "a\\x80b", "Ж", "✓", "\\\"q", "Ї", "°²·÷©"];
  for (let i = 0; i < 300; i++) {
    let src = "";
    for (let j = 0; j < 3; j++) {
      const k = rnd(3), p = pick(pieces) + pick(pieces);
      src += k === 0 ? `  .asciz "${p}"\n` : k === 1 ? `  .byte '${pick(["Я", "ё", "═", "€", "a", "→", "ЯЯ"])}'\n` : `  .ascii "${p}", "${pick(pieces)}"\n`;
    }
    add(src);
  }
  // invalid UTF-8 - bytes right in the source (latin1 -> the same bytes)
  for (const bad of ["\xff", "\xd0", "\xe2\x82", "\xc0\x80", "\xf0\x9f\x98\x80", "\x80"]) {
    add(Buffer.from(`  .ascii "x${bad}y"\n  .byte '${bad}'\n`, "latin1").toString("latin1"), "bad.asm", null, true);
  }
}
// ---- 6c. .reserve and CX16 headers: correct ones and with a violation of each checked condition
{
  const hdr = (o = {}) => {
    const f = { jmp: "JMP     #start", magic: '"CX16"', ver: "1", flags: "0", ie: "image_end", be: "bss_end", mf: "512", mt: "$2000", ...o };
    return `        .org    0\n        ${f.jmp}\n        .ascii  ${f.magic}\n        .word   ${f.ver}, ${f.flags}\n` +
           `        .word   ${f.ie}, ${f.be}\n        .word   ${f.mf}, ${f.mt}\n        .word   0, 0, 0, 0\n`;
  };
  const body = (o = {}) => `start:  LD      R0, #0\n${o.code || ""}        RET\n${o.tail || "image_end:\nbuf:    .reserve 100\nbss_end:\n"}`;
  const variants = [
    [{}, {}], [{ jmp: "JMP     start" }, {}], [{ jmp: "JMP     [start]" }, {}], [{ magic: '"CX17"' }, {}], [{ magic: '"CX1"' }, {}],
    [{ ver: "2" }, {}], [{ flags: "1", mf: "3" }, {}], [{ ie: "8" }, {}], [{ be: "image_end - 2" }, {}], [{ be: "$E002" }, {}],
    [{ be: "$E000" }, {}], [{ ie: "image_end + 600" }, {}], [{ jmp: "JMP     #start + 1" }, {}], [{ jmp: "JMP     #4" }, {}],
    [{ jmp: "JMP     #image_end" }, {}], [{}, { tail: "        .byte 1\nimage_end:\nbss_end:\n        .align 2\n" }],
    [{}, { tail: "image_end:\n        .reserve 2\n        .byte 7\nbss_end:\n" }], [{}, { code: "        .reserve 3\n" }],
    [{}, { tail: "image_end:\nbss_end:\n        .org $100\n        .byte 5\n" }], [{}, { tail: "image_end:\n.reserve $E000\nbss_end:\n" }],
  ];
  for (const [h, b] of variants) add(hdr(h) + body(b));
  add(`        JMP     #$1C\n        .ascii  "CX16"\n`);
  add(`        .reserve 40\n`);
  add(`        .org    $10\n        .reserve 8\n        .org    $14\n        NOP\n`);
  add(`        .org    $FFF0\n        .reserve 16\nend:\n`);
  add(`        NOP\nx:      .reserve 4, 1\n`);
}
// ---- 7. .include
const incDir = fs.mkdtempSync(path.join(os.tmpdir(), "cpu16inc-"));
{
  const files = [
    [`${incDir}/a/main.asm`, `start: LD R0, [val]\n.include "lib/inc.asm"\n  WFI\n`],
    [`${incDir}/a/lib/inc.asm`, `val: .word $ABCD\n.include "deeper.asm"\n`],
    [`${incDir}/a/lib/deeper.asm`, `deep: .byte 1, 2\n  JMP start\n`],
    [`${incDir}/b/main.asm`, `.include "missing.asm"\nNOP\n`],
    [`${incDir}/c/rec.asm`, `NOP\n.include "rec.asm"\n`],
    [`${incDir}/d/main.asm`, `.include "err.asm"\n`],
    [`${incDir}/d/err.asm`, `NOP\nBAD R0\nJMP nowhere\n`],
  ];
  for (const [p, t] of files) { fs.mkdirSync(path.dirname(p), { recursive: true }); fs.writeFileSync(p, t); }
  const enc = (list) => list.map(([p, t]) => `${p}=${Buffer.from(t).toString("base64")}`).join(";");
  add("", `${incDir}/a/main.asm`, enc(files.slice(0, 3)));
  add("", `${incDir}/b/main.asm`, enc(files.slice(3, 4)));
  add("", `${incDir}/c/rec.asm`, enc(files.slice(4, 5)));
  add("", `${incDir}/d/main.asm`, enc(files.slice(5, 7)));
  add("", `${incDir}/nofile.asm`, `${incDir}/zz/placeholder.txt=${Buffer.from("x").toString("base64")}`);
}

// ---- the reference
const srcBytes = (c) => Buffer.from(c.src, c.binary ? "latin1" : "utf8");
const tsv = corpus.map((c, i) => `${i}\t${c.name}\t${srcBytes(c).toString("base64")}\t${c.files || ""}`).join("\n") + "\n";
// not trim(): the last line may have empty trailing fields (tabs)
const out = execFileSync(oracle, [], { input: tsv, maxBuffer: 1 << 30 }).toString("utf8").replace(/\n$/, "").split("\n");
const expected = new Map(out.map((l) => { const f = l.split("\t"); return [Number(f[0]), f]; }));

// ---- comparison
const b64 = (s) => Buffer.from(s, "latin1").toString("base64");   // a byte string -> base64
const fnv = (h, v) => Math.imul(h ^ v, 16777619) >>> 0;
let ok = 0, okAssembled = 0, bad = 0, totalDiags = 0;
const samples = [];
corpus.forEach((c, i) => {
  const a = new Cpu16Assembler((p) => (fs.existsSync(p) ? fs.readFileSync(p, "utf8") : null));
  // a source with invalid UTF-8 is passed as bytes, as in C++ (assembleText expects a JS string, so through processText)
  let res;
  if (c.files) res = a.assembleFile(c.name);
  else if (c.binary) { a.processText(Cpu16Assembler.bytes(c.name), srcBytes(c).toString("latin1"), 0); res = a.finish(); }
  else res = a.assembleText(c.name, c.src);
  let h = 2166136261;
  for (let x = 0; x < 65536; x++) if (a.owner[x] >= 0) { h = fnv(h, x); h = fnv(h, a.image[x]); }
  const got = [String(i), res ? "1" : "0", b64(a.formatDiags()), String(a.minAddr), String(a.maxAddr), String(h),
               b64(a.listing()), b64(a.memoryMap()),
               b64(res ? a.checkExec().map((m) => m + "\n").join("") : "")];
  const exp = expected.get(i);
  const fields = ["id", "ok", "diagnostics", "minAddr", "maxAddr", "image", "listing", "map", "exec"];
  const diff = fields.filter((_, k) => got[k] !== exp[k]);
  totalDiags += a.diags.length;
  if (!diff.length) { ok++; if (res) okAssembled++; return; }
  bad++;
  if (samples.length < 6) {
    const dec = (x) => Buffer.from(x, "base64").toString("utf8");
    samples.push(`case ${i} differs in ${diff.join(", ")}\n  source: ${JSON.stringify(c.src.slice(0, 160))}` +
                 (diff.includes("diagnostics") ? `\n  C++: ${JSON.stringify(dec(exp[2]).slice(0, 300))}\n  JS:  ${JSON.stringify(dec(got[2]).slice(0, 300))}` : ""));
  }
});
fs.rmSync(incDir, { recursive: true, force: true });
for (const s of samples) console.log("FAIL:", s);
console.log(`assembler vs cpu16asm: ${corpus.length} sources (${okAssembled} assemble cleanly, ${totalDiags} diagnostics in total), ` +
            `${ok} identical, ${bad} different`);
console.log(bad ? "FAILED" : "OK");
process.exit(bad ? 1 : 0);
