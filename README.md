# NjuLmsCrawler v2 · 智汇南雍作业采集与管理系统

运行在 Windows 上的桌面程序：用 Qt WebEngine 打开南京大学「智汇南雍」（TronClass），
由用户**手动**完成统一身份认证，然后在页面上下文里调用站点自身的 JSON 接口，
把本人有权访问的课程与作业采集到本地 SQLite，支持增量同步、断点续传与定时同步。

- C++17 / CMake / Qt 6（Widgets + WebEngine + WebChannel）/ SQLite（Qt SQL）
- 网站专属逻辑（接口路径、字段名）集中在 `NjuLmsAdapter` 与 `AssignmentParser`，
  与数据库、调度、同步机制解耦
- 核心库 `njulms_core` 只依赖 Qt Core + Sql：没有 WebEngine 也能构建并跑完全部
  核心单元测试

> v2 与上一版（v1）的关系、本次修复的真实缺陷与新增能力，见
> [`docs/03-v2变更记录.md`](docs/03-v2变更记录.md)；可行性结论与实测证据见
> [`docs/00-可行性评估.md`](docs/00-可行性评估.md)。

---

## 一、当前状态

| 阶段 | 内容 | 状态 |
| --- | --- | --- |
| 1 | 项目骨架、主窗口、打开站点、手动登录、登录状态 | ✅ 已实现并编译运行 |
| 2 | 真实接口契约验证、解析器 | ✅ **已用有效会话真机复测通过**（13 门课程、7 条作业，见 `docs/02-阶段2验证结果.md` 第九节） |
| 3 | SQLite 与 Repository（唯一约束、UPSERT、事务） | ✅ 已实现并通过测试 |
| 4 | 全量采集与断点续传、失败恢复、运行记录 | ✅ 已实现并通过测试 |
| 5 | 增量同步（去重、更新变化字段） | ✅ 已实现并通过测试 |
| 6 | 定时同步、暂停/继续/取消、进度与日志 | ✅ 已实现并通过测试 |

**测试：201 个用例全部通过（12 个测试套件，`ctest`）。**

**真机验证已全部完成**（2026-10-09 晚）：登录、会话、接口、`page_size` 上限（服务端
夹取到 100）、13 门课程字段契约、应用层全量 + 两次增量同步（第二轮起完全幂等，
无重复记录）。详见 `docs/02-阶段2验证结果.md` 第九节。

> 提醒：统一身份认证在密码登录时强制滑块验证（学校策略），程序不做任何绕过；
> 会话较长一段时间未使用后需要重新人工登录一次。

```powershell
cd "D:\Project\HNS_Homework Notification_System_v2"

# 1) 构建（含验证工具）并跑测试（日常使用 Release）
.\build.ps1 -QtPrefix 'D:\Qt\6.8.3\msvc2022_64' -Config Release -WithVerifyTool

# 2) 启动程序，在界面里点「打开智汇南雍并登录」，人工完成认证
.\build\bin\njulms_app.exe

# 3) 一条命令复测站点契约（自动复制并清理临时 Profile）
.\verify.ps1
```

## 二、构建

### 依赖

| 组件 | 版本 / 位置 |
| --- | --- |
| Visual Studio | 2022 或 2026（需 C++ 桌面开发工作负载），本机 `D:\app\vs` |
| Qt | 6.8.3，**MSVC 2022 64-bit**，含 WebEngine / WebChannel / Positioning，本机 `D:\Qt\6.8.3\msvc2022_64` |
| CMake | ≥ 3.21（本机 4.3.2） |
| Ninja | 可选（推荐，构建更快） |

`qtpositioning` 必须安装：`Qt6WebEngineCore` 的 CMake 依赖里有它。

### 安装 Qt（无需 Qt 账户）

```powershell
python -m pip install aqtinstall
python -m aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 `
    --modules qtwebengine qtwebchannel qtpositioning --outputdir D:\Qt
