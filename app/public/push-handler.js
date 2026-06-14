// Web Push handlers — imported into the generated Workbox service worker via
// vite-plugin-pwa's workbox.importScripts. Plain ES5-ish so it needs no build.

self.addEventListener('push', function (event) {
  var data = {};
  try {
    data = event.data ? event.data.json() : {};
  } catch (e) {
    data = { body: (event.data && event.data.text()) || '' };
  }
  var title = data.title || 'Hive';
  var options = {
    body: data.body || '',
    icon: '/app/icon-192.png',
    badge: '/app/icon-192.png',
    tag: data.tag || 'hive',
    renotify: true,
    data: { url: data.url || '/app/' },
  };
  event.waitUntil(self.registration.showNotification(title, options));
});

self.addEventListener('notificationclick', function (event) {
  event.notification.close();
  var url = (event.notification.data && event.notification.data.url) || '/app/';
  event.waitUntil(
    self.clients
      .matchAll({ type: 'window', includeUncontrolled: true })
      .then(function (clientList) {
        for (var i = 0; i < clientList.length; i++) {
          var c = clientList[i];
          if (c.url.indexOf('/app') !== -1 && 'focus' in c) return c.focus();
        }
        if (self.clients.openWindow) return self.clients.openWindow(url);
      })
  );
});
