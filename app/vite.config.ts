import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';
import { VitePWA } from 'vite-plugin-pwa';

export default defineConfig({
  base: '/app/',
  plugins: [
    react(),
    VitePWA({
      // 'autoUpdate': a fresh build is fetched and applied automatically on the
      // next launch/poll — no tap-to-update toast. Switched from 'prompt' because
      // iOS PWAs were silently lagging when the toast was missed/flaky, leaving
      // phones stuck on an old bundle (e.g. missing the Bonds section).
      // NB: this is the APP service worker only — firmware OTA stays prompt-based.
      registerType: 'autoUpdate',
      includeAssets: ['icon-192.png', 'icon-512.png', 'apple-touch-icon.png'],
      manifest: {
        name: 'Hive Companion',
        short_name: 'Hive',
        description: 'A window onto your colony',
        theme_color: '#E9DCC1',
        background_color: '#E9DCC1',
        display: 'standalone',
        start_url: '/app/',
        scope: '/app/',
        icons: [
          { src: 'icon-192.png', sizes: '192x192', type: 'image/png' },
          { src: 'icon-512.png', sizes: '512x512', type: 'image/png', purpose: 'any maskable' },
        ],
      },
      workbox: {
        // wasm MUST be here. hive.wasm has no content hash in its filename and was
        // not precached, so it came from the browser's HTTP cache and could lag
        // indefinitely while the React bundle updated around it — an install can
        // run a MONTHS-old sim under a current UI. Caught 2026-08-02: a keeper's
        // princess sat frozen in `seeking_company` (a bug fixed in v221) because
        // her install was still on the v218 wasm while others were on v223.
        // Precaching it makes the service worker revision it in lockstep with
        // hive.js — which matters doubly, since glue and wasm must agree.
        globPatterns: ['**/*.{js,css,html,png,svg,woff2,wasm}'],
        // Pull our push / notificationclick handlers into the generated SW
        importScripts: ['push-handler.js'],
        runtimeCaching: [
          {
            urlPattern: /\/api\/v1\//,
            handler: 'NetworkFirst',
            options: {
              cacheName: 'api-cache',
              expiration: { maxEntries: 50, maxAgeSeconds: 3600 },
              networkTimeoutSeconds: 5,
            },
          },
        ],
      },
    }),
  ],
  server: {
    proxy: {
      '/api': {
        target: 'https://hive.campbell.fish',
        changeOrigin: true,
        secure: true,
      },
    },
  },
});
