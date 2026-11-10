/* 极简 service worker：只做网络直通，不缓存任何东西。
 *
 * 存在的唯一目的：Android Chrome 只有在站点有 fetch 处理器的 SW 时，才会把
 * 「添加到主屏幕」升级成 WebAPK（独立窗口、无地址栏），也就是"像真应用"。
 *
 * 不缓存是有意为之：页面内容（尤其是内联进去的密钥和脚本）更新频繁，
 * 一旦加缓存就会出现"改了线上、手机还是旧页面"的老问题。
 */
self.addEventListener('install', function () {
  self.skipWaiting();
});

self.addEventListener('activate', function (e) {
  e.waitUntil(self.clients.claim());
});

self.addEventListener('fetch', function (e) {
  if (e.request.method !== 'GET') return;      // MQTT 走 WebSocket，不经过这里
  e.respondWith(fetch(e.request).catch(function () {
    return new Response('离线：板子或网络暂时连不上', {
      status: 503,
      headers: { 'Content-Type': 'text/plain; charset=utf-8' }
    });
  }));
});
