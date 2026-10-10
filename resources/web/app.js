/*
 * iPad 前端的全部逻辑。
 *
 * 结构：
 *   * 三个 tab（未提交 / 已提交 / 设置）在同一个页面里切换，不重新加载；
 *   * 轮询：状态 2 秒，两个列表 30 秒；页面切到后台就停（省电），回前台立刻刷新；
 *   * 写操作（改设置、触发同步）都必须带访问令牌，并且**只以后端返回的能力为准**：
 *     后端说不能做（正在同步）就置灰按钮、把原因显示出来，前端不自己猜状态。
 */

'use strict';

// 轮询间隔：采集进行中用快档（进度要跟得上），空闲时用慢档
// （实测服务端请求只要 1~5 ms，慢档纯粹是为了省 iPad 的射频唤醒与电量）
const POLL_FAST_MS = 2000;
const POLL_IDLE_MS = 6000;
const LIST_INTERVAL_MS = 30000;
const OFFLINE_AFTER_MS = 8000;

const FONT_SIZES = { small: '15px', normal: '17px', large: '19.5px', xlarge: '22px' };

const state = {
  tab: localStorage.getItem('njulms.tab') || 'pending',
  fontSize: localStorage.getItem('njulms.fontSize') || 'normal',
  token: localStorage.getItem('njulms.token') || '',
  status: null,
  pending: [],
  submitted: [],
  openPending: new Set(),
  openCourses: new Set(),
  lastOkAt: Date.now(),
  lastListsAt: 0,
  /// 是否成功拉到过作业列表（决定断线时是显示占位说明还是保留旧数据）
  listsLoaded: false,
  /// 是否成功连上过后端（决定断线提示是"立刻"还是"等几秒"）
  connectedOnce: false,
  timer: null,
  busy: false,
  /// 上一帧看到的那次运行：{ id, running }。用来识别"采集刚结束"
  runState: { id: 0, running: false },
};

const el = (id) => document.getElementById(id);

function escapeHtml(value) {
  return String(value ?? '').replace(/[&<>"']/g, (ch) => ({
    '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;',
  }[ch]));
}

// ---------------------------------------------------------------------------
// 网络
// ---------------------------------------------------------------------------

class ApiError extends Error {
  constructor(message, status) {
    super(message);
    this.status = status;
  }
}

async function api(path, options = {}) {
  const init = {
    headers: { Accept: 'application/json' },
    cache: 'no-store',
    method: options.method || 'GET',
  };
  if (options.body !== undefined) {
    init.headers['Content-Type'] = 'application/json';
    init.body = JSON.stringify(options.body);
  }
  if (options.auth) {
    init.headers['X-Auth-Token'] = state.token;
  }

  let response;
  try {
    response = await fetch(path, init);
  } catch (error) {
    throw new ApiError('请求失败（Failed to fetch）', 0);
  }

  const payload = await response.json().catch(() => null);
  if (!response.ok || !payload || payload.ok !== true) {
    let message = payload && payload.error ? payload.error.message : `HTTP ${response.status}`;
    if (!payload && response.status >= 500) {
      // PC 上的程序没运行时，Tailscale 的代理会回 502/503 —— 对用户来说就是"程序没开"
      message = 'PC 上的程序没有运行';
    }
    throw new ApiError(message, response.status);
  }
  return payload.data;
}

function toast(message, isError) {
  const node = el('toast');
  node.textContent = message;
  node.className = isError ? 'toast error' : 'toast';
  node.hidden = false;
  clearTimeout(toast.timer);
  toast.timer = setTimeout(() => { node.hidden = true; }, isError ? 6000 : 3000);
}

/**
 * 在原站打开（跳转 Safari）。
 *
 * 为什么是 window.open 而不是 location.href：这是"添加到主屏幕"的独立网页 app，
 * 它在 iOS 上只能留在自己的域名里；用 location.href 跳外域会把 **app 窗口本身**
 * 导航走（回不来，只能杀掉重开）。window.open / target=_blank 会交给系统打开 Safari，
 * 我们的 app 只是进后台，切回来还在原处。
 */
