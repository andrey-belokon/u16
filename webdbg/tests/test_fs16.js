// Run: node webdbg/tests/test_fs16.js
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
vm.runInThisContext(fs.readFileSync(__dirname + '/../src/fs16.js', 'utf8') + '\nglobalThis.Fs16 = Fs16;');
function data(n) { return Uint8Array.from({length:n}, (_,i)=> (i*13 + (i>>>7)) & 255); }
function disk(n=6000) { return Fs16.format(new Uint8Array(n*2048), 'TESTDISK'); }
function match(a,b) { assert.equal(Buffer.compare(a,b),0); }
let fsys = disk();
assert.equal(fsys.N, 6000); assert.equal(fsys.label, 'TESTDISK');
assert.deepEqual(fsys.list(), []);
const lengths = [0, 1, 511, 512, 2047, 2048, 2049, 10*2048, 10*2048+1, 1024*2048, 1034*2048, 1034*2048 + 1, 1040*2048+191];
for (let i=0; i<lengths.length; i++) {
 const bytes = data(lengths[i]);
 fsys.writeFile([], `SAMPLE${i}.BIN`, bytes);
 match(fsys.readFile(fsys.list().find(e=>e.name===`SAMPLE${i}.BIN`)),bytes);
 fsys = new Fs16(fsys.image); // validate checksum after every operation
}
assert.equal(fsys.u16(14) & 1,0);
assert.equal(fsys.freeClusters,fsys.u16(10));
fsys.mkdir([], 'SRC'); fsys.mkdir(['SRC'],'NEST');
fsys.writeFile(['SRC','NEST'], 'привет.txt', data(1537));
match(fsys.readFile(fsys.list(['SRC','NEST'])[0]), data(1537));
assert.throws(()=>fsys.writeFile(['SRC','NEST'], 'привет.txt', new Uint8Array(0)), /Already exists/);
assert.throws(()=>fsys.mkdir([], 'SRC'), /Already exists/);
assert.throws(()=>fsys.remove([], 'SRC'), /not empty/);
// names ignore letter case (FS16 section 7.1), the stored spelling is kept
assert.throws(()=>fsys.mkdir([], 'src'), /Already exists/);
assert.throws(()=>fsys.writeFile(['Src','nest'], 'ПРИВЕТ.TXT', new Uint8Array(0)), /Already exists/);
assert.equal(fsys.list(['src','NeSt'])[0].name, 'привет.txt');
assert.ok(Fs16.sameName('Ёлка', 'ёЛКА') && !Fs16.sameName('ёж', 'еж'));
for (let i=0;i<42;i++) fsys.writeFile([], `F${i}`, new Uint8Array([i]));
assert.equal(fsys.list().length, lengths.length+1+42);
assert.equal(fsys._entry(36).size, 4096);
const toDelete=fsys.list(['SRC','NEST'])[0];fsys.remove(['SRC','NEST'],toDelete.name);
fsys.remove(['SRC'],'NEST');fsys.remove([],'SRC');
assert.equal(fsys.freeClusters,fsys.u16(10));
const small = Fs16.format(new Uint8Array(16*512));
assert.equal(small.N,4); small.writeFile([], 'ONE', data(2048));
assert.throws(()=>small.writeFile([], 'TWO', data(1)), /Not enough free clusters/);
const readonly = new Fs16(fsys.image,true); assert.throws(()=>readonly.mkdir([], 'X'), /read-only/);
const marked = fsys.image.slice(); const m = new Fs16(marked); m.w16(14, 1); m.checksum();
const dirty = new Fs16(marked); assert.equal(dirty.dirty, true);
assert.doesNotThrow(()=>dirty.list()); assert.throws(()=>dirty.writeFile([], 'X', new Uint8Array()), /DIRTY/);
const broken = fsys.image.slice(); broken[14]=1; broken[510] = 0; broken[511] = 0;
assert.throws(()=>new Fs16(broken), /checksum/);
// Test DIND and second-level allocation with a larger disk.
const big = Fs16.format(new Uint8Array(1800*2048));
const large = data(1040*2048+1);
big.writeFile([], 'BIG.DAT', large);
const bigEntry = big.list()[0];
assert.ok(bigEntry.ind && bigEntry.dind);
match(big.readFile(bigEntry), large);
big.remove([], 'BIG.DAT');
assert.equal(big.freeClusters, big.N - big.B - 2);
console.log('FS16 tests passed: sizes, indirect and double indirect, nested directories, deletion, validation, read-only, disk full.');
