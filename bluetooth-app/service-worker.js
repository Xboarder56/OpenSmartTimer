// The Pages builder replaces this with a digest of the complete published shell.
const APP_VERSION = 'development-v17';
const CACHE_PREFIX = 'smart-timer-bluetooth:' + self.registration.scope + ':';
const CACHE_NAME = CACHE_PREFIX + APP_VERSION;
const APP_SHELL = [
  './', './index.html', './app.css', './app.js', './ble-transport.mjs', './ble-management.mjs',
  './icon.svg', './manifest.webmanifest', '../web/styles.css', '../web/i18n.js', '../web/appearance.js', '../web/updates.js',
];
const shellUrls = new Set(APP_SHELL.map(path => new URL(path, self.registration.scope).href));

self.addEventListener('install', event => {
  // Cache.addAll is atomic. Failed downloads cannot promote a partial shell.
  // Existing controller windows keep their version until closed.
  event.waitUntil(caches.open(CACHE_NAME).then(cache => cache.addAll(
    APP_SHELL.map(path => new Request(new URL(path, self.registration.scope), {cache: 'reload'})),
  )));
});

self.addEventListener('activate', event => {
  event.waitUntil(caches.keys().then(names => Promise.all(names
    .filter(name => name.startsWith(CACHE_PREFIX) && name !== CACHE_NAME)
    .map(name => caches.delete(name)))));
});

self.addEventListener('fetch', event => {
  if (event.request.method !== 'GET') return;
  const url = new URL(event.request.url);
  if (url.origin !== self.location.origin) return;
  const key = event.request.mode === 'navigate'
    ? new URL('./index.html', self.registration.scope).href : url.href;
  if (!shellUrls.has(key)) return;
  // Immutable version: never overwrite individual files from a newer release.
  event.respondWith(caches.open(CACHE_NAME).then(cache => cache.match(key))
    .then(response => response || new Response('App cache unavailable. Close all app windows and reconnect online.',
      {status: 503, headers: {'Content-Type': 'text/plain'}})));
});
