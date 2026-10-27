// Run against npm run dev to catch missing static assets and directory indexes.
import assert from 'node:assert/strict';
import config from '../astro.config.mjs';
const base = `http://${config.server.host}:${config.server.port}${config.base.replace(/\/$/, '')}/`;
for (const path of ['', 'learn/', 'downloads/', 'play/', 'play/?check=1', 'user-guide/', 'manual.html', 'manual/', 'screenshots/engine-braids.png', 'play/choochootracker.wasm']) {
  const response = await fetch(new URL(path, base), { method: 'HEAD' });
  assert.equal(response.status, 200, `${path}: HTTP ${response.status}`);
}
console.log('Live site checks passed: pages, directory indexes, screenshot and WebAssembly.');
