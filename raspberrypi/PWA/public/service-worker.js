const APP_VERSION = "0.0.1";
const CACHE_NAME = `esp32-alarm-${APP_VERSION}`;
const ASSET_VERSION = `?v=${APP_VERSION}`;

const STATIC_ASSETS = [
  "/",
  "/index.html",
  "/styles.css",
  "/app.js",
  `/manifest.webmanifest${ASSET_VERSION}`,
  `/favicon.png${ASSET_VERSION}`,
  `/apple-touch-icon.png${ASSET_VERSION}`,
  `/icons/icon-192x192.png${ASSET_VERSION}`,
  `/icons/icon-512x512.png${ASSET_VERSION}`,
  `/icons/maskable-icon-192x192.png${ASSET_VERSION}`,
  `/icons/maskable-icon-512x512.png${ASSET_VERSION}`,
  `/icons/notification-badge-96x96.png${ASSET_VERSION}`
];

const DEFAULT_NOTIFICATION = {
  type: "alarm_status",
  severity: "info",
  event_id: "status",
  title: "ESP32 Intruder Alarm",
  body: "Alarm status updated.",
  url: "/",
  icon: `/icons/icon-192x192.png${ASSET_VERSION}`,
  badge: `/icons/notification-badge-96x96.png${ASSET_VERSION}`
};

self.addEventListener("install", (event) => {
  event.waitUntil(
    caches
      .open(CACHE_NAME)
      .then((cache) =>
        Promise.allSettled(
          STATIC_ASSETS.map((url) => cache.add(url).catch((error) => console.warn("SW cache failed", url, error)))
        )
      )
      .then(() => self.skipWaiting())
  );
});

self.addEventListener("activate", (event) => {
  event.waitUntil(
    caches
      .keys()
      .then((keys) => Promise.all(keys.filter((key) => key !== CACHE_NAME).map((key) => caches.delete(key))))
      .then(() => self.clients.claim())
  );
});

self.addEventListener("message", (event) => {
  if (event.data?.type === "SKIP_WAITING") {
    self.skipWaiting();
  }
});

self.addEventListener("fetch", (event) => {
  const url = new URL(event.request.url);

  if (url.pathname.startsWith("/api/")) {
    event.respondWith(
      fetch(event.request).catch(
        () =>
          new Response(JSON.stringify({ error: "Offline or Cloudflare Access session unavailable." }), {
            status: 503,
            headers: { "Content-Type": "application/json" }
          })
      )
    );
    return;
  }

  if (event.request.mode === "navigate") {
    event.respondWith(
      fetch(event.request)
        .then((response) => {
          if (response && response.status === 200) {
            const clone = response.clone();
            caches.open(CACHE_NAME).then((cache) => cache.put("/", clone));
          }
          return response;
        })
        .catch(() => caches.match("/").then((cached) => cached || fetch("/")))
    );
    return;
  }

  event.respondWith(
    caches.match(event.request).then((cached) => {
      if (cached) {
        return cached;
      }

      return fetch(event.request)
        .then((response) => {
          if (response && response.status === 200 && event.request.method === "GET") {
            const clone = response.clone();
            caches.open(CACHE_NAME).then((cache) => cache.put(event.request, clone));
          }
          return response;
        })
        .catch(() => new Response("Offline", { status: 503 }));
    })
  );
});

self.addEventListener("push", (event) => {
  let data = { ...DEFAULT_NOTIFICATION };

  if (event.data) {
    try {
      data = { ...data, ...event.data.json() };
    } catch {
      data = { ...data, body: event.data.text() || data.body };
    }
  }

  const tag = data.event_id ? `esp32-alarm-${data.event_id}` : `esp32-alarm-${data.type}`;
  const requireInteraction = data.severity === "critical";

  event.waitUntil(
    self.registration.showNotification(data.title || DEFAULT_NOTIFICATION.title, {
      body: data.body || DEFAULT_NOTIFICATION.body,
      icon: data.icon || DEFAULT_NOTIFICATION.icon,
      badge: data.badge || DEFAULT_NOTIFICATION.badge,
      tag,
      renotify: true,
      requireInteraction,
      timestamp: data.timestamp_ms || Date.now(),
      data: {
        url: data.url || "/",
        event_id: data.event_id,
        type: data.type,
        severity: data.severity
      },
      actions: [
        { action: "open", title: "Open" },
        { action: "dismiss", title: "Dismiss" }
      ]
    })
  );
});

self.addEventListener("notificationclick", (event) => {
  event.notification.close();

  if (event.action === "dismiss") {
    return;
  }

  const targetUrl = event.notification.data?.url || "/";

  event.waitUntil(
    self.clients.matchAll({ type: "window", includeUncontrolled: true }).then((clientList) => {
      const parsedTarget = new URL(targetUrl, self.location.origin);

      for (const client of clientList) {
        const clientUrl = new URL(client.url);
        if (clientUrl.origin === parsedTarget.origin && "focus" in client) {
          client.focus();
          if ("navigate" in client) {
            return client.navigate(parsedTarget.href);
          }
          return;
        }
      }

      if (self.clients.openWindow) {
        return self.clients.openWindow(parsedTarget.href);
      }
    })
  );
});

self.addEventListener("pushsubscriptionchange", (event) => {
  event.waitUntil(
    self.clients.matchAll({ type: "window", includeUncontrolled: true }).then((clients) => {
      for (const client of clients) {
        client.postMessage({ type: "PUSH_SUBSCRIPTION_CHANGED" });
      }
    })
  );
});
