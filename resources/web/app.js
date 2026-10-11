/*
 * iPad 前端的全部逻辑。
 *
 * 结构：
 *   * 四个 tab（未提交 / 已提交 / 已删除 / 设置）在同一个页面里切换，不重新加载；
 *   * 轮询：状态 2 秒，列表 30 秒；页面切到后台就停（省电），回前台立刻刷新；
 *   * 写操作（改设置、触发同步）都必须带访问令牌，并且**只以后端返回的能力为准**：
 *     后端说不能做（正在同步）就置灰按钮、把原因显示出来，前端不自己猜状态。
 *
 * 删除是**软删除**：只标记、不删数据，可以在「已删除」里恢复；
 * 动画（条目退场/入场、布局 FLIP）与数据状态严格分离：
 *   * 点击删除 → 立刻播退场动画（不等网络）；
 *   * 后端确认成功 → 才提交本地状态；失败 → 反向恢复 + 提示原因；
 *   * 有退场动画在播时，后台轮询的列表重建会被挂起（uiOps / renderDirty），
 *     避免"动画播到一半节点被重建"导致的断帧与跳位。
 */

'use strict';

// 轮询间隔：采集进行中用快档（进度要跟得上），空闲时用慢档
// （实测服务端请求只要 1~5 ms，慢档纯粹是为了省 iPad 的射频唤醒与电量）
const POLL_FAST_MS = 2000;
const POLL_IDLE_MS = 6000;
const LIST_INTERVAL_MS = 30000;
const OFFLINE_AFTER_MS = 8000;

const FONT_SIZES = { small: '15px', normal: '17px', large: '19.5px', xlarge: '22px' };

/**
 * 动画规范（与 style.css 里的 --dur-* / --ease-* 一一对应，改一处要同步另一处）。
 * 只调一条曲线、一组时长：按压 → 条目 → 布局 → 页面，速度依次放慢。
 */
const MOTION = {
  item: 240,       // 条目增删（180–280ms）
  layout: 280,     // 卡片重排 FLIP（220–350ms）
  page: 260,       // tab 切换（250–400ms）
  stagger: 30,     // 多条同时入场时的错开量
  maxStagger: 4,   // 错开的最多条数（再多就一起进，不做"多米诺"）
  easeOut: 'cubic-bezier(0.22, 0.61, 0.36, 1)',
  easeInOut: 'cubic-bezier(0.4, 0, 0.2, 1)',
};

/// 系统"减少动态效果"：开着一律不做非必要动画（读的是实时值，改设置立即生效）
const motionQuery = window.matchMedia('(prefers-reduced-motion: reduce)');
const reduceMotion = () => motionQuery.matches;

