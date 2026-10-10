/*
 * 离线外壳（Service Worker）。
 *
 * 为什么需要：iPad 上的"添加到主屏幕"网页 app 如果打开时 PC 上的程序没在运行，
 * 请求会打到 Tailscale 的代理而拿不到任何页面 —— 结果是一片空白，而且因为页面
 * 从未加载过，谁也救不回来，只能从后台划掉重开。
 *
 * 这里把页面外壳（HTML/CSS/JS）缓存下来：
 *   * 后端没开 → 直接用缓存渲染页面，页面自己会显示"连不上"，并继续轮询；
 *   * 后端起来 → 下一次轮询自动成功，数据自己刷新，**不需要重开网页 app**。
 *
 * 缓存策略：
 *   * /api/*            永远走网络（不得缓存，否则会显示旧数据）；
 *   * 带版本号的静态资源 缓存优先（URL 里的内容哈希保证不会串版本）；
 *   * 页面导航          网络优先，失败回落缓存。
 */

'use strict';

const CACHE_NAME = 'njulms-shell';
const ASSET_PATTERNS = [/^\/app\.js$/, /^\/style\.css$/, /^\/apple-touch-icon\.png$/];

const isAsset = (url) => ASSET_PATTERNS.some((pattern) => pattern.test(url.pathname));

self.addEventListener('install', (event) => {
  event.waitUntil((async () => {
    try {
      const cache = await caches.open(CACHE_NAME);
      // 预缓存入口页**以及它引用的资源**。
      //
      // 为什么必须在这里做：页面"第一次"打开时还不受 Service Worker 控制，
      // 那些 app.js / style.css 请求不会被拦截，也就不会进缓存 ——
      // 结果离线重载时只有 HTML 能出来，JS 加载失败，页面就停在初始状态
      // （实测踩过：缓存里只有 '/'）。这里直接从 HTML 里解析资源 URL 补上。
      const response = await fetch(new Request('/', { cache: 'reload' }));
      if (response.ok) {
        const html = await response.clone().text();
        await cache.put('/', response);
        const assetUrls = [...html.matchAll(/(?:src|href)="(\/[^"]+)"/g)]
          .map((match) => match[1])
          .filter((url) => /^\/(app\.js|style\.css|apple-touch-icon\.png)(\?|$)/.test(url));
        await Promise.all(assetUrls.map(
          (url) => cache.add(new Request(url, { cache: 'reload' })).catch(() => { /* 单个资源失败不影响安装 */ })));
      }
    } catch (error) {
      // 忽略：没有缓存也还能正常工作，只是离线时打不开
    }
    await self.skipWaiting();
  })());
});

self.addEventListener('activate', (event) => {
  event.waitUntil((async () => {
    for (const key of await caches.keys()) {
      if (key !== CACHE_NAME) {
        await caches.delete(key);
      }
    }
    await self.clients.claim();
  })());
});

self.addEventListener('fetch', (event) => {
  const request = event.request;
  if (request.method !== 'GET') {
    return;
  }

  const url = new URL(request.url);
  if (url.origin !== self.location.origin || url.pathname.startsWith('/api/')) {
    return;   // 接口与跨域请求一律交给网络
  }

  event.respondWith((async () => {
    const cache = await caches.open(CACHE_NAME);

    // 带版本号的静态资源：缓存优先
    if (isAsset(url) && url.searchParams.has('v')) {
      const hit = await cache.match(request);
      if (hit) {
        return hit;
      }
      const response = await fetch(request);
      if (response.ok) {
        cache.put(request, response.clone());
      }
      return response;
    }

    // 页面与无版本号资源：网络优先，失败回落缓存
    try {
      const response = await fetch(request);
      if (response.ok) {
        if (request.mode === 'navigate') {
          cache.put('/', response.clone());        // 入口页统一按 '/' 存
        } else if (isAsset(url)) {
          cache.put(request, response.clone());
        }
      }
      return response;
    } catch (error) {
      const fallback = (request.mode === 'navigate' ? await cache.match('/') : null)
                       || await cache.match(request);
      if (fallback) {
        return fallback;
      }
      throw error;
    }
  })());
});
