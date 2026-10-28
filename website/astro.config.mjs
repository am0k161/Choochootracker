import { defineConfig } from 'astro/config';

const repository = process.env.GITHUB_REPOSITORY || 'paiheulevrai/Choochootracker';
const [owner, name] = repository.split('/');
const cloudflarePages = process.env.CF_PAGES === '1';
export default defineConfig({
  site: cloudflarePages
    ? 'https://choochootracker.pages.dev'
    : `https://${owner}.github.io`,
  base: cloudflarePages || name.endsWith('.github.io') ? '/' : `/${name}`,
  trailingSlash: 'always',
  markdown: {
    remarkPlugins: [() => (tree, file) => {
      if (!file.path?.replaceAll('\\', '/').endsWith('/docs/USER_MANUAL.md')) return;
      // Companion Markdown documents live in the repository, not under /manual/.
      function resolveLinks(node) {
        if (node.type === 'link' && /^[^:#?]+\.md(?:#.*)?$/.test(node.url)) {
          node.url = new URL(node.url, `https://github.com/${repository}/blob/main/docs/`).href;
        }
        node.children?.forEach(resolveLinks);
      }
      resolveLinks(tree);
    }],
  },
  publicDir: './.generated-public',
  server: { host: '127.0.0.1', port: 4321 },
  vite: {
    server: { strictPort: true },
    plugins: [{
      name: 'static-guide-and-app-index',
      configureServer(server) {
        // Vite serves public files but does not resolve directory index pages.
        server.middlewares.use((req, res, next) => {
          req.url = req.url?.replace(/\/(user-guide)\/(?=\?|$)/, '/$1/index.html');
          next();
        });
      },
    }],
  },
});