const state = {
  tab: localStorage.getItem('njulms.tab') || 'pending',
  fontSize: localStorage.getItem('njulms.fontSize') || 'normal',
  token: localStorage.getItem('njulms.token') || '',
  status: null,
  pending: [],
  submitted: [],
  /// 回收站（软删除的作业，服务端 scope=deleted）
  deleted: [],
  openPending: new Set(),
  openDeleted: new Set(),
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
  /// 正在播放的删除/恢复退场动画数：>0 时列表重建被挂起（见 renderDirty）
  uiOps: 0,
  /// 正在进行的左滑手势（见"左滑删除手势"小节）；非空时也挂起列表重建
  swipe: null,
  /// 哪些列表有"被挂起的重建"（数组更新了但没画），动画结束后统一补渲染
  renderDirty: { pending: false, deleted: false },
  /// 上一次真正渲染出来的 key 集合：新 key 播入场动画、老 key 做 FLIP 差分
  pendingSnapshot: null,
  deletedSnapshot: null,
  pendingRenderedOnce: false,
  deletedRenderedOnce: false,
  /// 首屏（第一次 selectTab）不做页面转场，避免刚打开就"闪一下"
  booted: false,
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

/**
 * 提示条。可选的第三个参数带一个动作按钮（删除后的「撤销」）：
 *   toast('已删除「xx」', false, { label: '撤销', seconds: 6, onClick() {} })
 * 动作只在提示条可见期间有效；弹出新提示时旧动作自动被替换。
 */
function toast(message, isError, action) {
  const node = el('toast');
  el('toast-text').textContent = message;
  const button = el('toast-action');
  if (action && action.label) {
    button.hidden = false;
    button.textContent = action.label;
    button.onclick = () => {
      hideToast();
      action.onClick();
    };
  } else {
    button.hidden = true;
    button.onclick = null;
  }
  node.className = isError ? 'toast error' : 'toast';
  node.hidden = false;
  clearTimeout(toast.timer);
  const holdMs = action && action.seconds ? action.seconds * 1000 : (isError ? 6000 : 3000);
  toast.timer = setTimeout(hideToast, holdMs);
}

function hideToast() {
  clearTimeout(toast.timer);
  const node = el('toast');
  if (node) {
    node.hidden = true;
  }
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
  el('sync-status').textContent =
    sync.statusMessage ||
    (sync.running || sync.chaoxingRunning ? '正在采集…' : '空闲');

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

/// 类型胶囊标签（放每张卡片最前端）：作业绿 / 考试紫 / 其它中性灰
const KIND_TAG_CLASS = { homework: 'tag-homework', exam: 'tag-exam' };

function kindTag(row) {
  const label = row.kindLabel || row.kind || '作业';
  const cls = KIND_TAG_CLASS[row.kind] || 'tag-other';
  return `<span class="tag ${cls}">${escapeHtml(label)}</span>`;
}

/// 行尾跳转按钮（原站详情页，交给 Safari）
function jumpButton(row) {
  if (!row.detailUrl) {
    return '';
  }
  return `<button class="item-jump" type="button" data-detail-url="${escapeHtml(row.detailUrl)}"
                 aria-label="在原网站打开这条作业">↗</button>`;
}

/// 展开区的作业说明（未提交 / 已删除共用）
function descriptionHtml(row) {
  if (!row.descriptionText) {
    return '<p class="description empty">站点没有提供这条作业的说明。</p>';
  }
  const truncated = row.descriptionTruncated ? '\n（内容过长，已截断）' : '';
  return `<p class="description">${escapeHtml(row.descriptionText)}${truncated}</p>`;
}

/// 垃圾桶图标（内联 SVG：不引外部资源，颜色跟随 CSS 的 currentColor）
const ICON_TRASH = '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" '
  + 'stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">'
  + '<path d="M4 7h16M9.5 7V5.5A1.5 1.5 0 0 1 11 4h2a1.5 1.5 0 0 1 1.5 1.5V7"/>'
  + '<path d="m6.5 7 .7 11.1A2 2 0 0 0 9.2 20h5.6a2 2 0 0 0 2-1.9L17.5 7"/></svg>';

/**
 * 未提交卡片（todo 样式）。
 *
 * 结构上是"滑动容器 + 卡片 + 底层动作区"：
 *   * `.swipe` 是列表项本体（承担间距与塌缩动画），`touch-action: pan-y` 让竖向
 *     滚动照常交给浏览器、横向拖动交给我们；
 *   * `.swipe-backing` 是卡片底下的红色删除区，随左滑进度浮现；
 *   * 卡片左滑由 pointer 事件驱动（见下方"左滑删除"小节）。
 */
function pendingItemHtml(row) {
  const key = pendingKey(row);
  const open = state.openPending.has(key);
  const overdue = row.status === '已逾期';
  // 倒计时（剩余时间）：临期 ≤48 小时或已逾期时标红（后端 dueUrgent 已按 UTC 精确判断）
  const countdown = row.dueRelative
    ? ` · <span class="countdown${row.dueUrgent ? ' urgent' : ''}">${escapeHtml(row.dueRelative)}</span>`
    : '';
  return `
    <div class="swipe" data-swipe-key="${escapeHtml(key)}">
      <div class="swipe-backing" aria-hidden="true">
        <span class="swipe-backing-label">${ICON_TRASH}<span>删除</span></span>
      </div>
      <article class="item${open ? ' open' : ''}" data-key="${escapeHtml(key)}">
        <div class="item-head">
          <button class="item-toggle" type="button" aria-expanded="${open}">
            ${kindTag(row)}
            <span class="item-main">
              <span class="item-title">${escapeHtml(row.title)}</span>
              <span class="item-meta">截止 ${escapeHtml(row.dueAt || '未知')}${countdown}</span>
              <span class="item-meta">${escapeHtml(row.courseName || '')}</span>
            </span>
            <span class="item-side">
              ${row.sourceLabel
                ? `<span class="chip chip-source">${escapeHtml(row.sourceLabel)}</span>` : ''}
              ${overdue ? '<span class="chip chip-bad">已逾期</span>' : ''}
              <span class="chev"></span>
            </span>
          </button>
          ${jumpButton(row)}
        </div>
        <div class="collapse">
          <div class="collapse-inner">
            <div class="item-body">
              <h3>作业内容与要求</h3>
              ${descriptionHtml(row)}
              <dl class="meta-grid">
                <dt>课程</dt><dd>${escapeHtml(row.courseName || '')}</dd>
                <dt>站点</dt><dd>${escapeHtml(row.sourceLabel || '未知')}</dd>
                <dt>类型</dt><dd>${escapeHtml(row.kindLabel || '作业')}</dd>
                <dt>状态</dt><dd>${escapeHtml(row.status)}</dd>
                <dt>发布</dt><dd class="mono">${escapeHtml(row.publishedAt || '未知')}</dd>
                <dt>来源</dt><dd class="mono">${escapeHtml(row.sourceId)}</dd>
              </dl>
            </div>
          </div>
        </div>
      </article>
    </div>`;
}

/// 已删除卡片：与未提交同构，行尾按钮换成「恢复」，元信息带删除时间
function deletedItemHtml(row) {
  const key = pendingKey(row);
  const open = state.openDeleted.has(key);
  return `
    <article class="item${open ? ' open' : ''}" data-key="${escapeHtml(key)}">
      <div class="item-head">
        <button class="item-toggle" type="button" aria-expanded="${open}">
          ${kindTag(row)}
          <span class="item-main">
            <span class="item-title">${escapeHtml(row.title)}</span>
            <span class="item-meta">删除于 ${escapeHtml(row.deletedAt || '未知')} · 截止 ${escapeHtml(row.dueAt || '未知')}</span>
            <span class="item-meta">${escapeHtml(row.courseName || '')}</span>
          </span>
          <span class="item-side">
            ${row.sourceLabel
              ? `<span class="chip chip-source">${escapeHtml(row.sourceLabel)}</span>` : ''}
            <span class="chev"></span>
          </span>
        </button>
        ${jumpButton(row)}
        <button class="item-restore" type="button" data-undo-key="${escapeHtml(key)}"
                aria-label="恢复这条作业">恢复</button>
      </div>
      <div class="collapse">
        <div class="collapse-inner">
          <div class="item-body">
            <h3>作业内容与要求</h3>
            ${descriptionHtml(row)}
            <dl class="meta-grid">
              <dt>课程</dt><dd>${escapeHtml(row.courseName || '')}</dd>
              <dt>站点</dt><dd>${escapeHtml(row.sourceLabel || '未知')}</dd>
              <dt>类型</dt><dd>${escapeHtml(row.kindLabel || '作业')}</dd>
              <dt>删除</dt><dd class="mono">${escapeHtml(row.deletedAt || '未知')}</dd>
              <dt>截止</dt><dd class="mono">${escapeHtml(row.dueAt || '未知')}</dd>
              <dt>来源</dt><dd class="mono">${escapeHtml(row.sourceId)}</dd>
            </dl>
          </div>
        </div>
      </div>
    </article>`;
}

// ---------------------------------------------------------------------------
// 动画：条目增删 / 布局重排（FLIP）
//
// 为什么用 Web Animations API 而不是 CSS class：删除流程要"等动画真的播完"
// 才能提交数据状态（成功）或反向恢复（失败）—— WAAPI 的 finished Promise
// 是唯一能可靠等待动画结束的方式；CSS transition 只能靠猜时长。
// ---------------------------------------------------------------------------

/// 记录容器里每个条目的纵向位置（相对容器顶部），供 FLIP 计算位移
function captureTops(container) {
  const tops = new Map();
  const base = container.getBoundingClientRect().top;
  for (const node of container.querySelectorAll('.item[data-key]')) {
    tops.set(node.dataset.key, node.getBoundingClientRect().top - base);
  }
  return tops;
}

/// FLIP：重建后把"位置变了的老条目"先用反向位移放回旧位置，再平滑归零
function playLayoutShift(container, prevTops) {
  if (reduceMotion()) {
    return;
  }
  const base = container.getBoundingClientRect().top;
  for (const node of container.querySelectorAll('.item[data-key]')) {
    const prev = prevTops.get(node.dataset.key);
    if (prev === undefined) {
      continue;
    }
    const delta = prev - (node.getBoundingClientRect().top - base);
    if (Math.abs(delta) < 1.5) {
      continue;   // 位置几乎没变：不播无意义的微动
    }
    node.animate(
      [{ transform: `translateY(${delta.toFixed(1)}px)` }, { transform: 'translateY(0)' }],
      { duration: MOTION.layout, easing: MOTION.easeOut },
    );
  }
}

/// 新条目入场：轻微缩小 + 小幅下移 + 淡入（不弹跳；多条错开一点点）
function playEnter(node, index) {
  if (reduceMotion()) {
    return;
  }
  const delay = Math.min(index, MOTION.maxStagger) * MOTION.stagger;
  node.animate([
    { opacity: 0, transform: 'scale(0.97) translateY(-6px)' },
    { opacity: 1, transform: 'none' },
  ], { duration: MOTION.item, easing: MOTION.easeOut, delay });
}

/**
 * 条目退场：高度 + 下方间距一起收到 0（邻居全程连续补位，不跳位），
 * 同时淡出并轻微缩小。
 * @return {Promise<{height:number, margin:number}>} 原尺寸，失败时用于反向恢复
 */
function playExit(card) {
  const geo = {
    height: card.offsetHeight,
    margin: parseFloat(getComputedStyle(card).marginBottom) || 0,
  };
  card.classList.add('leaving');
  if (reduceMotion()) {
    return Promise.resolve(geo);
  }
  const animation = card.animate([
    { height: `${geo.height}px`, marginBottom: `${geo.margin}px`, opacity: 1, transform: 'none' },
    { height: '0px', marginBottom: '0px', opacity: 0, transform: 'scale(0.97)' },
  ], { duration: MOTION.item, easing: MOTION.easeInOut, fill: 'forwards' });
  card.__exitAnimation = animation;
  // finished 在动画被取消时 reject；吞掉异常，调用方只关心"已经结束了"
  return animation.finished.catch(() => {}).then(() => geo);
}

/// 退场失败：取消退场（同一帧内接反向展开，不会闪一下全高）并恢复交互
function playExitBack(card, geo) {
  if (card.__exitAnimation) {
    card.__exitAnimation.cancel();
    card.__exitAnimation = null;
  }
  card.classList.remove('leaving');
  if (reduceMotion()) {
    return;
  }
  card.animate([
    { height: '0px', marginBottom: '0px', opacity: 0, transform: 'scale(0.97)' },
    { height: `${geo.height}px`, marginBottom: `${geo.margin}px`, opacity: 1, transform: 'none' },
  ], { duration: MOTION.item, easing: MOTION.easeOut });
}

/**
 * 软列表（未提交 / 已删除）的差量渲染。
 *
 * 流程：记录旧位置与旧 key → innerHTML 重建（沿用项目原有的模板渲染方式）→
 * 老条目位置变化播 FLIP、新条目播入场 → 更新快照。
 * 有退场动画在播时**挂起**（uiOps > 0）：否则重建会把正在退场的节点直接抽走，
 * 出现断帧、邻居跳位；挂起的渲染在最后一个动画结束时由 finishUiOp() 补上。
 * 首屏不播入场动画；tab 不可见时也不播（切过去时直接是最终样式）。
 */
function renderAnimatedList(cfg) {
  if (state.uiOps > 0 || state.swipe) {
    state.renderDirty[cfg.dirtyProp] = true;
    return;
  }
  const container = el(cfg.containerId);
  if (!cfg.rows.length) {
    container.innerHTML = cfg.emptyHtml;
    state[cfg.snapshotProp] = new Set();
    state[cfg.onceProp] = true;
    return;
  }

  const prevTops = captureTops(container);
  const prevKeys = state[cfg.snapshotProp];
  const wasRendered = state[cfg.onceProp];
  const visible = state.tab === cfg.tab;

  container.innerHTML = cfg.rows.map(cfg.itemBuilder).join('');

  if (visible && wasRendered) {
    let index = 0;
    for (const node of container.querySelectorAll('.item[data-key]')) {
      if (!prevKeys || !prevKeys.has(node.dataset.key)) {
        playEnter(node, index);
        index += 1;
      }
    }
  }
  if (visible) {
    playLayoutShift(container, prevTops);
  }
  state[cfg.snapshotProp] = new Set(cfg.rows.map(cfg.keyOf));
  state[cfg.onceProp] = true;
}

function renderPendingAnimated() {
  const rows = state.pending;
  el('pending-count').textContent = rows.length ? String(rows.length) : '';
  el('pending-meta').textContent =
    rows.length
      ? `临期（48 小时内）与已逾期优先 · 按截止时间由近到远 · 左滑删除 · ${rows.length} 条`
      : '';
  renderAnimatedList({
    containerId: 'pending-list',
    rows,
    tab: 'pending',
    dirtyProp: 'pending',
    snapshotProp: 'pendingSnapshot',
    onceProp: 'pendingRenderedOnce',
    keyOf: pendingKey,
    itemBuilder: pendingItemHtml,
    emptyHtml: '<p class="empty">没有未提交的作业。<br>数据来自 PC 端最近一次采集。</p>',
  });
}

function renderDeletedAnimated() {
  const rows = state.deleted;
  el('deleted-count').textContent = rows.length ? String(rows.length) : '';
  el('deleted-meta').textContent = rows.length
    ? `软删除 · 点「恢复」放回列表 · ${rows.length} 条`
    : '软删除 · 点「恢复」放回列表';
  renderAnimatedList({
    containerId: 'deleted-list',
    rows,
    tab: 'deleted',
    dirtyProp: 'deleted',
    snapshotProp: 'deletedSnapshot',
    onceProp: 'deletedRenderedOnce',
    keyOf: pendingKey,
    itemBuilder: deletedItemHtml,
    emptyHtml: '<p class="empty">回收站是空的。<br>在「未提交」里删除的作业会出现在这里。</p>',
  });
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
        ${kindTag(row)}
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
            <span class="item-side">
              ${group.rows[0].sourceLabel
                ? `<span class="chip chip-source">${escapeHtml(group.rows[0].sourceLabel)}</span>` : ''}
              <span class="chev"></span>
            </span>
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
  const [pending, submitted, deleted] = await Promise.all([
    api('/api/assignments?scope=pending'),
    api('/api/assignments?scope=submitted'),
    api('/api/assignments?scope=deleted'),
  ]);
  state.pending = pending;
  state.submitted = submitted;
  state.deleted = deleted;
  state.listsLoaded = true;   // 之后即使断线也保留最后一次的数据
  renderPendingAnimated();
  renderSubmitted();
  renderDeletedAnimated();
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
  el('deleted-list').innerHTML = hint;
  el('pending-meta').textContent = '';
  el('submitted-meta').textContent = '';
  el('deleted-meta').textContent = '';
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
  if (!['pending', 'submitted', 'deleted', 'settings'].includes(tab)) {
    tab = 'pending';
  }
  const changed = tab !== state.tab;
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
  el('view-deleted').hidden = tab !== 'deleted';
  el('view-settings').hidden = tab !== 'settings';
  window.scrollTo({ top: 0 });
  // 页面转场：轻微淡入 + 上移（首屏不播；系统开启"减少动态效果"时跳过）
  if (changed && state.booted && !reduceMotion()) {
    el(`view-${tab}`).animate([
      { opacity: 0, transform: 'translateY(6px)' },
      { opacity: 1, transform: 'none' },
    ], { duration: MOTION.page, easing: MOTION.easeOut });
  }
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

/// 展开/收起一张卡片（未提交 / 已删除共用；各自的展开集合通过参数区分）
function toggleCard(button, openSet) {
  const item = button.closest('.item');
  const key = item.dataset.key;
  const open = !item.classList.contains('open');
  item.classList.toggle('open', open);
  button.setAttribute('aria-expanded', String(open));
  if (open) {
    openSet.add(key);
  } else {
    openSet.delete(key);
  }
}

el('pending-list').addEventListener('click', (event) => {
  if (suppressClickNow()) {
    event.preventDefault();
    event.stopPropagation();
    return;
  }
  const button = event.target.closest('.item-toggle');
  if (button) {
    toggleCard(button, state.openPending);
  }
});

el('deleted-list').addEventListener('click', (event) => {
  const restore = event.target.closest('[data-undo-key]');
  if (restore) {
    restoreDeletedItem(restore.closest('.item'), restore.dataset.undoKey);
    return;
  }
  const button = event.target.closest('.item-toggle');
  if (button) {
    toggleCard(button, state.openDeleted);
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
  if (suppressClickNow()) {
    // 刚结束一次左滑拖动：这次 click 是拖动的尾巴，不是点击意图
    event.preventDefault();
    event.stopPropagation();
    return;
  }
  const jump = event.target.closest('[data-detail-url]');
  if (!jump) {
    return;
  }
  event.preventDefault();
  event.stopPropagation();
  openExternal(jump.dataset.detailUrl);
}, true);

// ---------------------------------------------------------------------------
// 删除 / 恢复（软删除）
// ---------------------------------------------------------------------------

/**
 * 删除/恢复的公共请求封装。
 *
 * 注意：这两个接口**不要求访问令牌**（删除是浏览列表的一部分动作）；
 * 如果本机配了令牌仍然带上（对服务端无影响，便于以后加回校验）。
 * 不加全局 busy 锁——需要支持连续多张卡片操作。
 */
async function authPost(path, body) {
  try {
    return { ok: true, data: await api(path, { method: 'POST', body, auth: Boolean(state.token) }) };
  } catch (error) {
    if (error.status === 401) {
      toast('访问令牌不对：请在「设置 → 访问令牌」里粘贴 PC 端托盘菜单复制的那串', true);
    } else {
      toast(error.message, true);
    }
    return { ok: false, error };
  }
}

/// 滑动刚结束的一小段时间里吞掉 click（拖动松手不该触发展开/跳转）
let suppressClickUntil = 0;
function suppressClickNow() {
  return performance.now() < suppressClickUntil;
}

/// 没有在途动画/拖动时，把被挂起的列表渲染补上
function maybeFlushRenders() {
  if (state.uiOps > 0 || state.swipe) {
    return;
  }
  if (state.renderDirty.pending) {
    state.renderDirty.pending = false;
    renderPendingAnimated();
  }
  if (state.renderDirty.deleted) {
    state.renderDirty.deleted = false;
    renderDeletedAnimated();
  }
}

/// 结束一个在途界面操作：全部结束后，把被挂起的列表渲染补上
function finishUiOp() {
  state.uiOps = Math.max(0, state.uiOps - 1);
  maybeFlushRenders();
}

/// 本地"删除于"时间（iPad 本地时区即北京时间；服务端刷新后会用权威值替换）
function localTimeDisplay() {
  const d = new Date();
  const pad = (n) => String(n).padStart(2, '0');
  return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())} `
    + `${pad(d.getHours())}:${pad(d.getMinutes())}`;
}

/**
 * 删除一条未提交作业（软删除，左滑触发）。
 *
 * 动画与请求并行，且严格两段式（用户指定）：
 *   ① 卡片若处于展开态，先"流体收拢"回原胶囊形态（高度收拢、展开体淡出、箭头还原）；
 *   ② 拉伸飞出 + 渐隐（横向拉伸、圆角收到胶囊、容器塌缩让邻居连续补位）。
 * 请求确认成功后才提交数据状态；失败则反向恢复并提示原因——
 * 不会出现"前端显示删掉了、服务端其实还在"。
 */
async function deletePendingItem(card, key, options = {}) {
  if (!card || card.dataset.busy === '1') {
    return;   // 重复触发：忽略（防重复删除）
  }
  card.dataset.busy = '1';
  const wrap = card.closest('.swipe');
  const row = state.pending.find((item) => pendingKey(item) === key);
  if (!row) {
    if (wrap) { wrap.remove(); } else { card.remove(); }
    return;   // 数据已被后台轮询换掉：直接清理，不再发请求
  }
  const startOffset = Math.max(0, options.startOffset || 0);

  state.uiOps += 1;
  const request = authPost('/api/assignments/delete', {
    courseId: row.courseId,
    sourceId: row.sourceId,
  });

  // ① 展开态 → 胶囊形态（流体收拢）
  if (card.classList.contains('open')) {
    await morphToCapsule(card);
  }
  // ② 拉伸飞出 + 渐隐，同时容器塌缩
  const geo = await flyOut(card, wrap, startOffset);

  const result = await request;
  if (!result.ok) {
    restoreAfterFly(card, wrap, geo);
    card.dataset.busy = '0';
    finishUiOp();
    return;   // 失败原因已由 authPost 提示
  }

  finishUiOp();
  // 服务端已确认：移除节点、更新本地状态（下一轮轮询以服务端为准）
  if (wrap) { wrap.remove(); } else { card.remove(); }
  state.pending = state.pending.filter((item) => pendingKey(item) !== key);
  state.openPending.delete(key);   // 展开集合也清掉，避免残留
  if (state.pendingSnapshot) {
    state.pendingSnapshot.delete(key);
  }
  const copy = Object.assign({}, row, { deletedAt: localTimeDisplay() });
  state.deleted = [copy, ...state.deleted.filter((item) => pendingKey(item) !== key)];
  renderPendingAnimated();
  renderDeletedAnimated();
  toast(`已删除「${row.title}」`, false, {
    label: '撤销',
    seconds: 6,
    onClick: () => undoDelete(copy),
  });
}

/**
 * ① 流体收拢：展开卡片 → 原胶囊形态。
 * 高度用 WAAPI 从当前尺寸收到"只有标题行"的尺寸（卡片自身 overflow:hidden，
 * 内容被连续裁掉），展开体同步淡出，open 类移除让箭头与强调描边一起还原。
 */
function morphToCapsule(card) {
  card.classList.remove('open');
  if (reduceMotion()) {
    return Promise.resolve();
  }
  const head = card.querySelector('.item-head');
  const target = head ? head.offsetHeight + 2 : card.offsetHeight;   // + 上下 1px 描边
  const from = card.offsetHeight;
  if (from <= target + 2) {
    return Promise.resolve();   // 本来就在胶囊尺寸附近
  }
  const animations = [
    card.animate([{ height: `${from}px` }, { height: `${target}px` }],
                 { duration: 220, easing: MOTION.easeInOut, fill: 'forwards' }),
  ];
  const collapse = card.querySelector('.collapse');
  if (collapse) {
    animations.push(collapse.animate([{ opacity: 1 }, { opacity: 0 }],
                                     { duration: 140, easing: 'ease-out', fill: 'forwards' }));
  }
  card.__morphAnimations = animations;
  return Promise.all(animations.map((anim) => anim.finished.catch(() => {})));
}

/**
 * ② 拉伸飞出：卡片向左划出（横向略微拉伸 + 圆角收到胶囊 + 后半段渐隐），
 * 容器（.swipe）同步塌缩高度与间距，下面的卡片连续补位。
 * @return {Promise<{height:number, margin:number}|null>} 容器原尺寸（失败恢复用）
 */
function flyOut(card, wrap, startOffset) {
  card.classList.add('leaving');
  if (reduceMotion()) {
    return Promise.resolve(null);
  }
  const width = card.getBoundingClientRect().width;
  const radius = getComputedStyle(card).borderRadius || '12px';
  const geo = wrap
    ? { height: wrap.offsetHeight, margin: parseFloat(getComputedStyle(wrap).marginBottom) || 0 }
    : null;

  const fly = card.animate([
    { transform: `translateX(${-startOffset}px)`, opacity: 1, borderRadius: radius },
    { transform: `translateX(${-(width + 160)}px) scaleX(1.12) scaleY(0.96)`,
      opacity: 0, borderRadius: '999px' },
  ], { duration: 340, easing: 'cubic-bezier(0.45, 0, 0.85, 0.5)', fill: 'forwards' });
  card.__exitAnimation = fly;

  const jobs = [fly.finished.catch(() => {})];
  if (wrap && geo) {
    const collapse = wrap.animate([
      { height: `${geo.height}px`, marginBottom: `${geo.margin}px` },
      { height: '0px', marginBottom: '0px' },
    ], { duration: 300, easing: MOTION.easeOut, fill: 'forwards' });
    wrap.__collapseAnimation = collapse;
    jobs.push(collapse.finished.catch(() => {}));
  }
  return Promise.all(jobs).then(() => geo);
}

/// 删除失败：取消飞出/塌缩/收拢，卡片带着一点回弹回到原位
function restoreAfterFly(card, wrap, geo) {
  if (card.__exitAnimation) {
    card.__exitAnimation.cancel();
    card.__exitAnimation = null;
  }
  if (wrap && wrap.__collapseAnimation) {
    wrap.__collapseAnimation.cancel();
    wrap.__collapseAnimation = null;
  }
  if (card.__morphAnimations) {
    for (const anim of card.__morphAnimations) {
      anim.cancel();
    }
    card.__morphAnimations = null;
    if (state.openPending.has(card.dataset.key)) {
      card.classList.add('open');   // 状态没提交过：展开态要还原
    }
  }
  card.classList.remove('leaving');
  const backing = wrap ? wrap.querySelector('.swipe-backing') : null;
  if (backing) {
    backing.style.opacity = '0';
    backing.classList.remove('armed');
  }
  if (reduceMotion()) {
    card.style.transform = '';
    return;
  }
  if (wrap && geo) {
    wrap.animate([
      { height: '0px', marginBottom: '0px' },
      { height: `${geo.height}px`, marginBottom: `${geo.margin}px` },
    ], { duration: 260, easing: MOTION.easeOut });
  }
  card.animate([
    { opacity: 0.25, transform: 'translateX(-56px)' },
    { opacity: 1, transform: 'translateX(0)' },
  ], { duration: 260, easing: MOTION.easeOut }).finished.catch(() => {}).then(() => {
    card.style.transform = '';
  });
}

// ---------------------------------------------------------------------------
// 左滑删除手势（Pointer Events）
//
// 目标手感（用户指定）：跟手带阻尼、松手不过线就回弹、**滑过触发线才**触发删除。
// 刻意不做"快速甩动也能删"的捷径：手指轻轻一甩不该把东西删掉——
// 触发条件只有"真实滑动距离 ≥ 触发线"一条，过线时红色动作区放大作为"已到位"确认。
// 用 Pointer Events 而不是 Touch Events：同一套代码同时覆盖触屏与鼠标（PC 预览）。
// `.swipe { touch-action: pan-y }` 把竖向滚动完全留给浏览器（一旦判定竖向就
// 安全退出），横向手势才会作为 pointermove 流交给我们，不需要 preventDefault。
// ---------------------------------------------------------------------------

const SWIPE = {
  engage: 15,            // 横向位移超过它才判定"在左滑"（轻点/竖向滚动不受影响）
  maxOverdrag: 220,      // 越过触发线之后还能再拉出的最大距离（阻尼段）
};

/// 触发删除所需的滑动距离：卡片宽度的 40%，夹在 [96, 240] px
function swipeTriggerDistance(card) {
  const width = card.getBoundingClientRect().width;
  return Math.min(240, Math.max(96, width * 0.4));
}

/// 阻尼曲线：触发线以内 1:1 跟手；越线后指数逼近 +maxOverdrag（越拉越重）
function dampedOffset(raw, trigger) {
  if (raw <= trigger) {
    return raw;
  }
  return trigger + SWIPE.maxOverdrag * (1 - Math.exp(-(raw - trigger) / SWIPE.maxOverdrag));
}

function swipeResetVisual(swipe) {
  swipe.card.style.transform = '';
  if (swipe.backing) {
    swipe.backing.style.opacity = '0';
    swipe.backing.classList.remove('armed');
  }
}

function onPendingPointerDown(event) {
  if (state.swipe || state.uiOps > 0) {
    return;   // 已有滑动/删除动画在途：忽略，避免互相打断
  }
  if (event.pointerType === 'mouse' && event.button !== 0) {
    return;
  }
  const card = event.target.closest('.item[data-key]');
  if (!card || card.dataset.busy === '1' || card.classList.contains('leaving')) {
    return;
  }
  const wrap = card.closest('.swipe');
  if (!wrap) {
    return;   // 只有未提交列表带滑动容器
  }
  state.swipe = {
    card,
    wrap,
    backing: wrap.querySelector('.swipe-backing'),
    pointerId: event.pointerId,
    startX: event.clientX,
    startY: event.clientY,
    raw: 0,
    offset: 0,
    dragging: false,
  };
  try {
    card.setPointerCapture(event.pointerId);   // 手指滑出列表也能收到后续事件
  } catch (error) {
    // 指针已失效等边缘情况：忽略，靠 pointerup / pointercancel 兜底
  }
}

function onPendingPointerMove(event) {
  const swipe = state.swipe;
  if (!swipe || event.pointerId !== swipe.pointerId) {
    return;
  }
  const dx = event.clientX - swipe.startX;
  const dy = event.clientY - swipe.startY;

  if (!swipe.dragging) {
    if (Math.abs(dy) > SWIPE.engage && Math.abs(dy) >= Math.abs(dx)) {
      state.swipe = null;   // 竖向：让页面正常滚动
      return;
    }
    if (-dx < SWIPE.engage || Math.abs(dx) < Math.abs(dy) * 1.1) {
      return;   // 还没越过死区 / 往右拖（不处理）
    }
    swipe.dragging = true;
    swipe.wrap.classList.add('dragging');
  }

  swipe.raw = Math.max(0, swipe.startX - event.clientX);
  const trigger = swipeTriggerDistance(swipe.card);
  swipe.offset = dampedOffset(swipe.raw, trigger);
  swipe.card.style.transform = `translateX(${-swipe.offset.toFixed(1)}px)`;
  if (swipe.backing) {
    swipe.backing.style.opacity = String(Math.min(1, (swipe.offset / trigger) * 1.2));
    // 滑过触发线 = "已到位，松手就删"：红色动作区放大一点作为确认信号
    swipe.backing.classList.toggle('armed', swipe.raw >= trigger);
  }
}

function onPendingPointerUp(event) {
  const swipe = state.swipe;
  if (!swipe || event.pointerId !== swipe.pointerId) {
    return;
  }
  state.swipe = null;
  if (!swipe.dragging) {
    return;   // 普通点击：交给 click（展开/收起、跳转）
  }
  swipe.wrap.classList.remove('dragging');
  suppressClickUntil = performance.now() + 350;   // 拖动后的 click 一律吞掉

  // 触发条件只有一条：真实滑动距离达到触发线。
  // 刻意不做"快速甩动也能删"的捷径——手指轻轻一甩不该把东西删掉。
  if (swipe.raw >= swipeTriggerDistance(swipe.card)) {
    deletePendingItem(swipe.card, swipe.card.dataset.key, { startOffset: swipe.offset });
    return;
  }
  swipeSpringBack(swipe);
}

/// 没到位：松手回弹（缓动曲线带一点超调，就是那种"弹回来"的手感）
function swipeSpringBack(swipe) {
  if (reduceMotion()) {
    swipeResetVisual(swipe);
    maybeFlushRenders();
    return;
  }
  state.uiOps += 1;   // 回弹也是动画：期间挂起列表重建
  const animation = swipe.card.animate([
    { transform: `translateX(${-swipe.offset.toFixed(1)}px)` },
    { transform: 'translateX(0)' },
  ], { duration: 300, easing: 'cubic-bezier(0.18, 1.25, 0.4, 1)' });
  if (swipe.backing) {
    swipe.backing.animate(
      [{ opacity: swipe.backing.style.opacity || '0' }, { opacity: 0 }],
      { duration: 200, easing: 'ease-out' });
  }
  animation.finished.catch(() => {}).then(() => {
    swipeResetVisual(swipe);
    finishUiOp();
  });
}

/// 系统手势打断（开始滚动、来电等）：瞬时复位，不播动画
function onPendingPointerCancel(event) {
  const swipe = state.swipe;
  if (!swipe || event.pointerId !== swipe.pointerId) {
    return;
  }
  state.swipe = null;
  swipe.wrap.classList.remove('dragging');
  swipeResetVisual(swipe);
  maybeFlushRenders();
}

el('pending-list').addEventListener('pointerdown', onPendingPointerDown);
el('pending-list').addEventListener('pointermove', onPendingPointerMove);
el('pending-list').addEventListener('pointerup', onPendingPointerUp);
el('pending-list').addEventListener('pointercancel', onPendingPointerCancel);
// 捕获丢失（节点被重建、指针被系统收走）：按时序这一定发生在 pointerup 之后，
// 没结束的手势在这里兜底清理，避免残留状态挡住下一次滑动
el('pending-list').addEventListener('lostpointercapture', (event) => {
  const swipe = state.swipe;
  if (swipe && event.pointerId === swipe.pointerId) {
    state.swipe = null;
    swipe.wrap.classList.remove('dragging');
    swipeResetVisual(swipe);
    maybeFlushRenders();
  }
});

/// 恢复成功后的公共收尾：移出回收站、提示、拉一次权威列表（拿回服务端排序）
function applyRestoreSuccess(row) {
  const key = pendingKey(row);
  state.deleted = state.deleted.filter((item) => pendingKey(item) !== key);
  if (state.deletedSnapshot) {
    state.deletedSnapshot.delete(key);
  }
  renderDeletedAnimated();
  toast(`已恢复「${row.title}」`);
  state.lastListsAt = 0;
  loadLists(true).catch(() => {});
}

/// 撤销（恢复）一条刚删除的作业（提示条里的「撤销」）
async function undoDelete(row) {
  const result = await authPost('/api/assignments/restore', {
    courseId: row.courseId,
    sourceId: row.sourceId,
  });
  if (result.ok) {
    applyRestoreSuccess(row);
  }
}

/**
 * 恢复「已删除」里的一条（点行尾「恢复」）：先播退场动画，确认成功后才删标记；
 * 失败同样反向恢复。恢复后强制刷新一次未提交列表，拿回服务端的排序位置。
 */
async function restoreDeletedItem(card, key) {
  if (!card || card.dataset.busy === '1') {
    return;
  }
  card.dataset.busy = '1';
  const row = state.deleted.find((item) => pendingKey(item) === key);
  if (!row) {
    card.remove();
    return;
  }

  state.uiOps += 1;
  const request = authPost('/api/assignments/restore', {
    courseId: row.courseId,
    sourceId: row.sourceId,
  });
  const geo = await playExit(card);
  const result = await request;

  if (!result.ok) {
    card.dataset.busy = '0';
    playExitBack(card, geo);
    finishUiOp();
    return;
  }

  finishUiOp();
  card.remove();
  applyRestoreSuccess(row);
}

// 手动同步
el('btn-incremental').addEventListener('click', async () => {
  await writeAction('/api/sync', { mode: 'incremental' }, '已开始增量同步');
  tick(true);
});

el('btn-full').addEventListener('click', async () => {
  await writeAction('/api/sync', { mode: 'full' }, '已开始全量采集',
                    '全量采集会把所有课程完整扫一遍（南大十几门课约 10 秒；' +
                    '学习通随后逐门取作业列表，多花一两分钟），确定开始吗？');
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
state.booted = true;   // 首屏之后才给 tab 切换加转场动画

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