function openExternal(url) {
  if (!url) {
    return;
  }
  const opened = window.open(url, '_blank', 'noopener');
  if (opened) {
    return;
  }
  // 某些情况下 window.open 会被拦，兜底用一次性的 a[target=_blank]
  const anchor = document.createElement('a');
  anchor.href = url;
  anchor.target = '_blank';
  anchor.rel = 'noopener';
  document.body.appendChild(anchor);
  anchor.click();
  anchor.remove();
}

function setLinkBadge(offline) {
  el('link-badge').hidden = !offline;
}

/** 写操作统一入口：处理令牌缺失/过期与被拒绝的原因 */
async function writeAction(path, body, successMessage, needConfirm) {
  if (state.busy) {
    return null;
  }
  if (needConfirm && !window.confirm(needConfirm)) {
    return null;
  }
  state.busy = true;
  try {
    const data = await api(path, { method: 'POST', body, auth: true });
    if (successMessage) {
      toast(successMessage, false);
    }
    if (data && data.sync) {
      renderStatus(data);
    }
    return data;
  } catch (error) {
    if (error.status === 401) {
      toast('访问令牌不对：请在「设置 → 访问令牌」里粘贴 PC 端托盘菜单复制的那串', true);
    } else {
      toast(error.message, true);
    }
    return null;
  } finally {
    state.busy = false;
  }
}

// ---------------------------------------------------------------------------
// 渲染：状态
// ---------------------------------------------------------------------------

const AUTH_CLASS = {
  good: 'badge badge-good',
  bad: 'badge badge-bad',
  warn: 'badge badge-warn',
  unknown: 'badge badge-unknown',
  busy: 'badge badge-muted',
};

function renderStatus(data) {
  state.status = data;
  const auth = data.auth || {};
  const badge = el('auth-badge');
  badge.textContent = auth.display || '未知';
  badge.className = AUTH_CLASS[auth.tone] || AUTH_CLASS.unknown;

  const sync = data.sync || {};
  const percent = Number.isFinite(sync.percent) ? sync.percent : 0;
  el('progress-fill').style.width = `${percent}%`;
  el('progress-percent').textContent = `${percent}%`;
  el('sync-status').textContent = sync.statusMessage || (sync.running ? '正在采集…' : '空闲');

  const last = data.lastRun;
  el('last-run').textContent = last
    ? `#${last.runId} ${last.statusDisplay} · 开始 ${last.startedAt}` +
      (last.finishedAt ? ` · 结束 ${last.finishedAt}` : '') +
      (last.message ? ` · ${last.message}` : '')
    : '尚未执行';

  const schedule = data.schedule || {};
  el('next-run').textContent = schedule.enabled
    ? `${schedule.nextRunAt || '已启用'}（每 ${schedule.intervalMinutes} 分钟）`
    : '未启用（定时器仅在 PC 端程序运行期间有效）';

  renderSettings(data);
}

// ---------------------------------------------------------------------------
// 渲染：设置页
// ---------------------------------------------------------------------------

function setCheckedIf(id, value) {
  const node = el(id);
  if (document.activeElement !== node) {
    node.checked = Boolean(value);
  }
}

function renderSettings(data) {
  const settings = data.settings || {};
  const caps = data.capabilities || {};
  const sync = data.sync || {};

  setCheckedIf('set-schedule-enabled', settings.scheduleEnabled);
  setCheckedIf('set-quick', settings.quickIncremental);
  setCheckedIf('set-startup-scan', settings.startupFullScan);
  setCheckedIf('set-enrich', settings.enrichDescriptions);

  const interval = el('set-interval');
  if (document.activeElement !== interval && typeof settings.scheduleIntervalMinutes === 'number') {
    interval.value = settings.scheduleIntervalMinutes;
  }
  if (typeof caps.scheduleIntervalMin === 'number') {
    interval.min = caps.scheduleIntervalMin;
    interval.max = caps.scheduleIntervalMax;
  }

  const running = Boolean(sync.running);
  const paused = Boolean(sync.paused);
  const canStart = caps.canStartSync !== false;
  const canChangeSchedule = caps.canChangeSchedule !== false;

  el('btn-incremental').disabled = !canStart || state.busy;
  el('btn-full').disabled = !canStart || state.busy;
  el('btn-pause').hidden = !caps.canPause;
  el('btn-resume').hidden = !caps.canResume;
  el('btn-cancel').hidden = !caps.canCancel;
  el('set-schedule-enabled').disabled = !canChangeSchedule;
  el('set-interval').disabled = !canChangeSchedule;
  el('save-interval').disabled = !canChangeSchedule;
  el('btn-purge').disabled = caps.canPurge === false || state.busy;

  const blocked = el('blocked-note');
  if (caps.blockedReason) {
    blocked.hidden = false;
    blocked.textContent = running
      ? `正在同步：${sync.statusMessage || ''}（可以暂停或取消；设置改不了）`
      : caps.blockedReason;
  } else {
    blocked.hidden = true;
  }
  if (paused) {
    blocked.hidden = false;
    blocked.textContent = '任务已暂停（登录可能已失效，请在 PC 上处理），点「继续」恢复。';
  }
}

