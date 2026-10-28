// Check the assembled site at the project subpath used by GitHub Pages.
import assert from 'node:assert/strict';
import { readFile, stat } from 'node:fs/promises';
const dist = new URL('../dist/', import.meta.url);
const repository = process.env.GITHUB_REPOSITORY || 'paiheulevrai/Choochootracker';
const name = repository.split('/')[1];
const base = process.env.CF_PAGES === '1' || name.endsWith('.github.io') ? '/' : `/${name}/`;
for (const path of ['index.html', 'learn/index.html', 'downloads/index.html', 'manual/index.html', 'play/index.html']) {
  const html = await readFile(new URL(path, dist), 'utf8');
  assert(html.includes('lang="en"'), `${path}: English document`);
  assert(!html.includes('choochootracker.js'), `${path}: presentation must not load the app`);
  for (const [, target] of html.matchAll(/(?:href|src)="([^"#?]+)[^\"]*"/g)) {
    if (/^(https?:|mailto:|data:)/.test(target)) continue;
    assert(target.startsWith(base), `${path}: link outside project base: ${target}`);
    const relative = target.slice(base.length);
    const file = relative.endsWith('/') || !relative ? `${relative}index.html` : relative;
    assert((await stat(new URL(file, dist))).isFile(), `${path}: missing ${file}`);
  }
}
for (const path of ['play/index.html', 'play/runtime.html', 'play/choochootracker.js', 'play/choochootracker.wasm', 'play/choochootracker.data', 'play/manual.html', 'manual.html', 'user-guide/index.html']) {
  assert((await stat(new URL(path, dist))).size > 0, `${path}: missing or empty`);
}
console.log('Site checks passed: project paths, linked assets, app and guides.');
