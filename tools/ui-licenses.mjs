// Writes runtime\licenses\buffrx-page-THIRD-PARTY.txt: every package that ships inside ui/dist, its
// licence expression, and the licence texts it carries. MIT and Apache-2.0 require their notice to
// travel with the built page, and the page is minified into the editor binary, so the texts must ship
// with the installer. Reads ui/package-lock.json (the same tree `npm ci` installs) and needs the
// production dependencies installed in ui/node_modules.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const ui = path.join(root, 'ui');
const out = path.join(root, 'runtime', 'licenses', 'buffrx-page-THIRD-PARTY.txt');

const lock = JSON.parse(fs.readFileSync(path.join(ui, 'package-lock.json'), 'utf8'));
const packages = lock.packages || {};
const seen = new Map();

for (const [key, info] of Object.entries(packages)) {
  if (!key.startsWith('node_modules/') || info.dev || info.link) continue;
  const dir = path.join(ui, key);
  const manifest = path.join(dir, 'package.json');
  if (!fs.existsSync(manifest)) continue; // an optional dependency for another platform
  const pkg = JSON.parse(fs.readFileSync(manifest, 'utf8'));
  const name = pkg.name || key.slice('node_modules/'.length);
  const version = pkg.version || info.version || '';
  const license = pkg.license || (Array.isArray(pkg.licenses) ? pkg.licenses.map((l) => l.type).join(', ') : '');
  if (!seen.has(`${name}@${version}`)) seen.set(`${name}@${version}`, { name, version, license, dir });
}

const entries = [...seen.values()].sort((a, b) => a.name.localeCompare(b.name));
const texts = new Map(); // content hash -> { text, users: [] }
const missing = [];
const lines = entries.map((entry) => {
  const files = fs.readdirSync(entry.dir).filter((file) => /^(LICENSE|LICENCE|COPYING|NOTICE|UNLICENSE)/i.test(file));
  if (files.length === 0) missing.push(`${entry.name} ${entry.version}`);
  for (const file of files) {
    const text = fs.readFileSync(path.join(entry.dir, file), 'utf8').trim();
    if (!text) continue;
    const key = text;
    if (!texts.has(key)) texts.set(key, { text, users: [] });
    texts.get(key).users.push(`${entry.name} ${entry.version}`);
  }
  return `${entry.name} ${entry.version} - ${entry.license || 'see text'}`;
});

const report = [
  'BuffrX editor page (ui/dist): third-party JavaScript packages',
  '=============================================================',
  '',
  ...lines,
  '',
  'Licence texts. Each text applies to the packages listed above it.',
];
for (const key of [...texts.keys()].sort()) {
  report.push('', '-----------------------------------------------------------------',
    `Used by: ${[...new Set(texts.get(key).users)].sort().join(', ')}`, '', texts.get(key).text);
}

fs.mkdirSync(path.dirname(out), { recursive: true });
fs.writeFileSync(out, report.join('\n'), 'utf8');
console.log(`wrote ${out} (${entries.length} packages, ${texts.size} distinct licence texts)`);
if (missing.length) console.log(`no licence file shipped in: ${missing.join(', ')}`);