// ---------------------------------------------------------------------------
// 渲染：未提交（todo 卡片）
// ---------------------------------------------------------------------------

function pendingKey(row) {
  return `${row.courseId}/${row.sourceId}`;
}

function renderPending() {
  const container = el('pending-list');
  const count = state.pending.length;
  el('pending-count').textContent = count ? String(count) : '';
  el('pending-meta').textContent = count ? `按截止时间由近到远 · ${count} 条` : '';

  if (!count) {
    container.innerHTML = '<p class="empty">没有未提交的作业。<br>数据来自 PC 端最近一次采集。</p>';
    return;
  }

  container.innerHTML = state.pending.map((row) => {
    const key = pendingKey(row);
    const open = state.openPending.has(key);
    const overdue = row.status === '已逾期';
    const description = row.descriptionText
      ? `<p class="description">${escapeHtml(row.descriptionText)}${
          row.descriptionTruncated ? '\n（内容过长，已截断）' : ''}</p>`
      : '<p class="description empty">站点没有提供这条作业的说明。</p>';
    const jump = row.detailUrl
      ? `<button class="item-jump" type="button" data-detail-url="${escapeHtml(row.detailUrl)}"
                 aria-label="在原网站打开这条作业">↗</button>`
      : '';
    return `
      <article class="item${open ? ' open' : ''}" data-key="${escapeHtml(key)}">
        <div class="item-head">
          <button class="item-toggle" type="button" aria-expanded="${open}">
            <span class="item-main">
              <span class="item-title">${escapeHtml(row.title)}</span>
              <span class="item-meta">截止 ${escapeHtml(row.dueAt || '未知')}${
                row.dueRelative ? ' · ' + escapeHtml(row.dueRelative) : ''}</span>
              <span class="item-meta">${escapeHtml(row.courseName || '')}</span>
            </span>
            <span class="item-side">
              ${overdue ? '<span class="chip chip-bad">已逾期</span>' : ''}
              <span class="chev"></span>
            </span>
          </button>
          ${jump}
        </div>
        <div class="collapse">
          <div class="collapse-inner">
            <div class="item-body">
              <h3>作业内容与要求</h3>
              ${description}
              <dl class="meta-grid">
                <dt>课程</dt><dd>${escapeHtml(row.courseName || '')}</dd>
                <dt>状态</dt><dd>${escapeHtml(row.status)}</dd>
                <dt>发布</dt><dd class="mono">${escapeHtml(row.publishedAt || '未知')}</dd>
                <dt>来源</dt><dd class="mono">${escapeHtml(row.sourceId)}</dd>
              </dl>
            </div>
          </div>
        </div>
      </article>`;
  }).join('');
}

// ---------------------------------------------------------------------------
// 渲染：已提交（按课程分组）
// ---------------------------------------------------------------------------

function groupByCourse(rows) {
  const groups = new Map();
  for (const row of rows) {
    const key = row.courseId || 'unknown';
    if (!groups.has(key)) {
      groups.set(key, { id: key, name: row.courseName || '未知课程', rows: [] });
    }
    groups.get(key).rows.push(row);
  }
  return [...groups.values()];
}

