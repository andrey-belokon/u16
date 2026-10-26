// FS16 disk manager opens in its own browser window (one per drive).
// Local files are only a staging list: browser security does not allow reading
// arbitrary host folders or writing to a host folder without a download.
const fs16Windows = new Map();
function openFs16Manager(diskNumber) {
  const old = fs16Windows.get(diskNumber);
  if (old && !old.win.closed) { old.win.focus(); old.refresh(); return; }
  const win = window.open('', `cpu16-files-${diskNumber}`, 'width=990,height=670,resizable=yes,scrollbars=yes');
  if (!win) { setMsg('Allow popup windows to open the FS16 file manager.'); return; }
  const manager = new Fs16Manager(win, diskNumber);
  fs16Windows.set(diskNumber, manager);
  win.addEventListener('beforeunload', () => { if (fs16Windows.get(diskNumber) === manager) fs16Windows.delete(diskNumber); });
  manager.refresh();
}
class Fs16Manager {
  constructor(win, n) {
    this.win = win; this.n = n; this.path = [];
    this.selected = new Set(); this.last = -1;
    this.local = []; this.localSelected = new Set(); this.lastLocal = -1;
    this.fs = null; this.entries = [];
    this.pending = false;
    this.initialize();
  }
  $(id) { return this.win.document.getElementById(id); }
  initialize() {
    const d = this.win.document;
    d.open();
    d.write(`<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>FS16 Disk ${this.n} — File Manager</title><style>
:root { color-scheme:light dark; font-family:system-ui,"Segoe UI",sans-serif; font-size:13px; }
* {box-sizing:border-box} body{margin:0;display:flex;flex-direction:column;height:100vh;background:Canvas;color:CanvasText}
header{padding:12px 15px;border-bottom:1px solid #8994a555;display:flex;align-items:center;gap:12px;flex-wrap:wrap}
header strong{font-size:16px} #summary{opacity:.75} #warning{color:#bf7121;padding:0 15px;min-height:20px}
main{display:grid;grid-template-columns:minmax(0,1fr) minmax(0,1fr);gap:12px;padding:10px 14px;flex:1;min-height:0}
section{border:1px solid #8994a588;border-radius:7px;display:flex;flex-direction:column;min-width:0;overflow:hidden}
section h2{font-size:14px;font-weight:600;margin:0} .head{padding:9px;display:flex;align-items:center;gap:8px;min-height:44px;border-bottom:1px solid #8994a555;flex-wrap:wrap}
.spacer{flex:1} button{font:inherit;padding:5px 9px;background:ButtonFace;color:ButtonText;border:1px solid #8994a588;border-radius:4px;cursor:pointer}
button:hover{filter:brightness(.96)}button:disabled{opacity:.4;cursor:default}
.path{font:12px ui-monospace,Consolas,monospace;border-bottom:1px solid #8994a555;padding:5px 10px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;min-height:27px}
.list{flex:1;overflow:auto;min-height:100px} .item{display:grid;grid-template-columns:22px minmax(0,1fr) 85px;gap:8px;align-items:center;min-height:29px;padding:4px 10px;cursor:default;user-select:none;border-bottom:1px solid #8994a51c}
.item:hover{background:#80808019}.item.selected{background:color-mix(in srgb,Highlight 20%,transparent)}
.item b{font-weight:400;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}.item small{text-align:right;opacity:.7;font-variant-numeric:tabular-nums}
.item span{font-size:17px} .item[draggable=true]{cursor:grab}
.list.dropover{outline:3px dashed Highlight;outline-offset:-4px;background:#80808012}
.foot{border-top:1px solid #8994a555;min-height:42px;display:flex;align-items:center;flex-wrap:wrap;padding:6px 9px;gap:6px}
#message{padding:7px 15px;border-top:1px solid #8994a555;white-space:pre-wrap;min-height:40px;max-height:110px;overflow:auto}
.hint{opacity:.72;font-size:12px;padding:5px 10px;border-top:1px solid #8994a555}
input[type=file]{display:none}
@media(max-width:640px){main{grid-template-columns:1fr;overflow:auto}section{min-height:260px}header{padding:8px} }
</style></head><body><header><strong>FS16 file manager · Disk ${this.n}</strong><span id="summary"></span>
<button id="refresh">Refresh</button><button id="format" hidden>Format FS16…</button></header>
<div id="warning"></div><main>
<section><div class="head"><h2>Computer</h2><span class="spacer"></span><button id="pick">Add files…</button><button id="folder">Add folder…</button></div>
<div class="path">Staged files (drag files here from your computer)</div><div class="list" id="local-list" aria-label="Files staged from computer"></div>
<div class="hint">Drag files to the right, or select and click Copy →</div>
<div class="foot"><button id="upload">Copy →</button><button id="clear">Clear staging</button></div></section>
<section><div class="head"><h2>FS16 disk</h2><span class="spacer"></span><button id="up">↑ Up</button><button id="mkdir">New folder</button></div>
<div class="path" id="path">/</div><div class="list" id="disk-list" aria-label="FS16 directory contents"></div>
<div class="hint">Double-click a folder to open. Ctrl/Shift to select several files.</div>
<div class="foot"><button id="download">← Download</button><button id="remove">Delete</button><span class="spacer"></span><span id="counter"></span></div></section>
</main><div id="message" role="status">Drop files or choose files to copy into the FS16 image. Changes remain in memory until the disk image is downloaded.</div>
<input type="file" id="files" multiple><input type="file" id="folders" multiple webkitdirectory directory>
</body></html>`);
    d.close();
    this.$('refresh').onclick = () => this.refresh();
    this.$('up').onclick = () => { if (this.path.length) { this.path.pop(); this.selected.clear(); this.refresh(); } };
    this.$('pick').onclick = () => this.$('files').click();
    this.$('folder').onclick = () => this.$('folders').click();
    this.$('files').onchange = (e) => { this.stage(e.target.files); e.target.value = ''; };
    this.$('folders').onchange = (e) => { this.stage(e.target.files); e.target.value = ''; };
    this.$('clear').onclick = () => { this.local = []; this.localSelected.clear(); this.renderLocal(); };
    this.$('upload').onclick = () => this.upload();
    this.$('download').onclick = () => this.download();
    this.$('mkdir').onclick = () => this.makeDirectory();
    this.$('remove').onclick = () => this.remove();
    this.$('format').onclick = () => this.format();
    this.$('disk-list').addEventListener('click', e => this.clickDisk(e));
    this.$('disk-list').addEventListener('dblclick', e => {
      const row = e.target.closest('[data-index]');
      if (row && this.entries[+row.dataset.index]?.dir) {
        this.path.push(this.entries[+row.dataset.index].name);
        this.selected.clear(); this.refresh();
      }
    });
    this.$('local-list').addEventListener('click', e => this.clickLocal(e));
    this.$('local-list').addEventListener('dragstart', e => {
      const row = e.target.closest('[data-index]'); if (!row) return;
      const i = +row.dataset.index;
      if (!this.localSelected.has(i)) this.localSelected = new Set([i]);
      e.dataTransfer.setData('application/x-fs16-local', '1');
      e.dataTransfer.effectAllowed = 'copy'; this.updateSelectionClasses('local-list', this.localSelected);
    });
    this.$('disk-list').addEventListener('dragstart', e => {
      const row = e.target.closest('[data-index]'); if (!row) return;
      const i = +row.dataset.index;
      if (!this.selected.has(i)) this.selected = new Set([i]);
      e.dataTransfer.setData('application/x-fs16-disk', '1');
      e.dataTransfer.effectAllowed = 'copy'; this.updateSelectionClasses('disk-list', this.selected);
    });
    const bindDrop = (id, fn) => {
      const el = this.$(id);
      el.addEventListener('dragover', e => { e.preventDefault(); e.dataTransfer.dropEffect = 'copy'; el.classList.add('dropover'); });
      el.addEventListener('dragleave', e => { if (!el.contains(e.relatedTarget)) el.classList.remove('dropover'); });
      el.addEventListener('drop', e => { e.preventDefault(); e.stopPropagation(); el.classList.remove('dropover'); fn(e); });
    };
    bindDrop('local-list', e => {
      if (e.dataTransfer.types.includes('application/x-fs16-disk')) this.download();
      else if (e.dataTransfer.files.length) this.stage(e.dataTransfer.files);
    });
    bindDrop('disk-list', e => {
      // Dropping on a folder copies directly into that folder; on empty space, into the current path.
      const row = e.target.closest('[data-index]');
      const entry = row && this.entries[+row.dataset.index];
      const destination = entry?.dir ? [...this.path, entry.name] : [...this.path];
      if (e.dataTransfer.types.includes('application/x-fs16-local')) this.upload(null, destination);
      else if (e.dataTransfer.files.length) this.uploadFiles(Array.from(e.dataTransfer.files), destination);
    });
    d.addEventListener('dragover', e => e.preventDefault());
    d.addEventListener('drop', e => {
      if (e.defaultPrevented) return;
      e.preventDefault();
      if (e.dataTransfer.files.length) this.stage(e.dataTransfer.files);
    });
  }
  say(text) { this.$('message').textContent = text; }
  image() { return engine.getDiskImage(this.n); }
  volume() {
    const disk = engine.getDisks()[this.n], image = this.image();
    if (!disk || !image) throw new Error('Disk has been detached');
    return new Fs16(image, disk.readOnly, MACHINE.charset);
  }
  refresh() {
    if (this.win.closed) return;
    try {
      const disk = engine.getDisks()[this.n];
      this.fs = this.volume();
      try { this.entries = this.fs.list(this.path); }
      catch (err) { this.path = []; this.selected.clear(); this.entries = this.fs.list(); }
      this.$('summary').textContent = `${disk.name} · ${this.fs.label || 'unnamed'} · ${this.fs.freeClusters} free clusters`;
      this.$('warning').textContent = this.fs.dirty ? 'DIRTY volume: writing is disabled until FS16 is checked and unmounted cleanly.' :
        disk.readOnly ? 'Read-only disk: browsing and exporting are available.' :
        'Do not modify a disk mounted by the guest OS for writing; pause execution first.';
      this.$('format').hidden = true;
    } catch (err) {
      this.fs = null; this.entries = []; this.selected.clear();
      const disk = engine.getDisks()[this.n];
      this.$('summary').textContent = disk ? disk.name : 'not attached';
      this.$('warning').textContent = err.message;
      this.$('format').hidden = !disk || disk.readOnly;
    }
    const disabled = !this.fs || this.fs.readOnly || this.fs.dirty || this.pending || ui.running || engine.diskBusy;
    this.$('mkdir').disabled = disabled; this.$('remove').disabled = disabled;
    this.$('upload').disabled = disabled; this.$('up').disabled = !this.path.length;
    this.$('download').disabled = !this.fs;
    this.$('path').textContent = '/' + this.path.join('/');
    this.renderDisk(); this.renderLocal();
  }
  renderRows(target, items, selection, getName, getSize, isDir) {
    const el = this.$(target); el.replaceChildren();
    items.forEach((entry, index) => {
      const row = this.win.document.createElement('div');
      row.className = 'item' + (selection.has(index) ? ' selected' : '');
      row.dataset.index = index; row.draggable = true;
      const icon = this.win.document.createElement('span'); icon.textContent = isDir(entry) ? '📁' : '📄';
      const name = this.win.document.createElement('b'); name.textContent = getName(entry); name.title = name.textContent;
      const size = this.win.document.createElement('small'); size.textContent = isDir(entry) ? '<DIR>' : `${getSize(entry).toLocaleString()} B`;
      row.append(icon, name, size); el.appendChild(row);
    });
    if (!items.length) { const p = this.win.document.createElement('p'); p.className = 'hint'; p.textContent = target === 'disk-list' ? 'Directory is empty' : 'Drag files here or choose Add files'; el.appendChild(p); }
  }
  renderDisk() {
    this.renderRows('disk-list', this.entries, this.selected, e=>e.name, e=>e.size, e=>e.dir);
    this.$('counter').textContent = `${this.selected.size} selected · ${this.entries.length} entries`;
  }
  renderLocal() {
    this.renderRows('local-list', this.local, this.localSelected, f=>f.webkitRelativePath || f.name, f=>f.size, ()=>false);
  }
  toggle(e, items, selected, lastIndexProperty) {
    const row = e.target.closest('[data-index]'); if (!row) return;
    const i = +row.dataset.index;
    if (e.shiftKey && this[lastIndexProperty] >= 0) {
      const a = Math.min(i, this[lastIndexProperty]), b = Math.max(i, this[lastIndexProperty]);
      for (let x = a; x <= b; x++) selected.add(x);
    } else if (e.ctrlKey || e.metaKey) {
      if (selected.has(i)) selected.delete(i); else selected.add(i);
      this[lastIndexProperty] = i;
    } else { selected.clear(); selected.add(i); this[lastIndexProperty] = i; }
  }
  updateSelectionClasses(id, selected) {
    for (const row of this.$(id).querySelectorAll('[data-index]'))
      row.classList.toggle('selected', selected.has(+row.dataset.index));
    this.$('counter').textContent = `${this.selected.size} selected · ${this.entries.length} entries`;
  }
  clickDisk(e) { this.toggle(e, this.entries, this.selected, 'last'); this.updateSelectionClasses('disk-list', this.selected); }
  clickLocal(e) { this.toggle(e, this.local, this.localSelected, 'lastLocal'); this.updateSelectionClasses('local-list', this.localSelected); }
  stage(files) {
    const additions = Array.from(files);
    this.local.push(...additions);
    this.localSelected = new Set(additions.map((_, i) => this.local.length - additions.length + i));
    this.renderLocal(); this.say(`${additions.length} file(s) staged. Drag the selection to the disk pane to copy.`);
  }
  canWrite() {
    if (this.pending) throw new Error('A file operation is already in progress');
    if (ui.running || engine.diskBusy) throw new Error('Pause the CPU and finish the disk command before modifying FS16');
    this.fs = this.volume();
    if (this.fs.readOnly) throw new Error('This disk is read-only');
    if (this.fs.dirty) throw new Error('FS16 volume is DIRTY; unmount/check the guest filesystem first');
  }
  changed() { const disk = engine.disk.disks[this.n]; if (disk) disk.modified = true; renderDisks(); }
  async upload(files = null, destination = this.path) {
    const chosen = files || (this.localSelected.size ? Array.from(this.localSelected).sort((a,b)=>a-b).map(i=>this.local[i]) : this.local);
    if (!chosen.length) { this.say('Select files on the computer side first.'); return; }
    try { this.canWrite(); } catch (e) { this.say(e.message); this.refresh(); return; }
    this.pending = true; this.refresh();
    let ok = 0, skipped = 0; const errors = [];
    for (const f of chosen) {
      try {
        const parts = (f.webkitRelativePath || f.name).split('/').filter(Boolean);
        const name = parts.pop(), path = [...destination];
        // Folder uploads preserve their relative directory structure.
        for (const dirName of parts) {
          let item = this.fs.list(path).find(e => Fs16.sameName(e.name, dirName));
          if (!item) { this.fs.mkdir(path, dirName); this.changed(); item = this.fs.list(path).find(e => Fs16.sameName(e.name, dirName)); }
          if (!item.dir) throw new Error(`File conflicts with folder: ${dirName}`);
          path.push(item.name);
        }
        this.fs.writeFile(path, name, new Uint8Array(await f.arrayBuffer()));
        this.changed(); ok++;
      } catch (e) {
        errors.push(`${f.name}: ${e.message}`); skipped++;
        if (this.fs?.dirty) break; // do not continue after an incomplete metadata update
      }
    }
    this.pending = false; this.refresh();
    this.say(`Copied ${ok} file(s); ${skipped} failed.${errors.length ? '\n' + errors.slice(0, 6).join('\n') : ''}`);
  }
  uploadFiles(files, destination = this.path) { return this.upload(files, destination); }
  collect(entry, prefix, out, visited = new Set()) {
    if (out.length >= 65000) throw new Error('Too many entries for a ZIP archive');
    if (entry.dir) {
      if (visited.has(entry.offset)) throw new Error('Directory cycle detected');
      visited.add(entry.offset);
      const children = this.fs._entries(entry);
      if (!children.length) out.push({name: prefix + entry.name + '/', data: new Uint8Array()});
      for (const ch of children) this.collect(ch, prefix + entry.name + '/', out, visited);
      visited.delete(entry.offset);
    } else out.push({ name: prefix + entry.name, data: this.fs.readFile(entry) });
  }
  async download() {
    try {
      this.fs = this.volume();
      const chosen = Array.from(this.selected).sort((a,b)=>a-b).map(i=>this.entries[i]).filter(Boolean);
      if (!chosen.length) { this.say('Select one or more entries on the disk side.'); return; }
      if (chosen.length === 1 && !chosen[0].dir && !inViewer()) {
        await offerDownload(chosen[0].name, this.fs.readFile(chosen[0]), 'application/octet-stream');
        this.say(`Downloaded ${chosen[0].name}.`);
      } else {
        const files = [];
        for (const entry of chosen) this.collect(entry, '', files);
        const name = chosen.length === 1 ? chosen[0].name + '.zip' : 'fs16-files.zip';
        await offerDownload(name, makeZip(files));
        this.say(`Downloaded ${chosen.length} selected entry/entries (${files.length} files/folders in ZIP).`);
      }
    } catch (e) { this.say(`Download failed: ${e.message}`); }
  }
  makeDirectory() {
    const name = this.win.prompt('New FS16 folder name (1–32 characters):', 'NEWDIR');
    if (name === null) return;
    try { this.canWrite(); this.fs.mkdir(this.path, name); this.changed(); this.refresh(); this.say(`Created directory ${name}.`); }
    catch (e) { this.say(e.message); this.refresh(); }
  }
  remove() {
    const chosen = Array.from(this.selected).sort((a,b)=>b-a).map(i=>this.entries[i]).filter(Boolean);
    if (!chosen.length) { this.say('Select entries to delete.'); return; }
    if (!this.win.confirm(`Delete ${chosen.length} selected FS16 entries? Folders must be empty.`)) return;
    try { this.canWrite(); } catch (e) { this.say(e.message); return; }
    let ok = 0; const errors = [];
    for (const entry of chosen) {
      try { this.fs.remove(this.path, entry.name); this.changed(); ok++; }
      catch (e) { errors.push(`${entry.name}: ${e.message}`); if (this.fs.dirty) break; }
    }
    this.selected.clear(); this.refresh(); this.say(`Deleted ${ok} entries.${errors.length ? '\n' + errors.join('\n') : ''}`);
  }
  format() {
    if (!this.win.confirm('Format this image as FS16? ALL existing data on the disk will be erased.')) return;
    try {
      if (ui.running || engine.diskBusy) throw new Error('Pause execution before formatting');
      const disk = engine.getDisks()[this.n];
      if (!disk || disk.readOnly) throw new Error('Disk missing or read-only');
      Fs16.format(this.image(), disk.name.replace(/\.[^.]*$/, '').slice(0, 16), MACHINE.charset);
      this.path = []; this.selected.clear(); this.changed(); this.refresh();
      this.say('FS16 filesystem formatted. Download the disk image to save changes.');
    } catch (e) { this.say(`Format failed: ${e.message}`); }
  }
}