```

### 构建与测试

```powershell
cd "D:\Project\HNS_Homework Notification_System_v2"

# 日常使用：Release 构建（推荐；同一页面加载比 Debug 快 6–8 倍，且不带控制台窗口）
.\build.ps1 -QtPrefix 'D:\Qt\6.8.3\msvc2022_64' -Config Release

# 排查问题：Debug 构建（含 GUI）+ 运行全部测试 + 部署 Qt 运行库
.\build.ps1 -QtPrefix 'D:\Qt\6.8.3\msvc2022_64'

# 只构建核心库与测试（不需要 Qt WebEngine）
.\build.ps1 -QtPrefix 'D:\Qt\6.8.3\msvc2022_64' -NoGui

# 同时构建真机验证工具
.\build.ps1 -QtPrefix 'D:\Qt\6.8.3\msvc2022_64' -WithVerifyTool

# 清空重建
.\build.ps1 -QtPrefix 'D:\Qt\6.8.3\msvc2022_64' -Clean
```

> **务必注意构建类型**：Debug 版基于未优化的 Qt/Chromium 调试库，同一页面加载实测需要
> **6.8–9.6 秒**，而 Release 版只要 **1.13 秒**（详见 `docs/03-v2变更记录.md` 第 10 节）。
> 日常使用请用 `-Config Release`。数据、日志、浏览器 Profile 都在可执行文件同级目录，
> 切换构建类型**不会**影响登录状态。

脚本会自动探测 Visual Studio 与 Windows SDK（本机 SDK 在 `D:\Windows Kits\10`，
不是默认路径，因此通过 `CMAKE_SYSTEM_VERSION` 显式传入）。

手工构建：

```powershell
cmake -S . -B build -G Ninja `
  -DCMAKE_PREFIX_PATH=D:/Qt/6.8.3/msvc2022_64 `
  -DCMAKE_BUILD_TYPE=Debug `
  -DCMAKE_SYSTEM_VERSION=10.0.26100.0
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

> 注意：GUI 冒烟测试用 `QT_QPA_PLATFORM=offscreen` 运行。构建脚本会把
> offscreen 平台插件部署到测试可执行文件旁（`tests/CMakeLists.txt` 的 POST_BUILD
> 步骤）。这是实测必需的：部署目录存在 `platforms/` 但缺少 offscreen 插件时，
> Qt 的平台插件查找会卡死（见 `docs/03-v2变更记录.md` 第 2.2 节）。

## 三、使用

1. 启动 `build\bin\njulms_app.exe`（同一时间只允许一个实例，v2 新增单实例保护）。
2. **程序启动后会自动打开智汇南雍并自动检查登录状态**（v2 新增）。
   会话有效时界面直接显示「已登录（可访问 N 门课程）」，你不需要做任何操作。
   只有会话真正失效时，才需要在浏览器窗口中**手动**完成统一身份认证
   （可能需要拖动滑块或扫码 —— 学校策略，程序不会也无法绕过）；
   登录成功后程序会自动重新检查并显示已登录。
   > 判定依据是受保护接口能否返回可解析 JSON，而不是首页是否加载成功。
4. 点「开始全量采集」。进度、当前课程与最近一次结果都会显示在界面上。
5. 之后可用「立即增量同步」，或勾选「启用定时同步」按周期（默认 60 分钟）自动同步。

### 增量同步的两种工作方式

| 模式 | 行为 | 常态开销 |
| --- | --- | --- |
| **快速增量**（默认开启，界面可关） | 先读一次跨课程待办 `GET /api/todos` 作为**变更信号**，只对"待办有变化/库里没有/截止时间不一致"的课程做完整（深）扫描 | **2 个请求**（课程列表 + 待办），通常 1–2 秒 |
| 完整扫描 | 逐门课程拉作业列表（首次采集、以及周期性全量对账都走这条） | N+1 个请求（N = 课程数） |

⚠️ 变更信号**只影响增量同步**：「开始全量采集」永远逐门课完整扫描，
不会因为"待办没变化"而跳过任何课程（这一点曾经有 bug，见
`docs/03-v2变更记录.md` 第 15 节）。

配套的**全量对账**：定时任务发现距上次成功全量扫描超过 24 小时（或从未全量成功过）时，
会把这一轮自动改成完整扫描——这样待办接口看不到的变化（标题/说明被改、已提交项、历史项）
最多 24 小时也会被对账到。

### 启动时自动全量扫描（默认开启）

程序启动并自动打开站点后，会执行一次**完整全量扫描**（`开始全量采集` 的那套流程），
保证你一打开程序看到的就是最新数据，不需要手动点任何按钮。

- 开关位置：「采集」页 → 定时同步区域 → **启动时自动全量扫描（保证数据最新）**，默认勾选；
  偏好会记住（`QSettings` 的 `sync/startupFullScan`）。
- 性能保护：**距上次成功全量扫描不足 5 分钟会跳过**（只做一次登录检查），
  避免"刚扫完又重启"重复发一整轮请求。
- 请求合并：这次扫描本身包含会话检查，所以启动时不会再额外单独查一次登录状态。
- 登录失效时不会空转：停在统一身份认证页面期间不触发，登录完成回跳后自动补上这次扫描。

### 界面设置会被记住

以下开关与数值写入 `QSettings`，下次启动自动沿用（不用每次重新设置）：

| 设置 | 键 | 默认 |
| --- | --- | --- |
| 启动时自动全量扫描 | `sync/startupFullScan` | 开 |
| 启用定时同步 | `sync/scheduleEnabled` | 关 |
| 定时周期（分钟） | `sync/scheduleIntervalMinutes` | 60 |
| 快速增量 | `sync/quickIncremental` | 开 |

> 注意：`sync/scheduleEnabled` 打开时，程序一启动就会自动启用定时同步（含 60 分钟默认周期，
> 每 24 小时自动升级一次全量对账）。如果不想让它常驻后台运行，把它取消勾选即可。

### 浏览器页面默认隐藏、按需弹出

浏览器不再占着主界面的下半部分，而是放在一个**独立的登录窗口**里，默认隐藏：

- 会话有效时你完全看不到它，后台照常取数；
- **会话失效需要人工登录时自动弹出**；登录成功后**自动隐藏**；
- 想主动打开/查看页面时，点「打开智汇南雍并登录」；
- 关闭该窗口只是隐藏，不会销毁页面与会话。

### 界面主题

界面使用统一的设计令牌（`src/ui/UiTheme.*`）：浅色/深色两套取值，**跟随系统主题自动切换**，
强调色、圆角、状态色各只有一套定义。改造要点与对比度预检结果见
`docs/03-v2变更记录.md` 第 12 节，改造前后截图在 `docs/images/`。

- 主操作（开始全量采集）为实心强调色；暂停/继续为次要动作；取消为描边危险色
- 表格：斑马纹 + 无网格线，时间与「来源 ID」列用等宽字体；无数据时显示空状态文案
- 所有可见文本不含 emoji / Markdown 标记；深浅两种主题的对比度均通过 WCAG AA

### iPad 前端（浏览 + 设置，v2 新增）

程序内置一个前端服务，用 iPad 的 Safari 就能看作业、改采集设置、手动触发同步，
不用在 iPad 上装任何东西：

- 访问地址：`https://laptop-pli3g45s.tail981d8b.ts.net/`（Tailscale 内网，MagicDNS）
- 服务只监听 `127.0.0.1:8790`，HTTPS 与身份边界由 Tailscale 提供
  （`tailscale serve --bg --https=443 http://127.0.0.1:8790`）；端口可用
  设置项 `web/port` 改（0 = 关闭），环境变量 `NJULMS_WEB_PORT` 优先
