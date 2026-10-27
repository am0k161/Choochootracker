# ChooChooTracker website

English presentation site, built with Astro. Fira Sans is bundled locally; no
font service, analytics or audio/video player is loaded by the presentation pages.
The tracker only loads when visiting the app. This is a **local preview** until
the design is approved; no publishing workflow is enabled by this change.

## Preview locally

Requires Node.js 20.19+ and npm. From this directory:

```sh
npm ci
npm run dev
```

Open <http://127.0.0.1:4321/Choochootracker/>. This is the single editing server:
Markdown, templates and CSS update live, including screenshots, the app and guides.
Edit only `src/`; never edit `dist/` or `.generated-public/`, which are generated.
Static assets are copied from the repository when the server starts. Restart it
if the tracker bundle or guide images change. The tracker is not recompiled.
The fixed port fails when already occupied instead of opening another version.
With the server running, `node scripts/check-live.mjs` checks the live pages,
app, guides and static assets.

For a production check, stop the editing server first, then run:

```sh
npm run build
npm run check
npm run preview
```

Preview uses the same URL and displays the last build. Restart `npm run dev`
to resume editing; Markdown changes do not update a production preview.

## Edit the content

- [Home introduction](src/content/home.md): headline, opening text and hero image.
- [Engine descriptions](src/content/featured-engines.md): all engine cards, their
  descriptions and optional screenshots. This is the homepage engine content.
- [Sampling](src/content/03-sampling.md) and [effects](src/content/04-effects.md):
  section headings and text. Workflow steps are in the home introduction.
- `01-workflow.md` and `02-engines.md` are older drafts, not rendered on the site.
- [Project story](src/content/about.md), [downloads](src/content/downloads.md),
  [learning introduction](src/content/learn.md).

The block between `---` lines contains the title and image metadata; the text
below it is Markdown. Screenshot names refer to the
[field guide assets](../docs/user-guide/assets). Use descriptive imageAlt text
and keep screenshots at their original 640 × 480 resolution. All field guide assets are copied automatically by
[the asset copy script](scripts/copy-assets.mjs).

The `unifont` dependency is held at 0.7.4 because 0.7.5 pulls in Undici 8,
which requires Node 22.19. The website does not use Astro's experimental
remote font provider; Fira Sans is bundled from Fontsource.

[Page layout](src/layouts/Page.astro) contains navigation and footer labels.
[Styles](src/styles/site.css) contain the anthracite/orange palette and
responsive layout. The [homepage template](src/pages/index.astro) contains
button labels, the feature strip and guide links.

## GitHub Pages, when approved

The default configuration targets
`https://paiheulevrai.github.io/Choochootracker/`. The `GITHUB_REPOSITORY`
environment variable selects another owner/repository at build time. All site
links include the repository subpath.

Publish the contents of `dist/` as a GitHub Pages artifact after running
`npm ci`, `npm run build` and `npm run check`. Select GitHub Actions as the
Pages source when a deployment workflow is added. Rebuild the site after
updating the checked-in `web/dist/` app bundle or guide assets.

Downloads currently link to the releases list, which includes prereleases.
GitHub's `/releases/latest` shortcut excludes prereleases, so it is unsuitable
as the sole alpha download link. No version number is hard-coded here.

Browser projects do not automatically migrate from the Vercel origin; the
downloads page explains export/import. Keep the old app available during any
future migration. Nothing in this site build changes the Vercel deployment.

## Manual theme

The [manual page](src/pages/manual.astro) renders the canonical
[USER_MANUAL.md](../docs/USER_MANUAL.md) with the same [Page layout](src/layouts/Page.astro),
fonts and [site theme](src/styles/site.css) as the presentation pages.
[Manual CSS](src/styles/manual.css) only controls document and contents layout;
use the shared theme variables for colours. Do not copy the standalone app manual
into the website. The asset script creates compatibility redirects for
`manual.html` and `play/manual.html`, preserving section anchors.

The [web app page](src/pages/play.astro) also uses the shared page layout. It
embeds the existing tracker in a same-origin frame, preserving browser storage
and keeping tracker themes and keyboard handlers separate from site navigation.
The asset script generates `play/runtime.html`; edit the original web wrapper
in `web/index.html` for tracker controls, not that generated file.