function renderSubmitted() {
  const container = el('submitted-list');
  const count = state.submitted.length;
  el('submitted-count').textContent = count ? String(count) : '';
  el('submitted-meta').textContent = count ? `按课程分组 · ${count} 条` : '';

  if (!count) {
    container.innerHTML = '<p class="empty">还没有已提交的作业记录。</p>';
    return;
  }

  container.innerHTML = groupByCourse(state.submitted).map((group) => {
    const open = state.openCourses.has(group.id);
    const items = group.rows.map((row) => `
      <li class="sub-item">
        <span class="sub-main">
          <span class="sub-title">${escapeHtml(row.title)}</span>
          <span class="sub-meta">
            <span>截止 ${escapeHtml(row.dueAt || '未知')}</span>
            <span>发布 ${escapeHtml(row.publishedAt || '未知')}</span>
            <span>得分 ${escapeHtml(row.score)}</span>
            <span class="mono">${escapeHtml(row.sourceId)}</span>
          </span>
        </span>
        ${row.detailUrl
          ? `<button class="item-jump sub-jump" type="button"
                     data-detail-url="${escapeHtml(row.detailUrl)}"
                     aria-label="在原网站打开这条作业">↗</button>`
          : ''}
      </li>`).join('');
    return `
      <article class="item${open ? ' open' : ''}" data-course="${escapeHtml(group.id)}">
        <div class="item-head">
          <button class="item-toggle" type="button" aria-expanded="${open}">
            <span class="item-main">
              <span class="item-title">${escapeHtml(group.name)}</span>
              <span class="item-meta">${group.rows.length} 条已提交 · 按发布时间由新到旧</span>
            </span>
            <span class="item-side"><span class="chev"></span></span>
          </button>
        </div>
        <div class="collapse">
          <div class="collapse-inner">
            <div class="item-body"><ul class="sub-list">${items}</ul></div>
          </div>
        </div>
      </article>`;
  }).join('');
}

// ---------------------------------------------------------------------------
// 数据加载
// ---------------------------------------------------------------------------

async function loadLists(force) {
  const now = Date.now();
  if (!force && now - state.lastListsAt < LIST_INTERVAL_MS) {
    return;
  }
  state.lastListsAt = now;
  const [pending, submitted] = await Promise.all([
    api('/api/assignments?scope=pending'),
    api('/api/assignments?scope=submitted'),
  ]);
  state.pending = pending;
  state.submitted = submitted;
  state.listsLoaded = true;   // 之后即使断线也保留最后一次的数据
  renderPending();
  renderSubmitted();
}

/**
 * 后端连不上时的占位文案。
 *
 * 场景：PC 上的程序还没打开就先打开了这个网页 app。因为页面外壳由 Service Worker
 * 缓存着，页面本身能正常显示 —— 这里把两个列表换成明确的说明，而不是停在
 * "正在读取…"上；后端一起来，下一轮轮询就会自动填上数据，不需要重开网页 app。
 * 如果之前已经成功加载过数据，则保留旧内容（不要在断线时清空）。
 */
function renderOfflinePlaceholders() {
  if (state.listsLoaded) {
    return;
  }
  const hint = '<p class="empty">连不上 PC 上的采集器。<br>'
    + '请确认电脑上的程序正在运行（托盘里有它的图标）。<br>'
    + '程序启动后本页会自动刷新，不需要重开。</p>';
  el('pending-list').innerHTML = hint;
  el('submitted-list').innerHTML = hint;
  el('pending-meta').textContent = '';
  el('submitted-meta').textContent = '';
  el('last-run').textContent = '尚未连接';
  el('next-run').textContent = '尚未连接';
}

/**
 * 轮询一次：先取状态，顺带识别"采集刚刚结束"，结束后强制刷新一次列表。
 *
 * 为什么要自动刷新：采集是 PC 那边跑完的，页面自己不刷新就会一直显示旧数据。
 * 这里用「上一帧还在跑 / 这次换了 runId」来判断，PC 上手动点的、定时任务触发的、
 * iPad 上自己发起的三条路径都覆盖得到（都以 /api/status 为准，不另做一套判断）。
 *
 * @return {boolean} 状态取到了就返回 true（列表失败不算掉线）
 */
