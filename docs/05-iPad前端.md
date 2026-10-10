# iPad 前端（浏览 + 令牌保护的设置）

| 项目 | 值 |
| --- | --- |
| 访问地址 | `https://laptop-pli3g45s.tail981d8b.ts.net/`（仅 tailnet 可见） |
| 监听地址 | `127.0.0.1:8790`（只回环，不对局域网与公网开放） |
| 端口设置 | QSettings `web/port`（0 = 关闭）；环境变量 `NJULMS_WEB_PORT` 优先 |
| 后端实现 | `src/web/WebApi.*`（主线程执行器）、`src/web/WebServer.*`（HTTP 路由） |
| HTTP 库 | cpp-httplib v0.60.1（MIT），单头文件放在 `third_party/httplib.h` |
| 前端实现 | `resources/web/index.html`、`app.js`、`style.css`（编进 exe 的 Qt 资源） |
| 外部入口 | `tailscale serve --bg --https=443 http://127.0.0.1:8790`，TLS 由 Tailscale 终止 |
| 写操作保护 | 访问令牌（QSettings `web/token`，首次启动生成；托盘菜单「复制访问令牌」） |
| 页面结构 | 三个 tab：未提交（todo 卡片）/ 已提交（按课程分组）/ 设置 |

## 1. 结构

```
iPad Safari
   │  https://laptop-pli3g45s.tail981d8b.ts.net/      （Tailscale，仅 tailnet）
   ▼
Tailscale 守护进程（本机 443，终止 TLS）
   │  http://127.0.0.1:8790
   ▼
njulms_app.exe ── WebServer（cpp-httplib，只监听回环）
                     │  WebApi::run() → BlockingQueuedConnection
                     ▼
              主线程：SyncManager / Scheduler / AuthManager / SqliteRepository / WebEngine
```

为什么要多一层 `WebApi`：后端对象全部活在主线程，且都不是线程安全的。
HTTP 线程只做 JSON 序列化，任何数据访问都通过 `WebApi::run()` 投递到主线程执行。

## 2. 线程与并发（"同步时前端来读/改会不会出事"）

- 所有回调都在主线程执行，与界面操作、同步步骤**天然串行**：同一时刻只有一个访问者，
  不存在「同步写库 + 前端读库」的并发，读到的永远是提交之后的状态。
- 同步进行中若前端请求状态，只是排一次队（当前数据量下每次 < 1 ms）。
- `WebServerTests::callbacksAreSerializedOnMainThread` 是这条约束的**直接验证**：
  12 个并发 HTTP 请求，全部在主线程执行、没有任何一次重叠，总耗时 ≈ 12 × 25 ms
  （若并行只需 ~25 ms，所以耗时本身就是串行证据）。
- P1 的写操作会复用同一条通道与桌面端已有的动作函数（`beginSync`、`scheduler->start()`
  等），并沿用同一套可用性判断（同步运行中拒绝启动、周期夹取到 [15, 1440] 分钟），
  不做第二套状态机，因此不会出现「前端改了设置、后端还在按旧设置跑」的错位。

### 写操作落地后的三条护栏（2026-10-10 补充）

1. **驱动桌面端同一批控件**：`POST /api/settings` 实际调用的是
   `scheduleMinutes->setValue()` / `scheduleEnabled->setChecked()` 这些**桌面端 UI 自己的控件**，
   它们的信号槽里已有"写 QSettings + 应用到 Scheduler/SyncManager + 记日志"的完整逻辑。
2. **与桌面端一致的拒绝规则**：同步进行中改周期/定时开关 → 409（桌面端此时也是灰的）；
   重复触发同步 → 409（`SyncManager::start()` 自己就拒绝并发）；暂停/继续/取消状态不匹配 → 409。
   前端把按钮置灰并把原因显示出来，不自己猜状态（一切以后端 `capabilities` 为准）。
3. **不在 PC 上弹窗**：iPad 触发同步时**不走** `beginSync()`（那个入口失败会弹桌面对话框），
   而是做同样的前置判断，把原因用 409 返回给 iPad。

## 3. 接口

