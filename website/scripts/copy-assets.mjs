// Prepare shared static assets for both the live server and production builds.
import { cp, mkdir, writeFile, readFile, rm } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
const root = new URL('../../', import.meta.url);
const dist = new URL('../.generated-public/', import.meta.url);
await mkdir(new URL('play/', dist), { recursive: true });
for (const name of ['choochootracker.js', 'choochootracker.wasm', 'choochootracker.data', 'choochootracker.html', 'privacy.html']) {
  await cp(new URL(`web/dist/${name}`, root), new URL(`play/${name}`, dist));
}
// The site owns /play/; keep the tracker in its own document so its theme and
// keyboard handlers cannot change the site's navigation.
const app = await readFile(new URL('web/dist/index.html', root), 'utf8');
await writeFile(new URL('play/runtime.html', dist), app.replace('<a href="manual.html">', '<a href="../manual/" target="_top">').replace('</style>',
  'body { min-height:0; display:block; } main { margin-inline:auto; }\n</style>'));
await rm(new URL('play/index.html', dist), { force: true });
// Keep old bookmarks and the app's Manual link, including section anchors.
for (const [path, target] of [['manual.html', './manual/'], ['play/manual.html', '../manual/']]) {
  await writeFile(new URL(path, dist), `<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width"><title>User manual</title><meta http-equiv="refresh" content="0;url=${target}"></head><body><a href="${target}">Open the user manual</a><script>location.replace(${JSON.stringify(target)} + location.search + location.hash);</script></body></html>`);
}
await mkdir(new URL('user-guide/', dist), { recursive: true });
await cp(new URL('docs/user-guide/index.html', root), new URL('user-guide/index.html', dist));
await cp(new URL('docs/user-guide/assets/', root), new URL('user-guide/assets/', dist), { recursive: true });
// Include every field guide capture so copied sections can reuse their images.
await cp(new URL('docs/user-guide/assets/', root), new URL('screenshots/', dist), { recursive: true });
await cp(new URL('../src/assets/homepage-screenshots/', import.meta.url), new URL('homepage-screenshots/', dist), { recursive: true });
await cp(new URL('../node_modules/@fontsource/fira-sans/LICENSE', import.meta.url), new URL('FiraSans-LICENSE.txt', dist));
await writeFile(new URL('.nojekyll', dist), '');
console.log(`App, guides and screenshots copied to ${fileURLToPath(dist)}`);
