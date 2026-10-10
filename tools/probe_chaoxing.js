// 进课程 → 抓作业页与考试页的真实地址与请求。
// usage: node probe_chaoxing4.js [debugPort]
const port = Number(process.argv[2] || 9222);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function main() {
  const list = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
  const target = list.find((t) => t.type === 'page' && /chaoxing/.test(t.url || ''));
  if (!target) throw new Error('no chaoxing page target');
  const ws = new WebSocket(target.webSocketDebuggerUrl);
  await new Promise((resolve, reject) => { ws.onopen = resolve; ws.onerror = reject; });
  let nextId = 1;
  const waiting = new Map();
  let requests = [];
  ws.onmessage = (e) => {
    const msg = JSON.parse(e.data);
    if (msg.id && waiting.has(msg.id)) { waiting.get(msg.id)(msg.result || {}); waiting.delete(msg.id); return; }
    if (msg.method === 'Network.requestWillBeSent') {
      const r = msg.params.request;
      if (!/\.(js|css|png|jpg|jpeg|gif|svg|woff2?|ico|map|ttf)(\?|$)/i.test(r.url)) {
        requests.push(`${r.method} ${r.url}${r.postData ? '  body=' + r.postData.slice(0, 200) : ''}`);
      }
    }
  };
  const send = (method, params = {}) => new Promise((resolve) => {
    const id = nextId++;
    waiting.set(id, resolve);
    ws.send(JSON.stringify({ id, method, params }));
  });
  const evaluate = async (expression) => {
    const res = await send('Runtime.evaluate', { expression, returnByValue: true, awaitPromise: true });
    if (res.exceptionDetails) {
      const d = res.exceptionDetails.exception ? res.exceptionDetails.exception.description : res.exceptionDetails.text;
      return { error: d };
    }
    return res.result ? res.result.value : undefined;
  };
  const dump = async (label, extra = '') => {
    const info = await evaluate(`({
      url: location.href, title: document.title,
      text: document.body ? document.body.innerText.replace(/\\s+/g, ' ').slice(0, 400) : '',
      nav: [...document.querySelectorAll('a,li,span,div')]
        .filter((n) => n.children.length === 0 && /^(作业|考试|任务点)/.test((n.innerText || '').trim()))
        .slice(0, 10).map((n) => (n.innerText || '').trim().slice(0, 16)),
    })`);
    console.log(`\n=== ${label} ===`);
    console.log(JSON.stringify(info, null, 1));
    if (extra) console.log(extra);
    console.log('--- 请求 ---');
    console.log([...new Set(requests)].join('\n'));
  };

  await send('Page.enable');
  await send('Network.enable');

  // 1) 课程中心
  await send('Page.navigate', { url: 'https://mooc1-1.chaoxing.com/visit/interaction?s=bb4baf70c992d60848c26f3edc4f84e2' });
  await sleep(6000);
  const courseEntry = await evaluate(`(() => {
    const a = [...document.querySelectorAll('a[href*="stucoursemiddle"]')][0];
    return a ? { href: a.href, name: (a.innerText || '').trim().slice(0, 30) } : null;
  })()`);
  console.log('第一门课:', JSON.stringify(courseEntry));
  if (!courseEntry) { ws.close(); return; }

  // 2) 进课程
  requests = [];
  await send('Page.navigate', { url: courseEntry.href });
  await sleep(9000);
  await dump('课程页');

  // 3) 点"作业"
  requests = [];
  const clickedWork = await evaluate(`(() => {
    const nodes = [...document.querySelectorAll('a,li,span,div')]
      .filter((n) => n.children.length === 0 && (n.innerText || '').trim() === '作业');
    if (!nodes.length) { return { found: false }; }
    const el = nodes[0];
    const info = { found: true, tag: el.tagName, href: el.closest('a') ? el.closest('a').href : (el.getAttribute('href') || '') };
    (el.closest('a') || el).click();
    return info;
  })()`);
  console.log('\n点击"作业":', JSON.stringify(clickedWork));
  await sleep(9000);
  await dump('作业页');

  // 4) 点"考试"
  requests = [];
  const clickedExam = await evaluate(`(() => {
    const nodes = [...document.querySelectorAll('a,li,span,div')]
      .filter((n) => n.children.length === 0 && (n.innerText || '').trim() === '考试');
    if (!nodes.length) { return { found: false }; }
    const el = nodes[0];
    const info = { found: true, tag: el.tagName, href: el.closest('a') ? el.closest('a').href : (el.getAttribute('href') || '') };
    (el.closest('a') || el).click();
    return info;
  })()`);
  console.log('\n点击"考试":', JSON.stringify(clickedExam));
  await sleep(9000);
  await dump('考试页');

  ws.close();
}

main().catch((e) => { console.error('failed:', e.message); process.exitCode = 1; });