读接口（无需令牌），统一返回 `{"ok":true,"data":...}`：

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| GET | `/api/health` | 存活检查 |
| GET | `/api/status` | 会话状态、任务进度、最近一次运行、设置快照、`capabilities`（前端据此置灰按钮） |
| GET | `/api/courses` | 课程列表（含作业条数） |
| GET | `/api/assignments?scope=pending` | 跨课程未提交（含已逾期），带纯文本说明，按截止时间升序 |
| GET | `/api/assignments?scope=submitted` | 跨课程已提交，按发布时间降序（缺失者按其截止时间降序排在其后） |
| GET | `/api/courses/{id}/assignments` | 单课程（早期接口，保留） |
| GET | `/api/logs?tail=N` | 日志尾部（N 夹取到 1..2000，只读文件末 256 KB） |
| GET | `/`、`/app.js`、`/style.css` | 前端页面（编进 exe 的资源） |

写接口（一律需要 `X-Auth-Token`，缺失或错误返回 401）：

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| POST | `/api/settings` | `scheduleIntervalMinutes`、`scheduleEnabled`、`quickIncremental`、`startupFullScan`、`enrichDescriptions` |
| POST | `/api/sync` | `{"mode":"incremental"｜"full"}` |
| POST | `/api/sync/pause`、`/api/sync/resume`、`/api/sync/cancel` | 暂停 / 继续 / 取消 |
| POST | `/api/data/purge` | `{"confirm":"DELETE"}` 清空采集数据（保留登录状态）；同步进行中返回 409 |

失败响应统一为 `{"ok":false,"error":{"message":"..."}}`（400 参数错误 / 401 令牌 /
409 与当前状态冲突 / 500 异常）；未注册路径返回 404；处理函数抛异常返回 500。

## 4. 怎么用

1. PC：启动 `njulms_app.exe`。启动后状态栏与日志会给出前端地址，托盘菜单里有
   「复制 iPad 访问地址」「在 PC 浏览器里预览前端」。
2. iPad：确认 Tailscale 已连接同一 tailnet，用 Safari 打开
   `https://laptop-pli3g45s.tail981d8b.ts.net/`；可以「添加到主屏幕」当作应用打开。
3. 首次要改设置或触发同步：在 PC 托盘菜单点「复制访问令牌」→ iPad 的
   「设置 → 访问令牌」粘贴保存（只读浏览不需要令牌）。
4. **关窗口不退出**：程序缩到托盘继续服务（定时同步与前端都继续工作）；
   要真正退出用托盘菜单的「退出程序」。托盘不可用的系统会退回旧行为（关窗即退出）。

## 5. 边界与已知限制

- **删除数据也在前端**，但有两道确认：设置页的红色「删除全部数据」按钮会弹出警告窗，
  列出会删/不会删的内容，必须勾选"我明白"才能点确认；接口再要求 `confirm == "DELETE"`。
  同步进行中直接 409（与 PC 上"先等它结束"的规则一致）。
- **只读接口不需要令牌**：tailnet 内其它设备（NAS、备用手机）仍可以**看**状态与作业；
  改设置与触发同步必须有令牌。
- **登录/滑块仍在 PC 上完成**：iPad 端只能看到"需要登录"状态。
  后端会话保活情况见第 6 节。
- **PC 必须开着**：程序退出或机器休眠时前端不可达（定时同步同理）。
- **已提交没有"提交时间"**：站点不提供该字段，所以按发布时间排序，界面写明口径。
- **作业说明可能缺失**：列表接口经常不带 `data.description`（实测微积分I 的 4 条全无），
  已加"补全作业说明"（缺正文时额外请求一次活动接口，只补正文不改记录，设置页可关）。
  附件（文件/图片）采集不到。
- 日志接口已就绪但前端还没做日志页面。
- 令牌比较是普通字符串比较（非常量时间）；在"回环 + tailnet"的威胁模型下可接受。

## 6. 会话保活现状（回答"后端有没有按周期登录"）

结论：**没有"按周期自动登录"这种功能，也不可能全自动**（站点有滑块/扫码，代码里明确不绕过）。
现有的周期性行为是：

| 机制 | 触发 | 说明 |
| --- | --- | --- |
| 启动检查 | 程序启动、页面加载完成后 | `AuthManager::check(false)`，只校验会话 |
| 手动检查 | 点「检查登录状态」 | `AuthManager::check(true)` |
| 定时同步 | 每 N 分钟（默认 60，范围 15~1440） | 跑一次**真实采集**，每个请求都带现有 Cookie |

