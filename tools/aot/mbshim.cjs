/* mbshim.cjs — foreign-DLL symbol-alias shimmer for MochiBake
 *
 * Given a DLL we do NOT own its source for and the set of symbols a
 * program actually calls, emit a tiny *forwarding* DLL that exports only
 * those symbols and forwards each to the original dynamic library. The OS
 * loader follows the forwarder at load time, so the implementation code is
 * BORROWED from the dynamic library ("meminjam aset dynamic libraries"),
 * never embedded. Only the used symbols appear in the shim, so a program
 * that touches 3 of 24 exports pays for 3, not 24.
 *
 *   node tools/aot/mbshim.cjs <orig.dll> [symbol ...]
 *     # symbols from argv...  otherwise read from stdin / --all
 *   node tools/aot/mbshim.cjs <orig.dll> --list          # print exports
 *
 * Output: <dir>/alias_<stem>.dll  (and alias_<stem>.def)
 * The original is copied next to it as <orig-name> (its base name is the
 * forwarder target), so the pair is self-contained.
 */
'use strict';
const fs = require('fs');
const os = require('os');
const path = require('path');
const { execFileSync } = require('child_process');

const GCC = process.env.MBGCC ||
  'C:/Users/Administrator/AppData/Local/Microsoft/WinGet/Packages/' +
  'BrechtSanders.WinLibs.MCF.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe/mingw64/bin/gcc.exe';

/* ---- minimal PE export-table reader (mirrors build-lean.js peInfo) ---- */
function peExports(bytes) {
  const u16 = (o) => bytes[o] | (bytes[o + 1] << 8);
  const u32 = (o) => ((bytes[o] | (bytes[o + 1] << 8) | (bytes[o + 2] << 16) |
                      (bytes[o + 3] << 24)) >>> 0);
  const cstr = (o) => { let e = o, s = '';
    while (e < bytes.length && bytes[e] !== 0) { s += String.fromCharCode(bytes[e]); e++; }
    return s; };
  if (bytes.length < 0x40) return [];
  const peoff = u32(0x3c);
  if (peoff + 24 + 20 + 0x38 > bytes.length) return [];
  const nsec = u16(peoff + 6), optsize = u16(peoff + 20), opt = peoff + 24;
  const magic = u16(opt);
  const dd = opt + (magic === 0x20b ? 0x70 : 0x60);
  const secs = [], base = opt + optsize;
  for (let i = 0; i < nsec; i++) {
    const s = base + i * 40;
    secs.push({ va: u32(s + 12), vsz: u32(s + 8), rsz: u32(s + 16), raw: u32(s + 20) });
  }
  const rva2off = (rva) => {
    for (const s of secs) { const span = Math.max(s.vsz, s.rsz);
      if (rva >= s.va && rva < s.va + span) return rva - s.va + s.raw; }
    return -1;
  };
  const out = [];
  const expRva = dd + 4 <= bytes.length ? u32(dd) : 0;
  if (expRva) {
    const e = rva2off(expRva);
    if (e >= 0 && e + 40 <= bytes.length) {
      const nnames = u32(e + 24), anames = u32(e + 32), no = rva2off(anames);
      for (let i = 0; i < nnames && no >= 0; i++) {
        const n = cstr(rva2off(u32(no + i * 4)));
        if (n && n.indexOf('.') < 0) out.push(n);   /* skip existing forwarders */
      }
    }
  }
  return out;
}

function stemOf(p) {
  const b = path.basename(String(p));
  return b.replace(/\.[^.]*$/, '');
}
function dotless(p) { return path.basename(String(p)).replace(/\.[^.]*$/, ''); }

function usage() {
  console.error('usage: node mbshim.cjs <orig.dll> [--list] [--all] [--out <dir>] [sym ...]');
  process.exit(2);
}

function main(argv) {
  let orig = null, outdir = null;
  const want = [];
  let list = false, all = false;
  for (let i = 2; i < argv.length; i++) {
    const a = argv[i];
    if (a === '--list') list = true;
    else if (a === '--all') all = true;
    else if (a === '--out') { outdir = argv[++i]; }
    else if (a.startsWith('--')) usage();
    else if (!orig) orig = a;
    else want.push(a);
  }
  if (!orig) usage();
  orig = path.resolve(orig);
  if (!fs.existsSync(orig)) { console.error('mbshim: not found: ' + orig); process.exit(1); }

  const bytes = fs.readFileSync(orig);
  const exps = peExports(bytes);
  const uniq = [...new Set(exps)];

  if (list) { console.log(uniq.join('\n')); return; }

  /* target the shim forwards to: the original, next to the shim */
  const stem = stemOf(orig);
  const target = dotless(orig);            /* "orig_dynalib" (no .dll) */
  const dir = outdir ? path.resolve(outdir) : path.dirname(orig);
  fs.mkdirSync(dir, { recursive: true });

  /* choose symbols: explicit list, --all, or stdin (newline/space) */
  let used;
  if (want.length) used = want;
  else if (all) used = uniq;
  else {
    let t = fs.readFileSync(0, 'utf8').trim();
    used = t ? t.split(/[\s,]+/).filter(Boolean) : [];
  }
  if (!used.length) { console.error('mbshim: no symbols requested'); process.exit(2); }

  const valid = [], missing = [];
  const seen = new Set();
  for (const s of used) {
    if (uniq.includes(s) && !seen.has(s)) { valid.push(s); seen.add(s); }
    else if (!seen.has(s)) { missing.push(s); }
  }
  if (!valid.length) { console.error('mbshim: no valid symbol in ' + used.join(',')); process.exit(1); }
  if (missing.length) console.error('mbshim: warning: not exported by ' +
                                   path.basename(orig) + ': ' + missing.join(', '));

  /* DEF forwarding file — the whole shim is these lines */
  const defLines = ['LIBRARY alias_' + stem + '.dll', 'EXPORTS'];
  for (const s of valid) defLines.push('  ' + s + ' = ' + target + '.' + s);
  const defPath = path.join(dir, 'alias_' + stem + '.def');
  fs.writeFileSync(defPath, defLines.join('\n') + '\n');

  /* empty C unit: we emit no code, only export forwarders */
  const csPath = path.join(dir, 'alias_' + stem + '.c');
  fs.writeFileSync(csPath, '/* generated by mbshim: export-forwarding shim, no code */\n');

  const shimPath = path.join(dir, 'alias_' + stem + '.dll');
  execFileSync(GCC, ['-shared', '-Os', '-s', csPath, '-o', shimPath, defPath],
               { stdio: ['ignore', 'ignore', 'inherit'],
                 env: Object.assign({}, process.env, { MSYS_NO_PATHCONV: '1' }) });

  const origSize = bytes.length;
  const shimSize = fs.statSync(shimPath).size;
  const saved = origSize ? (100 * (origSize - shimSize) / origSize) : 0;
  console.log('mbshim: ' + path.basename(orig) + ' -> ' + path.basename(shimPath));
  console.log('  forwarded symbols: ' + valid.join(', '));
  if (missing.length) console.log('  missing (skipped): ' + missing.join(', '));
  console.log('  size: ' + origSize + ' B -> ' + shimSize + ' B  (-' +
              saved.toFixed(1) + '% ; code borrowed from ' + path.basename(orig) + ')');
  console.log('  place ' + path.basename(orig) + ' next to the shim at runtime (borrowed DLL)');
}

main(process.argv);