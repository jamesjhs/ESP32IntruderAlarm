/*
 * ESP32 Intruder Alarm service worker.
 *
 * Purpose:
 * - Provides the installable PWA runtime layer for the browser dashboard served
 *   by `src/server.ts`.
 * - Caches the app shell and versioned visual assets so the dashboard can still
 *   open when the Pi, Cloudflare Access session, or phone network is briefly
 *   unavailable.
 * - Handles Web Push notifications sent by the PWA server through VAPID. Push
 *   payloads are intentionally self-contained because Android may wake this
 *   worker when the Cloudflare Access browser session has expired.
 * - Routes notification clicks back to the dashboard, focusing an existing PWA
 *   window when possible.
 *
 * Interactions:
 * - `src/server.ts` rewrites APP_VERSION at request time when APP_VERSION is
 *   supplied through environment configuration.
 * - `public/app.js` registers this worker, asks it to skip waiting during
 *   updates, and listens for `PUSH_SUBSCRIPTION_CHANGED` messages.
 * - `manifest.webmanifest` and the icon files share the same version query
 *   string so browser caches move together when the app version changes.
 */
const APP_VERSION = "0.2.1";
const CACHE_NAME = `esp32-alarm-${APP_VERSION}`;
const ASSET_VERSION = `?v=${APP_VERSION}`;

// The minimal shell needed for offline startup and push notification artwork.
// Routes that carry live alarm data remain network-first below.
const STATIC_ASSETS = [
  "/",
  "/index.html",
  "/styles.css",
  "/app-config.js",
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

// Used when a push payload is missing fields or cannot be parsed as JSON.
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

// Pre-cache the shell and take control immediately after install. Individual
// cache failures are logged but do not abort installation; a missing icon should
// not prevent alarm notifications from working.
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

// Delete old versioned caches and claim open clients. This is the mechanism that
// makes a 0.2.1 -> future-version update shed the previous app shell promptly.
self.addEventListener("activate", (event) => {
  event.waitUntil(
    caches
      .keys()
      .then((keys) => Promise.all(keys.filter((key) => key !== CACHE_NAME).map((key) => caches.delete(key))))
      .then(() => self.clients.claim())
  );
});

// The visible app can request immediate activation after it detects a host
// version change and asks the user to refresh.
self.addEventListener("message", (event) => {
  if (event.data?.type === "SKIP_WAITING") {
    self.skipWaiting();
  }
});

// Network strategy:
// - API calls are always network-first, because stale alarm/node data is worse
//   than an explicit offline error.
// - Navigations are network-first with cached shell fallback.
// - Static assets are cache-first after initial install/fetch.
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

// Show incoming push messages without fetching extra data. This matters for
// locked phones and expired Cloudflare sessions: the notification must be useful
// even when the dashboard cannot currently make authenticated API calls.
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

// Return the user to the dashboard when they tap a notification. Prefer focusing
// an existing installed-PWA window so Android does not open duplicate dashboards.
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

// Browser push subscriptions can be rotated by the user agent. Tell any open
// dashboard windows so app.js can re-check subscription state.
self.addEventListener("pushsubscriptionchange", (event) => {
  event.waitUntil(
    self.clients.matchAll({ type: "window", includeUncontrolled: true }).then((clients) => {
      for (const client of clients) {
        client.postMessage({ type: "PUSH_SUBSCRIPTION_CHANGED" });
      }
    })
  );
});