async function tick(force) {
  let data;
  try {
    data = await api('/api/status');
  } catch (error) {
    // 从没连上过就立刻亮红标；连上过则要连续失败超过阈值才提示（避免抖动）
    setLinkBadge(!state.connectedOnce || Date.now() - state.lastOkAt > OFFLINE_AFTER_MS);
    el('sync-status').textContent = `无法连接 PC 上的采集器：${error.message}`;
    renderOfflinePlaceholders();
    return false;
  }

  const run = data.lastRun || null;
  const runId = run ? Number(run.runId) : 0;
  const running = Boolean(data.sync && data.sync.running);
  const previous = state.runState;
  state.runState = { id: runId, running };
  // 首帧只记录（避免刚打开页面就弹一次"已刷新"）
  const completedNow = runId > 0 && !running && previous.id !== 0
                       && (runId !== previous.id || previous.running);

  renderStatus(data);
  state.connectedOnce = true;
  state.lastOkAt = Date.now();
  setLinkBadge(false);

  if (completedNow) {
    state.lastListsAt = 0;
    try {
      await loadLists(true);
    } catch (error) {
      // 列表失败不阻塞：下一轮轮询会再试
    }
    const status = run.statusDisplay || '';
    const troubled = /失败|等待重新登录|已取消/.test(status);
    toast(troubled ? `同步结束（${status || '未完成'}），数据已刷新` : '采集完成，数据已刷新',
          troubled);
  } else {
    try {
      await loadLists(force);
    } catch (error) {
      // 列表失败不改判「掉线」：保留上一次的数据，等下一轮轮询
    }
  }
  return true;
}

/// 手动刷新：按钮上转圈 → 完成/失败各给一个短反馈
async function manualRefresh() {
  const button = el('sync-refresh');
  if (button.dataset.busy === '1') {
    return;
  }
  button.dataset.busy = '1';
  button.disabled = true;
  button.innerHTML = '<span class="spinner" aria-hidden="true"></span>刷新中';

  const startedAt = Date.now();
  state.lastListsAt = 0;
  const ok = await tick(true);

  button.classList.toggle('done', ok);
  button.textContent = ok ? '已刷新' : '刷新失败';
  button.classList.toggle('failed', !ok);
  // 至少让反馈停留一会儿，避免"闪一下就没了"
  const wait = Math.max(500, 1100 - (Date.now() - startedAt));
  setTimeout(() => {
    button.classList.remove('done', 'failed');
    button.textContent = '刷新';
    button.disabled = false;
    button.dataset.busy = '0';
  }, wait);
}

/**
 * 安排下一次轮询。
 *
 * 用 setTimeout 链而不是 setInterval，是为了按当前状态换挡：
 * 采集进行中 2 秒一次，空闲 6 秒一次（切到后台则完全停）。
 */
function schedule() {
  if (state.timer) {
    clearTimeout(state.timer);
  }
  const delay = state.runState.running ? POLL_FAST_MS : POLL_IDLE_MS;
  state.timer = setTimeout(async () => {
    await tick(false);
    schedule();
  }, delay);
}

// ---------------------------------------------------------------------------
// 交互
// ---------------------------------------------------------------------------

function selectTab(tab) {
  if (!['pending', 'submitted', 'settings'].includes(tab)) {
    tab = 'pending';
  }
  state.tab = tab;
  localStorage.setItem('njulms.tab', tab);
  // 写进 URL：可以直接把「设置」页加到主屏幕，也方便分享某个 tab
  const url = new URL(location.href);
  url.searchParams.set('tab', tab);
  history.replaceState(null, '', url);
  for (const button of document.querySelectorAll('.tab')) {
    button.setAttribute('aria-selected', String(button.dataset.tab === tab));
  }
  el('view-pending').hidden = tab !== 'pending';
  el('view-submitted').hidden = tab !== 'submitted';
  el('view-settings').hidden = tab !== 'settings';
  window.scrollTo({ top: 0 });
}

function applyFontSize(size) {
  state.fontSize = FONT_SIZES[size] ? size : 'normal';
  localStorage.setItem('njulms.fontSize', state.fontSize);
  document.documentElement.style.setProperty('--base', FONT_SIZES[state.fontSize]);
  for (const button of document.querySelectorAll('#font-size-group button')) {
    button.setAttribute('aria-pressed', String(button.dataset.size === state.fontSize));
  }
}

