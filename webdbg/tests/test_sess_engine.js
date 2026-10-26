// Monitor sessions on the web debugger engine: loading a ready system, Ctrl+] and Ctrl+[ as the debugger
// sends them (scancode ] or [ with Ctrl in MODS), a session's screen through the SCRD register (the machine, revision 9).
// Volumes by letters (A:), the session number in the prompt ("[1] A:/> ") only with several sessions.
// Run: node tests/test_sess_engine.js [system.img]
"use strict";
const fs = require("fs"), path = require("path"), assert = require("assert");
const base = path.join(__dirname, "..");
const html = fs.readFileSync(path.join(base, "cpu16_debugger.html"), "utf8");
const code = html.slice(html.indexOf("// BEGIN ENGINE"), html.indexOf("// END ENGINE"));
const { MACHINE, Cpu16Engine } = new Function(code + "\nreturn { MACHINE, Cpu16Engine };")();
const rom = Uint8Array.from(JSON.parse(/const BOOT_ROM = new Uint8Array\((\[.*?\])\);/.exec(html)[1]));
const imgPath = process.argv[2] || path.join(base, "..", "monitor", "dist", "system.img");

const e = new Cpu16Engine();
e.attachDisk(0, Uint8Array.from(fs.readFileSync(imgPath)), "system.img");
e.diskDelayMs = 0;
let vt = 0;                                     // virtual timer clock, microseconds
e.dev.clock = () => vt;
e.clearMemory(); e.loadImage(rom, 0); e.reset();
e.setClockFrozen(false);

let out = "", seen = 0;
function run(until, what, budget = 3000) {
  for (let i = 0; i < budget; i++) {
    const s = e.runChunk(4096, new Set());
    out += e.takeConsoleOutput();
    if (until && until()) return;
    if (s.reason === "waiting") { vt = Math.max(vt, e.dev.nextTick); e.pollTimerNow(); }   // idle: the time is up to the tick, as in the interface
    else if (s.reason === "limit") vt += 2000;
    else if (!until) return;
    else throw new Error(`${what}: stopped (${s.reason}), output: ${JSON.stringify(out.slice(-300))}`);
  }
  if (until) throw new Error(`${what}: timeout, output: ${JSON.stringify(out.slice(-300))}`);
}
// wait for text in the output after the previous match
function waitOut(text) {
  run(() => out.indexOf(text, seen) >= 0, JSON.stringify(text));
  seen = out.indexOf(text, seen) + text.length;
}
// n ticks: in idle the interface polls the timer itself (pollTimerNow)
const idle = (n) => {
  for (let k = 0; k < n; k++) {
    vt += 20000;
    if (e.runChunk(20000, new Set()).reason === "waiting") e.pollTimerNow();
    e.runChunk(20000, new Set());
    out += e.takeConsoleOutput();
  }
};
const screen = () => {
  const rows = [];
  for (let r = 0; r < MACHINE.ROWS; r++) {
    let l = "";
    for (let c = 0; c < MACHINE.COLS; c++) l += String.fromCharCode(e.dev.chars[r * MACHINE.COLS + c]);
    rows.push(l.trimEnd());
  }
  return rows.join("\n").trimEnd();
};
const state = () => [e.dev.ctrl, e.dev.attr, e.dev.col, e.dev.row].join(",");
const CTRL = MACHINE.MOD_CTRL;
let n = 0;
const ok = (cond, what) => { if (!cond) console.log("SCREEN:\n" + screen() + "\nSTATE " + state()); assert(cond, what); n++; console.log("ok   " + what); };

waitOut("A:/> ");
e.feedText("echo first\n");
waitOut("first\nA:/> ");
ok(!/\[\d\]/.test(screen()), "one session: no session number in the prompt");
e.feedText("session new -b\n");
waitOut("session 1 started\n[0] A:/> ");
idle(3);
const s0 = screen(), st0 = state();
ok(/echo first\nfirst\nA:\/> session new -b\nsession 1 started\n\[0\] A:\/>$/.test(s0), "session 0: screen before the switch, prompt [0]");

e.feedKey(0, MACHINE.scancodes.BracketRight, CTRL);          // Ctrl+]: the next session
waitOut("[1] A:/> ");                                        // session 1's shell starts working
idle(3);
ok(screen() === "[1] A:/>", "Ctrl+]: session 1 with its own empty screen, prompt [1]");
e.feedText("session\n");
waitOut("* 1");
waitOut("[1] A:/> ");
idle(3);
ok(/^\[1\] A:\/> session\n  0    56K  A:\/sys\/shell.cx\n\* 1    56K  A:\/sys\/shell.cx\n\[1\] A:\/>$/.test(screen()), "session 1: list of sessions");

e.feedKey(0, MACHINE.scancodes.BracketLeft, CTRL);           // Ctrl+[: the previous session
idle(40);
ok(screen() === s0 && state() === st0, "Ctrl+[: session 0 screen, mode and position restored (SCRD)");
e.feedText("echo back\n");
waitOut("back\n[0] A:/> ");
ok(e.dev.keys.length === 0 && screen().endsWith("echo back\nback\n[0] A:/>"), "session 0: keyboard goes to the restored session");

e.feedText("exit\n");                                        // the root of session 0: session 1 becomes current
idle(40);
// session 1's screen is as it was left: the [1] prompt was printed when there were two sessions
ok(/^\[1\] A:\/> session\n  0 .*\n\* 1 .*\n\[1\] A:\/>$/.test(screen()), "exit in session 0: session 1 is current");
e.feedText("echo solo\n");
waitOut("solo\nA:/> ");
ok(screen().endsWith("[1] A:/> echo solo\nsolo\nA:/>"), "one session left: the next prompt has no session number");
e.feedText("exit\n");                                        // the last session: halt
waitOut("monitor: halted");
ok(true, "exit in the last session halts the system");
console.log(`OK: ${n} session checks on the JS engine`);