也就是说：开了定时同步，就等于每 N 分钟访问一次站点，客观上起到保活作用，
但代价是每次 N+1 个请求（一次全量十几门课约 13+ 个请求），比专门的心跳重。
若需要"正式"的保活，可加一个轻量心跳（每 20~30 分钟 1 个请求，例如复用 `/api/todos`
或用户信息接口），好处是：定时同步关闭时也能保活、能更早发现会话失效并提醒。
这条尚未实现，待确认是否需要。

## 7. 页面交互（2026-10-10 新增）

| Tab | 内容 | 交互 |
| --- | --- | --- |
| **未提交** | 跨课程所有未提交作业（含已逾期），按截止时间由近到远 | 卡片显示标题 / 截止 / 剩余时间 / 课程；点卡片就地展开，显示**作业内容与要求**（站点说明的纯文本）与来源信息；右端 **↗** 跳原站详情 |
| **已提交** | 按课程分组，课程卡片竖排 | 点课程卡片展开，组内按**发布时间由新到旧**列已提交作业（截止 / 发布 / 得分 / 来源），每条右端 **↗** 跳原站详情 |
| **设置** | 当前任务 + 同步设置 + 显示 + 访问令牌 | 见上表与第 3 节 |
| **设置 · 危险操作** | 删除全部数据（保留登录状态） | 点击弹警告窗 → 勾选确认 → 红色按钮才可点；成功后提示删除行数并刷新列表 |

其它细节：

- **采集完成后页面自动刷新**：轮询 `/api/status` 时识别"上一帧还在跑 / 这次换了 runId"，
  一旦某次采集结束（PC 上手动点的、定时任务触发的、iPad 上自己发起的都算），
  立刻强制重拉两个作业列表并弹一条提示（"采集完成，数据已刷新"；失败/取消会给对应的措辞）。
- **刷新按钮有反馈**：点击后按钮变成「⟳ 刷新中」（带转圈动画）并禁用，完成后短暂显示
  「已刷新」（绿色），失败显示「刷新失败」（红色），约 1 秒后恢复。
- **前端更新后怎么生效**：静态资源的 URL 带内容哈希（`/app.js?v=43d95bff`），
  前端一改哈希就变，浏览器必然重新拉取。但"添加到主屏幕"的网页 app 是常驻页面，
  不会自己重新加载 —— 需要**从后台任务里划掉再打开**（Safari 里则下拉刷新）。
  设置页「关于」里有一行「资源版本 xxxxxxxx」：**看得到 = 已是最新前端**。
- **禁用缩放**（防误触）：`touch-action: manipulation` 关双击缩放，`gesturestart` /
  多指 `touchmove` 拦截捏合（iOS Safari 从 iOS 10 起会忽略 `user-scalable=no`）。
  补偿手段是设置页的**字号四档**（小 / 标准 / 大 / 特大），存在本机浏览器里。
- 输入框字号 ≥ 16px，否则 iOS 聚焦时会自动放大页面，与"禁用缩放"的诉求冲突。
- 触摸目标 ≥ 44pt、跟随系统深浅色、`env(safe-area-inset-*)` 适配刘海与底部横条。
- 页面切到后台停止轮询（省电），回前台立刻刷新一次。
- 支持 `?tab=settings` 直链，可把"设置"页单独加进主屏幕。

### 原站详情页（↗ 按钮）

- 数据来源：`detailUrl`，真实格式为
  `https://lms.nju.edu.cn/course/{courseId}/learning-activity#/{activityId}`
  （**2026-10-10 实测确认**：在已登录站点里点开一条作业，SPA 跳到的就是这个地址；
  早期推导的 `/course/{id}/activity/{id}` 是错的，会显示"找不到资源"。
  注意它是 hash 路由，活动 id 在 `#` 之后）。
  接口下发前**校验 https + *.nju.edu.cn 域名**，
  不合规就不下发，前端自然不显示按钮。
- 打开方式：`window.open(url, '_blank')` → 交给系统打开 **Safari**。
  - 为什么不用 `location.href`：这是"添加到主屏幕"的独立网页 app，只能留在自己的域名里；
    用 `location.href` 跳外域会把 app 窗口本身导航走（回不来，只能杀掉重开）。
    `window.open` 只是让 app 进后台，切回来还在原处。
  - 由系统交给 Safari 是**唯一可行**的方式：原站响应头带 `x-frame-options: SAMEORIGIN`，
    不能 iframe 嵌入；iOS 也没有"应用内浏览器"API。
- 首次使用要**在 iPad 上登录一次**南大统一身份认证（过滑块），之后 Safari 自己记住会话
  （PC 的 WebEngine 会话和 iPad Safari 的会话是两套，互不影响）。
