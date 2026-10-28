import { readFile, writeFile } from 'node:fs/promises';

const page = new URL('../dist/index.html', import.meta.url);
let html = await readFile(page, 'utf8');

if (!html.includes('id="vercel-migration-notice"')) {
  html = html.replace(/<title>.*?<\/title>/, '<title>ChooChooTracker has moved</title>');
  html = html.replace('</style>', `
    .migration-notice { max-width: 640px; margin: 1rem auto; padding: 1rem; border: 1px solid var(--value); background: var(--empty); text-align: left; line-height: 1.5; }
    .migration-notice h2 { color: var(--value); font-size: 1.15rem; letter-spacing: normal; }
    .migration-notice a { color: var(--value); font-weight: 700; }
  </style>`);
  html = html.replace('</h1>', `</h1>
    <aside class="migration-notice" id="vercel-migration-notice" aria-labelledby="migration-title">
      <h2 id="migration-title">ChooChooTracker has moved</h2>
      <p>Please move to the new web app as soon as possible: <a href="https://choochootracker.pages.dev/play/">choochootracker.pages.dev/play/</a>.</p>
      <p>This Vercel copy will remain available temporarily so you can migrate. Browser projects do not transfer automatically. Download each project as a <code>.cct</code> file here, and keep copies of imported samples and sound banks. Then import them into the new app. Do not clear this site's browser data until you have finished.</p>
      <p><a href="https://choochootracker.pages.dev/downloads/#coming-from-the-vercel-web-app">Read the migration instructions</a>.</p>
    </aside>`);
  html = html.replace('</head>', `
  <meta name="description" content="ChooChooTracker has moved to Cloudflare Pages. Use the new web app and migrate projects from this temporary Vercel copy.">
  <meta name="robots" content="index,follow">
  <link rel="canonical" href="https://choochootracker.pages.dev/play/">
  <meta property="og:type" content="website">
  <meta property="og:title" content="ChooChooTracker has moved">
  <meta property="og:description" content="Move to the new ChooChooTracker web app and migrate projects from this temporary Vercel copy.">
  <meta property="og:url" content="https://choochootracker.pages.dev/play/">
</head>`);
  await writeFile(page, html);
}