function refreshTokenState() {
  el('token-state').textContent = state.token
    ? '已保存令牌：改设置、触发同步都会带上它。'
    : '只读浏览不需要令牌；改设置、触发同步需要（在 PC 端托盘菜单「复制访问令牌」）。';
}

/**
 * 在「关于」里显示离线外壳的状态。
 *
 * 为什么要有这一行：主屏图标（独立网页 app）可能和 Safari 是各自独立的存储，
 * 只有在**这个上下文里**成功联网打开过一次，离线外壳才装得上。
 * 用户一眼就能看出"到底装没装"，不用猜。
 */
function refreshShellState() {
  const node = el('about-shell');
  if (!node) {
    return;
  }
  const supported = 'serviceWorker' in navigator;
  const controlled = supported && navigator.serviceWorker.controller !== null;
  node.textContent = controlled
    ? '离线外壳：已生效（PC 没开机也能打开本页）'
    : '离线外壳：未生效 —— 请在 PC 程序运行时，用这个主屏图标打开一次本页';
}

document.addEventListener('gesturestart', (event) => event.preventDefault());
document.addEventListener('gesturechange', (event) => event.preventDefault());
document.addEventListener('gestureend', (event) => event.preventDefault());
document.addEventListener('touchmove', (event) => {
  if (event.touches.length > 1) {
    event.preventDefault();   // 双指捏合缩放：iOS Safari 会忽略 user-scalable=no
  }
}, { passive: false });

document.querySelector('.tabs').addEventListener('click', (event) => {
  const button = event.target.closest('.tab');
  if (button) {
    selectTab(button.dataset.tab);
  }
});

el('pending-list').addEventListener('click', (event) => {
  const button = event.target.closest('.item-toggle');
  if (!button) {
    return;
  }
  const item = button.closest('.item');
  const key = item.dataset.key;
  const open = !item.classList.contains('open');
  item.classList.toggle('open', open);
  button.setAttribute('aria-expanded', String(open));
  if (open) {
    state.openPending.add(key);
  } else {
    state.openPending.delete(key);
  }
});

el('submitted-list').addEventListener('click', (event) => {
  const button = event.target.closest('.item-toggle');
  if (!button) {
    return;
  }
  const item = button.closest('.item');
  const key = item.dataset.course;
  const open = !item.classList.contains('open');
  item.classList.toggle('open', open);
  button.setAttribute('aria-expanded', String(open));
  if (open) {
    state.openCourses.add(key);
  } else {
    state.openCourses.delete(key);
  }
});

// 原站详情页（跳 Safari）：用捕获阶段处理，避免同时触发卡片的展开/收起
document.addEventListener('click', (event) => {
  const jump = event.target.closest('[data-detail-url]');
  if (!jump) {
    return;
  }
  event.preventDefault();
  event.stopPropagation();
  openExternal(jump.dataset.detailUrl);
}, true);

// 手动同步
el('btn-incremental').addEventListener('click', async () => {
  await writeAction('/api/sync', { mode: 'incremental' }, '已开始增量同步');
  tick(true);
});

el('btn-full').addEventListener('click', async () => {
  await writeAction('/api/sync', { mode: 'full' }, '已开始全量采集',
                    '全量采集会把所有课程完整扫一遍（13 门课约 10 秒、十几个请求），确定开始吗？');
  tick(true);
});

el('btn-pause').addEventListener('click', async () => {
  await writeAction('/api/sync/pause', undefined, '已暂停');
  tick(true);
});

el('btn-resume').addEventListener('click', async () => {
  await writeAction('/api/sync/resume', undefined, '已继续');
  tick(true);
});

el('btn-cancel').addEventListener('click', async () => {
  await writeAction('/api/sync/cancel', undefined, '已取消', '确定取消当前同步吗？');
  tick(true);
});

el('sync-refresh').addEventListener('click', () => manualRefresh());

// 设置项：每次只提交改动的那个键
for (const [id, key] of [
  ['set-schedule-enabled', 'scheduleEnabled'],
  ['set-quick', 'quickIncremental'],
  ['set-startup-scan', 'startupFullScan'],
  ['set-enrich', 'enrichDescriptions'],
]) {
  el(id).addEventListener('change', async (event) => {
    const value = event.target.checked;
    const data = await writeAction('/api/settings', { [key]: value },
                                   value ? '已开启' : '已关闭');
    if (!data) {
      event.target.checked = !value;   // 失败回滚，保持界面与后端一致
    }
  });
}