- **关窗口不再退出程序**：会缩到系统托盘继续跑（定时同步与 iPad 前端都靠它活着），
  要退出用托盘菜单的「退出程序」
- 三个 tab：**未提交**（跨课程 todo 卡片，点开看作业内容与要求）、
  **已提交**（按课程分组，点开按发布时间列出）、**设置**（周期、开关、手动增量/全量、
  暂停/继续/取消、字号）
- 页面默认**禁止缩放**（防误触），改字号在「设置 → 显示」；跟随系统深浅色、
  触摸目标 ≥44pt、适配安全区
- 改设置与触发同步需要**访问令牌**（PC 托盘菜单「复制访问令牌」→ iPad 设置里粘贴一次）；
  只读浏览不需要
- 设置页底部有**危险操作 → 删除全部数据**：点击会弹警告窗列出会删/不会删的内容，
  勾选确认后才会真正删除（保留登录状态与日志；同步进行中会拒绝）
- 作业胶囊右端的 **↗** 会在 **Safari** 里打开原站详情页（独立网页 app 不能加载外部网站，
  只能交给系统浏览器）；iPad 上第一次需要自己登录一次南大统一身份认证（过滑块），
  之后 Safari 会记住会话
- 「未提交」页签平时可能是空的（作业都交了）。想看这条 UI，可以跑
  `python tools\demo_pending.py` 放一条带「【测试】」字样的演示作业
  （`--remove` 删除；下次完整全量扫描也会自动清掉它，因为站点上没有这条）
