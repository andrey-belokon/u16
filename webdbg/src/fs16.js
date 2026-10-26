// FS16 revision 3: browser-side read/write filesystem for in-memory CPU16 disk images.
// No dependency on DOM or the emulated CPU. All offsets are byte offsets in the image.
class Fs16 {
  static CLUSTER = 2048;
  static DIR_ENTRY = 64;
  static SIZE_MASK = 0x07FFFFFF;
  static DIR = 0x80000000;
  static RDONLY = 0x40000000;
  static HIDDEN = 0x20000000;
  static SYSTEM = 0x10000000;

  constructor(image, readOnly = false, charset = null) {
    if (!(image instanceof Uint8Array)) throw new Error('Disk image must be Uint8Array');
    this.image = image;
    this.readOnly = !!readOnly;
    this.charset = charset;
    this.encoder = new Map();
    if (charset) charset.forEach((cp, i) => { if (cp && !this.encoder.has(String.fromCodePoint(cp))) this.encoder.set(String.fromCodePoint(cp), i); });
    else {
      for (let c = 32; c <= 126; c++) this.encoder.set(String.fromCharCode(c), c);
      for (let c = 0; c < 32; c++) this.encoder.set(String.fromCharCode(0x410 + c), 0xC0 + c);
      for (let c = 0; c < 32; c++) this.encoder.set(String.fromCharCode(0x430 + c), 0xE0 + c);
      this.encoder.set('ё', 0xA3); this.encoder.set('Ё', 0xB3);
    }
    this.validate();
  }
  // Names are compared ignoring letter case (FS16 section 7.1): A-Z, Cyrillic capitals and capital Yo fold to lower case.
  static foldName(s) {
    let r = '';
    for (const ch of s) {
      const c = ch.codePointAt(0);
      r += (c >= 0x41 && c <= 0x5A) || (c >= 0x410 && c <= 0x42F) ? String.fromCodePoint(c + 0x20) : ch === 'Ё' ? 'ё' : ch;
    }
    return r;
  }
  static sameName(a, b) { return Fs16.foldName(a) === Fs16.foldName(b); }
  u16(o) { return this.image[o] | (this.image[o + 1] << 8); }
  u32(o) { return (this.u16(o) | (this.u16(o + 2) << 16)) >>> 0; }
  w16(o, v) { this.image[o] = v & 255; this.image[o + 1] = (v >>> 8) & 255; }
  w32(o, v) { this.w16(o, v); this.w16(o + 2, v >>> 16); }
  checksum() {
    this.w16(510, 0);
    let sum = 0;
    for (let o = 0; o < 510; o += 2) sum = (sum + this.u16(o)) & 65535;
    this.w16(510, (-sum) & 65535);
  }
  validate() {
    if (this.image.length < 6144 || this.image.length % 512) throw new Error('Invalid disk size for FS16');
    if (String.fromCharCode(...this.image.subarray(0, 4)) !== 'FS16') throw new Error('No FS16 superblock (unformatted disk)');
    if (this.u16(4) !== 1) throw new Error('Unsupported FS16 version');
    this.N = this.u16(6); this.B = this.u16(8);
    if (this.N < 3 || this.N > 65535 || this.N * 2048 > this.image.length ||
        this.B !== Math.ceil(this.N / 16384)) throw new Error('Invalid FS16 volume geometry');
    let sum = 0;
    for (let o = 0; o < 512; o += 2) sum = (sum + this.u16(o)) & 65535;
    if (sum) throw new Error('Invalid FS16 superblock checksum');
    if (!(this.u32(36 + 32) & Fs16.DIR)) throw new Error('Invalid FS16 root directory');
    if (this.u16(14) & ~1) throw new Error('Unsupported FS16 flags');
    this.dirty = !!(this.u16(14) & 1);
    this.mapBytes = this.B * Fs16.CLUSTER;
  }
  get label() { return this.decode(this.image.subarray(20, 36)); }
  get freeClusters() {
    let free = 0;
    for (let c = this.B + 1; c < this.N; c++) if (!this.bit(c)) free++;
    return free;
  }
  static format(image, label = '', charset = null) {
    if (!(image instanceof Uint8Array) || image.length < 6144 || image.length % 512)
      throw new Error('FS16 requires at least 12 sectors and a 512-byte sector size');
    const N = Math.min(Math.floor(image.length / 2048), 65535), B = Math.ceil(N / 16384);
    image.fill(0);
    image.set([70, 83, 49, 54]);
    const fs = Object.create(Fs16.prototype);
    fs.image = image; fs.N = N; fs.B = B; fs.mapBytes = B * 2048;
    fs.w16(4, 1); fs.w16(6, N); fs.w16(8, B); fs.w16(10, N - B - 1);
    fs.w16(12, B + 1); fs.w16(14, 0);
    fs.w32(16, ((Date.now() ^ (N << 11)) >>> 0));
    const codec = Object.create(Fs16.prototype);
    codec.charset = charset;
    codec.encoder = new Map();
    if (charset) charset.forEach((cp, i) => { if (cp && !codec.encoder.has(String.fromCodePoint(cp))) codec.encoder.set(String.fromCodePoint(cp), i); });
    else for (let i = 32; i <= 126; i++) codec.encoder.set(String.fromCharCode(i), i);
    let written = 0;
    for (const ch of label) { const b = codec.encoder.get(ch); if (b === undefined || written >= 16) break; image[20 + written++] = b; }
    fs.w32(36 + 32, Fs16.DIR); // root entry, empty directory
    for (let c = 0; c <= B; c++) fs._setBit(c, true);
    for (let c = N; c < B * 16384; c++) fs._setBit(c, true);
    fs.checksum();
    return new Fs16(image, false, charset);
  }
  decode(a) {
    const last = a.indexOf(0), bytes = last < 0 ? a : a.subarray(0, last);
    return Array.from(bytes, b => this.charset ? String.fromCodePoint(this.charset[b] || 0xFFFD) :
      (b >= 0xC0 ? String.fromCharCode(0x410 + ((b - 0xC0) & 31) + (b >= 0xE0 ? 32 : 0)) :
      b === 0xA3 ? 'ё' : b === 0xB3 ? 'Ё' : String.fromCharCode(b))).join('');
  }
  encodeName(name) {
    if (!name || name === '.' || name === '..' || name.includes('/') || name.includes(':'))
      throw new Error(`Invalid FS16 name: ${name}`);
    const bytes = [];
    for (const ch of name) {
      const b = this.encoder.get(ch);
      if (b === undefined || b < 32 || b === 47 || b === 58) throw new Error(`Character “${ch}” cannot be used in FS16 names`);
      bytes.push(b);
      if (bytes.length > 32) throw new Error(`FS16 names cannot exceed 32 bytes: ${name}`);
    }
    return new Uint8Array(bytes);
  }
  _entry(offset) {
    const flagsSize = this.u32(offset + 32);
    return { offset, name: this.decode(this.image.subarray(offset, offset + 32)),
      size: flagsSize & Fs16.SIZE_MASK, flags: (flagsSize & ~Fs16.SIZE_MASK) >>> 0,
      dir: !!(flagsSize & Fs16.DIR), readonly: !!(flagsSize & Fs16.RDONLY),
      mtime: this.u32(offset + 36), direct: Array.from({length:10}, (_, i) => this.u16(offset + 40 + 2*i)),
      ind: this.u16(offset + 60), dind: this.u16(offset + 62) };
  }
  _checkCluster(c) {
    if (c < this.B + 1 || c >= this.N) throw new Error(`Invalid filesystem cluster: ${c}`);
    return c * Fs16.CLUSTER;
  }
  _idx(c, index) {
    return this.u16(this._checkCluster(c) + index * 2);
  }
  _dataCluster(e, k) {
    let c;
    if (k < 10) c = e.direct[k];
    else if (k < 1034) c = e.ind && this._idx(e.ind, k - 10);
    else {
      const j = k - 1034;
      const indirect = e.dind && this._idx(e.dind, Math.floor(j / 1024));
      c = indirect && this._idx(indirect, j % 1024);
    }
    if (!c) throw new Error(`Missing data cluster at ${k} in ${e.name || '/'}`);
    this._checkCluster(c);
    return c;
  }
  _getDir(path = []) {
    let e = this._entry(36);
    for (const name of path) {
      if (!e.dir) throw new Error('Not a directory');
      const child = this._entries(e).find(x => Fs16.sameName(x.name, name));
      if (!child) throw new Error(`Directory not found: ${name}`);
      if (!child.dir) throw new Error(`Not a directory: ${name}`);
      e = child;
    }
    return e;
  }
  _entries(dir) {
    if (!dir.dir || dir.size % Fs16.CLUSTER) throw new Error('Invalid FS16 directory');
    const arr = [];
    for (let k = 0; k < dir.size / Fs16.CLUSTER; k++) {
      const pos = this._checkCluster(this._dataCluster(dir, k));
      for (let j = 0; j < 32; j++) if (this.image[pos + j * 64]) arr.push(this._entry(pos + j * 64));
    }
    return arr;
  }
  list(path = []) {
    return this._entries(this._getDir(path)).sort((a, b) => (+b.dir - +a.dir) || a.name.localeCompare(b.name));
  }
  readFile(entry) {
    if (entry.dir) throw new Error('Cannot read a directory as a file');
    const out = new Uint8Array(entry.size);
    for (let k = 0, o = 0; o < out.length; k++, o += Fs16.CLUSTER) {
      const p = this._checkCluster(this._dataCluster(entry, k));
      out.set(this.image.subarray(p, p + Math.min(Fs16.CLUSTER, out.length - o)), o);
    }
    return out;
  }
  bit(c) { return !!(this.image[2048 + (c >> 3)] & (1 << (c & 7))); }
  _setBit(c, val) { const i = 2048 + (c >> 3), mask = 1 << (c & 7); this.image[i] = val ? (this.image[i] | mask) : (this.image[i] & ~mask); }
  _alloc() {
    let hint = this.u16(12); if (hint <= this.B || hint >= this.N) hint = this.B + 1;
    for (let i = 0, c = hint; i < this.N - this.B - 1; i++, c++) {
      if (c >= this.N) c = this.B + 1;
      if (!this.bit(c)) {
        this._setBit(c, true); this._allocated.push(c);
        this.w16(10, Math.max(0, this.u16(10) - 1));
        this.w16(12, c + 1 < this.N ? c + 1 : this.B + 1);
        return c;
      }
    }
    throw new Error('Disk full');
  }
  _free(c) {
    this._checkCluster(c);
    if (!this.bit(c)) throw new Error(`Double-free cluster ${c}`);
    this._setBit(c, false);
    this.w16(10, this.u16(10) + 1);
    this.w16(12, c);
  }
  _writeEntry(e) {
    const o = e.offset;
    const name = e.name ? this.encodeName(e.name) : new Uint8Array(0);
    this.image.fill(0, o, o + 64);
    this.image.set(name, o);
    this.w32(o + 32, (e.size | e.flags) >>> 0);
    this.w32(o + 36, e.mtime);
    for (let i = 0; i < 10; i++) this.w16(o + 40 + i*2, e.direct[i]);
    this.w16(o + 60, e.ind); this.w16(o + 62, e.dind);
  }
  _newEntry(offset, name, dir) {
    return { offset, name, size: 0, flags: dir ? Fs16.DIR : 0, dir: !!dir, readonly: false,
      mtime: Fs16.timestamp(), direct: Array(10).fill(0), ind: 0, dind: 0 };
  }
  static timestamp(date = new Date()) {
    const y = date.getFullYear(); if (y < 2025 || y > 2088) return 0;
    const time = ((date.getHours() % 12) << 12) | (date.getMinutes() << 6) | date.getSeconds();
    const day = ((y - 2025) << 10) | ((date.getMonth() + 1) << 6) | (date.getDate() << 1) | +(date.getHours() >= 12);
    return ((day << 16) | time) >>> 0;
  }
  _setPointer(e, k, cluster) {
    if (k < 10) { e.direct[k] = cluster; return; }
    if (k < 1034) {
      if (!e.ind) { e.ind = this._alloc(); this.image.fill(0, e.ind*2048, e.ind*2048 + 2048); }
      this.w16(this._checkCluster(e.ind) + (k - 10)*2, cluster); return;
    }
    const j = k - 1034;
    if (!e.dind) { e.dind = this._alloc(); this.image.fill(0, e.dind*2048, e.dind*2048 + 2048); }
    const idx = Math.floor(j / 1024);
    let ind = this._idx(e.dind, idx);
    if (!ind) { ind = this._alloc(); this.image.fill(0, ind*2048, ind*2048 + 2048); this.w16(this._checkCluster(e.dind) + idx*2, ind); }
    this.w16(this._checkCluster(ind) + (j % 1024)*2, cluster);
  }
  _growthCost(dir) {
    if (this._entries(dir).length < dir.size / 64) return 0;
    const k = dir.size / 2048;
    return 1 + (k === 10 ? 1 : 0) + (k === 1034 ? 2 : (k > 1034 && (k - 1034) % 1024 === 0 ? 1 : 0));
  }
  _slot(dir) {
    for (let k = 0; k < dir.size / 2048; k++) {
      const pos = this._checkCluster(this._dataCluster(dir, k));
      for (let j = 0; j < 32; j++) if (!this.image[pos + j*64]) return pos + j*64;
    }
    const k = dir.size / 2048, c = this._alloc();
    this.image.fill(0, c*2048, c*2048 + 2048);
    this._setPointer(dir, k, c);
    dir.size += 2048; dir.mtime = Fs16.timestamp();
    this._writeEntry(dir);
    return c*2048;
  }
  _begin() {
    if (this.readOnly) throw new Error('Disk is read-only');
    if (this.dirty) throw new Error('FS16 volume is DIRTY: check/unmount it on the guest before writing from the host');
    if (this._allocated) throw new Error('Nested write operation');
    this._allocated = [];
    this.w16(14, 1); this.checksum(); this.dirty = true;
  }
  _end() {
    this._allocated = null;
    this.w16(14, 0); this.checksum(); this.dirty = false;
  }
  _transaction(fn) {
    this._begin();
    try { const result = fn(); this._end(); return result; }
    catch (err) {
      // A failed operation may already have modified indexes or bitmap; keep DIRTY
      // so a filesystem check is required before a subsequent write.
      this._allocated = null;
      this.checksum();
      throw err;
    }
  }
  mkdir(path, name) {
    this.encodeName(name);
    const parent = this._getDir(path);
    if (parent.readonly) throw new Error('Parent directory is read-only');
    if (this._entries(parent).some(e => Fs16.sameName(e.name, name))) throw new Error(`Already exists: ${name}`);
    if (this._growthCost(parent) > this.freeClusters) throw new Error('Not enough free clusters for directory');
    return this._transaction(() => {
      const slot = this._slot(parent), e = this._newEntry(slot, name, true);
      this._writeEntry(e); parent.mtime = Fs16.timestamp(); this._writeEntry(parent);
      return e;
    });
  }
  writeFile(path, name, data) {
    if (!(data instanceof Uint8Array)) data = new Uint8Array(data);
    if (data.length > Fs16.SIZE_MASK) throw new Error('File exceeds FS16 maximum size');
    this.encodeName(name);
    const parent = this._getDir(path);
    if (parent.readonly) throw new Error('Parent directory is read-only');
    if (this._entries(parent).some(e => Fs16.sameName(e.name, name))) throw new Error(`Already exists: ${name}`);
    const clusters = Math.ceil(data.length / Fs16.CLUSTER);
    const second = Math.max(0, clusters - 1034);
    const indexes = (clusters > 10 ? 1 : 0) + (second ? 1 + Math.ceil(second / 1024) : 0);
    const dirClusters = this._growthCost(parent);
    if (clusters + indexes + dirClusters > this.freeClusters) throw new Error('Not enough free clusters');
    return this._transaction(() => {
      // Data and bitmap are committed BEFORE publishing the new directory entry.
      const e = this._newEntry(0, name, false);
      for (let k = 0, p = 0; p < data.length; k++, p += Fs16.CLUSTER) {
        const c = this._alloc(), dst = c*Fs16.CLUSTER;
        this.image.set(data.subarray(p, p + Fs16.CLUSTER), dst);
        this._setPointer(e, k, c);
      }
      e.size = data.length;
      e.offset = this._slot(parent);
      this._writeEntry(e);
      parent.mtime = Fs16.timestamp(); this._writeEntry(parent);
      return e;
    });
  }
  _allClusters(e) {
    const out = [];
    const count = Math.ceil(e.size / Fs16.CLUSTER);
    for (let k = 0; k < count; k++) out.push(this._dataCluster(e, k));
    if (e.ind) out.push(e.ind);
    if (e.dind) {
      for (let i = 0, n = Math.ceil(Math.max(0, count - 1034) / 1024); i < n; i++) out.push(this._idx(e.dind, i));
      out.push(e.dind);
    }
    return out;
  }
  remove(path, name) {
    const parent = this._getDir(path);
    if (parent.readonly) throw new Error('Parent directory is read-only');
    const e = this._entries(parent).find(e => Fs16.sameName(e.name, name));
    if (!e) throw new Error('Entry not found');
    if (e.readonly) throw new Error('Read-only FS16 entry');
    if (e.dir && this._entries(e).length) throw new Error('Directory is not empty');
    const clusters = this._allClusters(e);
    return this._transaction(() => {
      this.image.fill(0, e.offset, e.offset + 64);
      parent.mtime = Fs16.timestamp(); this._writeEntry(parent);
      for (const c of clusters) this._free(c);
    });
  }
}