el('save-interval').addEventListener('click', async () => {
  const value = Number(el('set-interval').value);
  if (!Number.isFinite(value)) {
    toast('请填一个数字', true);
    return;
  }
  const data = await writeAction('/api/settings', { scheduleIntervalMinutes: value },
                                 `周期已改为 ${value} 分钟`);
  if (!data && state.status) {
    renderSettings(state.status);
  }
});

el('font-size-group').addEventListener('click', (event) => {
  const button = event.target.closest('button[data-size]');
  if (button) {
    applyFontSize(button.dataset.size);
  }
});

el('save-token').addEventListener('click', () => {
  state.token = el('set-token').value.trim();
  localStorage.setItem('njulms.token', state.token);
  el('set-token').value = '';
  refreshTokenState();
  toast(state.token ? '令牌已保存在本机浏览器里' : '已清除令牌', false);
});

// ---- 删除全部数据：先弹警告窗，勾选确认后才允许点删除 ----
function openPurgeModal() {
  el('purge-ack').checked = false;
  el('purge-confirm').disabled = true;
  el('purge-modal').hidden = false;
}

function closePurgeModal() {
  el('purge-modal').hidden = true;
}

el('btn-purge').addEventListener('click', openPurgeModal);
el('purge-cancel').addEventListener('click', closePurgeModal);
el('purge-ack').addEventListener('change', (event) => {
  el('purge-confirm').disabled = !event.target.checked;
});
el('purge-modal').addEventListener('click', (event) => {
  if (event.target === el('purge-modal')) {
    closePurgeModal();   // 点遮罩关闭，避免误触后无法退出
  }
});

el('purge-confirm').addEventListener('click', async () => {
  const data = await writeAction('/api/data/purge', { confirm: 'DELETE' });
  if (data) {
    closePurgeModal();
    toast(`已删除 ${data.removed} 行采集数据（登录状态保留）`, false);
    state.lastListsAt = 0;
    tick(true);
  }
});

document.addEventListener('visibilitychange', () => {
  if (document.hidden) {
    if (state.timer) {
      clearTimeout(state.timer);
      state.timer = null;
    }
  } else {
    tick(true);
    schedule();
  }
});

// ---------------------------------------------------------------------------
// 启动
// ---------------------------------------------------------------------------

applyFontSize(state.fontSize);
refreshTokenState();
selectTab(new URLSearchParams(location.search).get('tab') || state.tab);

// 脚本活过来了：撤掉 HTML 里的兜底提示（那个提示只在 app.js 加载失败时出现）
window.__njulmsBooted = true;
const bootFallback = el('boot-fallback');
if (bootFallback) {
  bootFallback.hidden = true;
}

// 注册离线外壳：让"PC 上的程序没开"时页面也能打开（显示连不上、等后端回来自动恢复）
if ('serviceWorker' in navigator
    && (location.protocol === 'https:'
        || location.hostname === 'localhost' || location.hostname === '127.0.0.1')) {
  navigator.serviceWorker.register('/service-worker.js', { updateViaCache: 'none' })
    .then((registration) => {
      // 每次打开都主动查一次更新：外壳修好后不用等浏览器自己想起来
      registration.update().catch(() => {});
      return navigator.serviceWorker.ready;
    })
    .catch(() => { /* 注册失败不影响正常使用 */ });
  // 外壳是否真的在接管本页（主屏图标有独立的存储，得在这个上下文里装一次）
  navigator.serviceWorker.addEventListener('controllerchange', refreshShellState);
}
refreshShellState();

el('about-line').textContent =
  `本页地址：${location.origin}/（仅 Tailscale 内网可见）· 只读接口 + 令牌保护的写操作`;
// 资源版本：用来确认 iPad 上跑的是不是最新前端（旧页面会一直停在旧哈希上）
const scriptSrc = document.querySelector('script[src*="app.js"]')?.getAttribute('src') || '';
const assetVersion = new URL(scriptSrc, location.origin).searchParams.get('v') || '—';
el('about-version').textContent = `资源版本 ${assetVersion}`;

tick(true);
schedule();