- **采集一结束页面会自己刷新**（不管是 PC 上点的、定时任务还是 iPad 上发起的），
  并弹一条"采集完成，数据已刷新"；刷新按钮点下去也有转圈与"已刷新/失败"反馈
- 前端资源带内容哈希（`/app.js?v=xxxxxxxx`）：我改了界面，你只要**把网页 app 从后台
  划掉再打开**就能拿到新版（不用清缓存）。设置页「关于」里的「资源版本」就是当前版本
- 可以「添加到主屏幕」当应用打开（Safari 分享菜单 → 添加到主屏幕，图标用
  `/apple-touch-icon.png`，全屏运行）；各 tab 支持直链，例如 `?tab=settings`
- 设计与接口见 `docs/05-iPad前端.md`

### 删除全部数据（保留登录状态）

「数据」页底部有 **危险操作 → 删除全部数据（保留登录状态）**：

- 会清空本地数据库里的课程、作业、同步断点、运行/批次记录与字段解析异常；
- **不会**删除浏览器登录状态（不需要重新扫码/过滑块）与日志文件；
- 有同步在跑时会拒绝执行；清空在单个事务里完成（要么全删、要么不动）；
- 下一次同步会重新做一次完整采集。

### 数据与日志位置

| 内容 | 位置 |
| --- | --- |
| 数据库 | 可执行文件同级 `data/njulms.db`（可用环境变量 `NJULMS_DATA_DIR` 覆盖） |
| 日志 | 可执行文件同级 `logs/njulms.log`（可用 `NJULMS_LOG_DIR` 覆盖） |
| 浏览器 Profile | 可执行文件同级 `profile/`（可用 `NJULMS_PROFILE_DIR` 覆盖） |
| 单实例锁 | 数据目录内的 `njulms.lock` |

目录都在运行时创建，不硬编码在源码目录。**Profile 目录内含会话 Cookie，
等同于账号凭证：不要外传、不要提交到 Git。**

### 真机验证（可选，用于确认站点契约未变化）

```powershell
.\verify.ps1                 # 用最近一次登录的会话复测
.\verify.ps1 -CourseId 11311 # 指定课程探测分页与字段
```

它会先把 `build\bin\profile` 复制到临时目录（避免程序正在运行时的 Profile 锁），
再运行 `njulms_verify.exe`，最后**无论成功失败都删除临时 Profile**。
输出只含状态码、字节数、字段状态与聚合统计，不含 Cookie、令牌或作业正文。

## 四、已知限制（务必阅读）