- 点 ↗ 不会触发卡片的展开/收起（用捕获阶段处理并 `stopPropagation`），
  实测：`expandedByJump=false`、`toggledOpen=true`。
- **未提交页签平时可能是空的**（作业都交了）。想在真机上看这条 UI，可以放一条演示数据：

  ```powershell
  python tools\demo_pending.py            # 插入（幂等）
  python tools\demo_pending.py --remove   # 删除
  ```

  它往本地库插一条标题带「【测试】」的未提交作业，并借用一条真实作业的详情链接，
  这样点 ↗ 打开的是真页面。注意它是**本地数据、站点上不存在**，
  所以下一次完整全量扫描会把它清掉（想留着就别勾"启动时自动全量扫描"，或再跑一次脚本）。

## 8. 验证记录（2026-10-10）

- `build.ps1 -Config Release -WithVerifyTool`：构建通过；**13 套件 / 207 用例全部通过**
  （`WebServerTests` 6 例：路由、scope 校验、令牌 401、后端裁决 409、并发串行化；
  GUI 冒烟测试用 `NJULMS_WEB_PORT=0` 避免占用端口）。
  ⚠️ 踩坑记录：跑过一次 `-NoTests` 之后，CMake 缓存里的 `NJULMS_BUILD_TESTS` 会**留在 OFF**，
  下次不带该开关构建时不会重新编译测试目标（于是在跑旧二进制、看不到新增用例）。
  恢复方式：`cmake -S . -B build -DNJULMS_BUILD_TESTS=ON` 再构建。
- 真机端到端：本机 `http://127.0.0.1:8790/api/*` 与
  `https://laptop-pli3g45s.tail981d8b.ts.net/api/*` 均返回 200，页面可加载；
  发送 `WM_CLOSE` 后**进程存活、主窗口隐藏、接口继续可用**（托盘常驻生效）。
- 端到端（用一份带"未提交作业"的临时数据库 + 独立数据目录）：
  `scope=pending` → 2 条且带 `descriptionText`（HTML 已转纯文本）；`scope=submitted` → 5 条；
  无令牌 POST `/api/sync` → **401**；带令牌把周期 45 改成 60 → 200 并立即生效（随后改回 45）；
  无任务时 POST `/api/sync/pause` → **409**。
- 删除数据端到端：同一份临时库（13 门课 / 2 条未提交）→ `POST /api/data/purge {confirm:"DELETE"}`
  返回 `removed=191`，随后 `pending=0`、`courses=0`；不带令牌调用 → **401**。
  警告窗截图：`docs/images/frontend-pad-purge-warning-dark.png`。
- 页面渲染（无头 Edge + CDP，iPad 竖屏 834×1112 视口，真实数据）：
  `docs/images/frontend-pad-pending-dark.png`（未提交 + 展开的作业内容）、
  `frontend-pad-submitted-dark.png`（已提交按课程分组 + 展开）、
  `frontend-pad-settings-dark.png`（设置页：任务、同步设置、字号、令牌）、
  `frontend-pad-offline-dark.png`（后端不可用：徽标变「连接中断」、说明给出错误原因）。
- 前端脚本通过 Node 语法检查（`node --check app.js`）。
- iPad 当前离线（Tailscale 里最后在线 6 天前），因此未在 iPad 上实测渲染；
  链路本身（Tailscale HTTPS → 回环服务）已验证可用。

### 顺带修掉的测试缺陷（与本功能同批）

`MainWindowSmokeTests::missingDataDirectoryIsHandled` 原来拿 `Z:\definitely\…`
当"写不进去的路径"，而那是一台 NAS 的网络盘：

- 盘离线时：建库失败 → 程序弹模态错误框 → 离屏测试没人点确定，**永久卡住**
  （2026-10-09 全量测试就是这样挂住的）；
- 盘在线时：真的在 SMB 共享上建 SQLite（含 WAL），单条用例耗时 20 s 以上
  （实测 21.5 s），并且会在 NAS 上留下 `Z:\definitely\not\a\real\path\for\njulms\njulms.db`。

现在改成「父级是一个普通文件」的本地路径（必然失败、毫秒级），并用定时器自动关闭
程序弹出的模态错误框，验证的是"构造过程不崩溃且给出提示"。
GUI 冒烟套件因此从 13.8 s 降到 **1.25 s**，也不再依赖网络盘。
