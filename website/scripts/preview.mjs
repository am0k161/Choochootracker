// Astro 5 does not forward vite.preview options; use its Vite preview directly.
import { preview } from 'vite';
import config from '../astro.config.mjs';
const server = await preview({
  configFile: false,
  base: config.base,
  appType: 'mpa',
  build: { outDir: 'dist' },
  preview: { ...config.server, strictPort: true },
});
server.printUrls();