1. **登录是自动保持的**（2026-10-09 实测）：
   - LMS 自己的 `session` Cookie 约 24 小时滚动过期（每次访问顺延），已持久化到 Profile；
   - 统一身份认证链的 `CASTGC` / `KEYCLOAK_IDENTITY` 等"会话级" Cookie，也会被
     Qt 的 `ForcePersistentCookies` 写进磁盘并在重启后恢复。实测：把 Profile 副本里
     `lms.nju.edu.cn` 的会话 Cookie 全部删除（模拟过期）后重启，程序仍能通过 SSO
     静默换回新会话（免密、13 门课程）。
   - 结论：正常情况下你不需要做任何操作；只有在学校侧 SSO 会话也失效
     （长期未使用、改密码、被强制下线）时才需要人工认证一次。
2. **定时同步的固有限制**：QTimer 在程序未运行（关机、退出）时不会触发，
   「每 N 分钟」只在程序运行期间有效；程序启动时会检查上次状态并提示补一次同步。
3. **访问频率**：默认 60 分钟、串行、相邻请求间隔 400 ms、课程间 300 ms，
   失败重试 ≤3 次且退避递增；站点没有公开的自动化频率规则，
   若你希望更频繁，请先自行确认是否合适。
4. **未验证的字段与路由**：作业「状态」与「详情链接」是按站点实测规则推导的值；
   `/activities` 不返回分页元数据，因此不用于推进断点；`publish_time` 经常缺失，
   缺失时保留库中原值，绝不用采集时间兜底。

## 五、架构

```
Qt WebEngine 浏览器与登录（WebEnginePageBridge）
        ↓ IPageBridge（抽象：在已登录页面里发一次同源请求）
认证状态管理（AuthManager：Unknown / Authenticated / NeedLogin）
        ↓
NjuLmsAdapter（接口路径、分页压平、去重、完整性判断）
        ↓
AssignmentParser（三态字段、UTC 时间、内容摘要、稳定 sourceId）
        ↓
SqliteRepository（唯一 SQL 入口：UPSERT、事务、软删除、运行记录、断点）
        ↕
SyncManager（全量/增量、批次与断点、暂停/继续/取消、恢复）
        ↑
Scheduler（QTimer 定时、单飞、启动孤儿状态回收）
UI：MainWindow（只显示与转发）
```

关键设计：

- **断点粒度 = 一门课程的完整作业快照**：作业写入 + 批次记录 + 断点更新在同一个
  SQLite 事务内，只有"能证明完整"的快照才推进断点。
- **恢复 = 重新扫描全部课程**：整份快照写入幂等，重扫天然避免漏判。
- **消失检测 = 软删除**：快照中消失的作业标记为不在场，保留历史。

## 六、目录结构

```
├─ CMakeLists.txt / build.ps1 / verify.ps1
├─ docs/
│  ├─ 00-可行性评估.md        可行性结论、实测证据、已知问题→修复对照
│  ├─ 01-实现决策.md          设计理由与踩坑记录（改代码前必读）
│  ├─ 02-阶段2验证结果.md     真机验证报告与字段契约审计（待人工登录复测）
│  └─ 03-v2变更记录.md        v2 相对 v1 的变更与本次修复的真实缺陷
├─ src/
│  ├─ models/     Course / Assignment / FieldState（三态标记）
│  ├─ parser/     AssignmentParser（JSON → 模型）
│  ├─ repository/ SqliteRepository（唯一 SQL 入口）
│  ├─ adapters/   IPageBridge + NjuLmsAdapter
│  ├─ auth/       AuthManager
│  ├─ sync/       SyncManager
│  ├─ scheduler/  Scheduler
│  ├─ utils/      Logger / Redact / TimeUtils / SingleInstanceLock
│  ├─ browser/    WebEnginePageBridge（Qt WebEngine）
│  ├─ ui/         MainWindow
│  └─ app/        main.cpp
├─ tests/         12 个测试套件（185 个用例）
└─ tools/         真机验证工具（verify_live.cpp）
```
