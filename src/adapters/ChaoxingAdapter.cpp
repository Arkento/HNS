#include "adapters/ChaoxingAdapter.h"

#include "parser/ChaoxingParser.h"
#include "utils/TimeUtils.h"

#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineView>

namespace njulms {
namespace {

Q_LOGGING_CATEGORY(lcChaoxing, "njulms.chaoxing")

/// [实测 2026-10-10] 学习通首页（登录后）。HTML 里含课程中心入口
constexpr auto kHomeUrl = "https://i.chaoxing.com/base?ws=2";

/// [实测] 课程中心：从首页 HTML 提取 s 参数后进入
constexpr auto kCourseCenterPrefix = "https://mooc1-1.chaoxing.com/visit/interaction?s=";

/**
 * 单个导航阶段的超时。学习通首屏实测 3~9 秒（同一时段），30 秒已非常宽松；
 * 超时按可重试的传输错误处理，绝不无限等待。
 */
constexpr int kStageTimeoutMs = 30000;

/// 首页阶段超时（含等待渲染）：隐藏窗口里首次渲染可能偏慢，给到 45 秒
constexpr int kHomeStageTimeoutMs = 45000;

/**
 * 轮询参数：点击「作业」后等 iframe、以及各页面等 JS 渲染出内容。
 * 实测课程列表与作业列表都会在导航完成后由页面 JS 再渲染一步，
 * loadFinished 立刻读 DOM 会拿到骨架（这正是首轮真机验证失败的形态）。
 */
constexpr int kIframePollIntervalMs = 250;
constexpr int kContentPollAttempts = 80;   // ≈20 秒（间隔缩到 250ms 后保持同样的等待窗口）

/**
 * 「找作业/考试入口」的窗口 ≈4 秒（16 轮）。
 * 实测课程页的「作业」「考试」入口都在 2.5 秒内渲染出来；4 秒里（含 4 次点击机会）
 * 还找不到就基本可以认定这门课没有该入口，早点跳过，不必白等 20 秒。
 * 拿到入口之后等待列表地址的窗口仍然是 20 秒。
 */
constexpr int kEntrySearchTicks = 16;

// ⚠️ 以下脚本含中文字面量（如课程页标签文本「作业」）。源文件是 UTF-8，
// 因此注入时必须用 QString::fromUtf8() 转换；用 fromLatin1 会把中文字节
// 逐个按 Latin-1 解释成乱码，字符串比较永远不匹配（真机调试踩过的坑）。
constexpr auto kClickWorkTabScript = R"JS((() => {
  const nodes = [...document.querySelectorAll('a,li,span,div')]
    .filter((n) => n.children.length === 0 && (n.textContent || '').trim() === '作业');
  if (!nodes.length) return 'notfound';
  // 优先点导航样式的元素（实测课程页的标签是 span.nav_content）：
  // 页面上可能有其它含「作业」字样的元素，点它不会切换标签。
  const el = nodes.find((n) => /nav|tab/i.test(n.className || '')) || nodes[0];
  (el.closest('a') || el).click();
  return 'clicked';
})())JS";

constexpr auto kReadWorkListUrlScript = R"JS((() => {
  const href = location.href || '';
  if (/work\/list/.test(href)) return href;
  const frames = [...document.querySelectorAll('iframe')];
  const hit = frames.find((f) => /work\/list/.test(f.src || ''));
  return hit ? hit.src : '';
})())JS";

constexpr auto kExtractCourseCenterScript = R"JS((() => {
  try {
    const html = document.documentElement ? document.documentElement.outerHTML : '';
    const m = html.match(/visit\/interaction\?s=([0-9a-zA-Z]{16,64})/);
    return JSON.stringify({ token: m ? m[1] : '', bytes: html.length, ready: document.readyState });
  } catch (e) {
    return JSON.stringify({ token: '', bytes: -1, ready: String(e) });
  }
})())JS";

constexpr auto kCountCourseLinksScript =
    R"JS(document.querySelectorAll('a[href*="stucoursemiddle"]').length)JS";

constexpr auto kWorkListRenderedScript = R"JS((() => {
  const html = document.documentElement ? document.documentElement.outerHTML : '';
  return /work\/task/.test(html) || /pageDiv/.test(html);
})())JS";

/// 失败前的一次性现场检查：入口是否出现、页面里有哪些 iframe
constexpr auto kWorkTabDiagnosticScript = R"JS((() => {
  const leaves = [...document.querySelectorAll('span,a,li,div')]
    .filter((n) => n.children.length === 0 && (n.textContent || '').trim() === '作业').length;
  return JSON.stringify({
    workLeaves: leaves,
    frames: [...document.querySelectorAll('iframe')].map((f) => (f.src || '').slice(0, 100)),
  });
})())JS";

// ---------------------------------------------------------------------------
// 考试（考试接入，2026-10-10）——与作业同一套流程，只换了标签文本与 URL 特征：
// 实测：点课程页「考试」标签后页面创建 iframe，
//       src = mooc1.chaoxing.com/exam-ans/mooc2/exam/exam-list?…
// ---------------------------------------------------------------------------

constexpr auto kClickExamTabScript = R"JS((() => {
  const nodes = [...document.querySelectorAll('a,li,span,div')]
    .filter((n) => n.children.length === 0 && (n.textContent || '').trim() === '考试');
  if (!nodes.length) return 'notfound';
  const el = nodes.find((n) => /nav|tab/i.test(n.className || '')) || nodes[0];
  (el.closest('a') || el).click();
  return 'clicked';
})())JS";

constexpr auto kReadExamListUrlScript = R"JS((() => {
  const href = location.href || '';
  if (/exam-list/.test(href)) return href;
  const frames = [...document.querySelectorAll('iframe')];
  const hit = frames.find((f) => /exam-list/.test(f.src || ''));
  return hit ? hit.src : '';
})())JS";

constexpr auto kExamListRenderedScript = R"JS((() => {
  const html = document.documentElement ? document.documentElement.outerHTML : '';
  return /暂无考试/.test(html) || /exam\/test\/look/.test(html)
      || /onclick="[A-Za-z0-9_]+\(\d+,\d+\)"/.test(html)
      || /onclick="[A-Za-z0-9_]+\('\d+','\d+'\)"/.test(html);
})())JS";

constexpr auto kExamTabDiagnosticScript = R"JS((() => {
  const leaves = [...document.querySelectorAll('span,a,li,div')]
    .filter((n) => n.children.length === 0 && (n.textContent || '').trim() === '考试').length;
  return JSON.stringify({
    workLeaves: leaves,
    frames: [...document.querySelectorAll('iframe')].map((f) => (f.src || '').slice(0, 100)),
  });
})())JS";

/// 日志用的安全 URL：只保留 scheme/host/path，绝不带查询串（含会话参数 s/enc）
QString safeUrlForLog(const QUrl &url)
{
    QUrl copy = url;
    copy.setQuery(QString());
    copy.setFragment(QString());
    return copy.toString();
}

bool isLoginHost(const QUrl &url)
{
    const QString host = url.host();
    return host.startsWith(QLatin1String("passport"))
           && host.endsWith(QLatin1String("chaoxing.com"));
}

/**
 * 分页线索检测（保守方向）。
 *
 * 实测作业列表底部有 <div class="pageDiv" id="page"> 分页容器，但翻页参数
 * **尚未实测确认**（见 docs/06-学习通探测.md 的遗留问题）。这里只在出现
 * "第 2 页及以后"的明确线索时判定可能多页：宁可把完整快照降级为不完整
 * （保留旧数据、记一条解析提示），也不能拿第一页当全量做软删除
 * ——那会误删后面几页的作业。
 */
bool evidenceOfMorePages(const QString &html)
{
    static const QRegularExpression pageRe(
        QStringLiteral("(?:goPage\\(|page=|pageNum=|currentPage=)[\"']?(\\d+)"));
    auto it = pageRe.globalMatch(html);
    while (it.hasNext()) {
        if (it.next().captured(1).toInt() >= 2) {
            return true;
        }
    }
    return html.contains(QStringLiteral("下一页")) || html.contains(QStringLiteral("goNextPage"));
}

} // namespace

// ---------------------------------------------------------------------------
// Impl：异步状态机
// ---------------------------------------------------------------------------

class ChaoxingAdapter::Impl
{
public:
    /// 要采集的列表类型：作业 / 考试（流程完全相同，脚本与缓存分开）
    enum class ListKind { Work, Exam };

    enum class Stage {
        Idle,
        Home,
        CourseCenter,
        CoursePage,   ///< 首次进入课程页：点「作业」/「考试」
        CourseReady,  ///< 已点过标签：等轮询拿到列表地址（避免重复点击）
        ListPage,     ///< 已进入作业/考试列表页
    };
    enum class Mode { None, Courses, Work };

    Impl(ChaoxingAdapter *owner, QWebEngineProfile *profile, QWebEngineView *view)
        : q(owner)
    {
        // 共享主页面同一个持久化 Profile：Cookie / 登录态天然复用。
        // ⚠️ 同进程不得为同一存储路径新建第二个 QWebEngineProfile（Chromium 会
        // 直接 abort），因此这里只接受外部传入的共享实例。
        // 页面以视图为父（与 WebEnginePageBridge 相同的构造方式）：
        // 实测"页面先于视图存在"时加载不可靠（loadFinished 不触发 / DOM 不渲染）。
        page = new QWebEnginePage(profile, view);
        view->setPage(page);

        timeout = new QTimer(q);
        timeout->setSingleShot(true);
        QObject::connect(timeout, &QTimer::timeout, q,
                         [this]() { fail(ErrorCode::Transport, QStringLiteral("学习通页面加载超时")); });

        poller = new QTimer(q);
        poller->setInterval(kIframePollIntervalMs);
        QObject::connect(poller, &QTimer::timeout, q, [this]() {
            if (pollTick) {
                pollTick();
            }
        });

        QObject::connect(page, &QWebEnginePage::loadFinished, q,
                         [this](bool ok) { onLoadFinished(ok); });
    }

    // ---- 入口 -------------------------------------------------------------

    void startCourses(CourseCallback callback)
    {
        begin(Mode::Courses);
        courseCb = std::move(callback);
        q->stageMessage(QStringLiteral("学习通：正在打开首页…"));
        navigate(QString::fromLatin1(kHomeUrl), Stage::Home, kHomeStageTimeoutMs);
    }

    void startList(const Course &course, ListKind kind, ListCallback callback)
    {
        begin(Mode::Work);
        currentCourse = course;
        listKind = kind;
        workTabClicked = false;
        listCb = std::move(callback);

        const bool isExam = kind == ListKind::Exam;
        const QString label = isExam ? QStringLiteral("考试") : QStringLiteral("作业");

        // 会话内缓存：同一门课的列表地址（enc 由页面生成、同一门课在同一会话里稳定）。
        // 直接复用可以跳过"课程页 + 点击 + 等 iframe"这三步（实测这三步是慢的部分），
        // 把重复同步从每门课 ~3 秒降到 ~1 秒。地址失效时自动回退到完整流程并刷新缓存。
        const QHash<QString, QString> &cache = isExam ? examUrlCache : workUrlCache;
        if (cache.contains(course.courseId)) {
            workByCachedUrl = true;
            triedCacheFallback = false;
            q->stageMessage(QStringLiteral("学习通：正在读取「%1」的%2列表…")
                                .arg(course.courseName, label));
            navigate(cache.value(course.courseId), Stage::ListPage);
            return;
        }
        workByCachedUrl = false;
        q->stageMessage(QStringLiteral("学习通：正在打开课程「%1」…").arg(course.courseName));
        navigate(course.detailUrl, Stage::CoursePage);
    }

    void cancel()
    {
        // 作废 generation：所有悬挂回调（runJavaScript / 定时器）都会静默失效
        ++generation;
        busy = false;
        mode = Mode::None;
        stage = Stage::Idle;
        expectingLoad = false;
        timeout->stop();
        stopPolling();
        courseCb = nullptr;
        listCb = nullptr;
        page->triggerAction(QWebEnginePage::Stop);
    }

    QWebEnginePage *page = nullptr;
    bool busy = false;

private:
    // ---- 通用 -------------------------------------------------------------

    void begin(Mode m)
    {
        mode = m;
        busy = true;
        ++generation;
        timeout->stop();
        stopPolling();
    }

    void navigate(const QString &url, Stage next, int timeoutMs = kStageTimeoutMs)
    {
        stage = next;
        expectingLoad = true;
        timeout->start(timeoutMs);
        page->load(QUrl(url));
    }

    /// runJavaScript 包装：generation 变化或任务结束后，回调静默作废
    void runJs(const QString &script, std::function<void(const QVariant &)> callback)
    {
        const quint64 gen = generation;
        page->runJavaScript(script, [this, gen, callback = std::move(callback)](
                                        const QVariant &value) {
            if (gen != generation || !busy) {
                return;
            }
            callback(value);
        });
    }

    void fail(ErrorCode code, const QString &message)
    {
        qCDebug(lcChaoxing) << "task failed; code:" << static_cast<int>(code)
                            << "message:" << message;
        Error error{code, message};
        if (mode == Mode::Courses) {
            auto cb = courseCb;
            end();
            if (cb) {
                cb(CourseListResult{error, {}});
            }
        } else if (mode == Mode::Work) {
            auto cb = listCb;
            end();
            if (cb) {
                cb(ListResult{error, {}, {}, false});
            }
        } else {
            end();
        }
    }

    void sessionExpired()
    {
        // 会话失效后页面生成的 enc 地址全部作废
        workUrlCache.clear();
        examUrlCache.clear();
        const QString message = QStringLiteral("需要登录学习通（请扫码）");
        emit q->loginRequired(message);
        fail(ErrorCode::SessionExpired, message);
    }

    void end()
    {
        ++generation;
        busy = false;
        mode = Mode::None;
        stage = Stage::Idle;
        expectingLoad = false;
        timeout->stop();
        stopPolling();
        courseCb = nullptr;
        listCb = nullptr;
    }

    void onLoadFinished(bool ok)
    {
        if (!busy || !expectingLoad) {
            return;   // 迟到的 loadFinished（例如上一任务的导航残余）
        }
        expectingLoad = false;
        timeout->stop();

        if (isLoginHost(page->url())) {
            sessionExpired();
            return;
        }
        if (!ok) {
            fail(ErrorCode::Transport, QStringLiteral("学习通页面加载失败"));
            return;
        }

        switch (stage) {
        case Stage::Home:        afterHome(); break;
        case Stage::CourseCenter: afterCourseCenter(); break;
        case Stage::CoursePage:  afterCoursePage(); break;
        case Stage::CourseReady: break;   // 点击后的次级加载：交给轮询处理
        case Stage::ListPage:    afterListPage(); break;
        case Stage::Idle:        break;
        }
    }

    // ---- 课程列表 ----------------------------------------------------------

    // ---- 轮询框架 ----------------------------------------------------------

    void startPolling(std::function<void()> tick, int maxAttempts = kContentPollAttempts)
    {
        pollTick = std::move(tick);
        pollMaxAttempts = maxAttempts;
        pollAttempts = 0;
        poller->start();
    }

    void stopPolling()
    {
        poller->stop();
        pollTick = nullptr;
        pollAttempts = 0;
    }

    /// 每轮先计一次尝试；超出上限说明"内容一直没出现"
    bool pollsExhausted() noexcept { return ++pollAttempts > pollMaxAttempts; }

    void readHtmlThen(std::function<void(const QString &)> callback)
    {
        runJs(QStringLiteral("document.documentElement.outerHTML"),
              [callback = std::move(callback)](const QVariant &value) {
                  callback(value.toString());
              });
    }

    // ---- 各阶段 ------------------------------------------------------------

    void afterHome()
    {
        qCDebug(lcChaoxing) << "home loaded:" << safeUrlForLog(page->url());
        // 一次性自检：确认 runJavaScript 在这只页面上确实有返回值
        // （真机调试用；返回 42 说明脚本执行链路正常，问题只可能在页面内容/时机）
        runJs(QStringLiteral("6 * 7"), [](const QVariant &value) {
            qCDebug(lcChaoxing) << "runJavaScript self-check:" << value;
        });
        // 首页内容（课程中心入口）由页面 JS 渲染，等它出现再提取，
        // 避免在骨架 DOM 上读到空结果（首轮真机验证正是这样失败的）。
        // 首页首次渲染可能偏慢（隐藏窗口），给到 ~36 秒。
        startPolling([this]() {
            if (pollsExhausted()) {
                stopPolling();
                fail(ErrorCode::UnexpectedShape,
                     QStringLiteral("学习通首页没有渲染出课程中心入口（模板改版或未登录）"));
                return;
            }
            runJs(QString::fromUtf8(kExtractCourseCenterScript), [this](const QVariant &value) {
                const QString payload = value.toString();
                const QJsonObject info = QJsonDocument::fromJson(payload.toUtf8()).object();
                const QString token = info.value(QStringLiteral("token")).toString();
                if (token.isEmpty()) {
                    // 每 10 轮记一次现场（字节数 / readyState / 当前地址），
                    // 便于对照"页面是否真的渲染出来了"
                    if (pollAttempts % 10 == 1) {
                        qCDebug(lcChaoxing)
                            << "home poll:" << pollAttempts
                            << "bytes:" << info.value(QStringLiteral("bytes"))
                            << "ready:" << info.value(QStringLiteral("ready")).toString()
                            << "url:" << safeUrlForLog(page->url());
                    }
                    return;   // 下一轮
                }
                stopPolling();
                qCDebug(lcChaoxing) << "course center token found";
                q->stageMessage(QStringLiteral("学习通：正在打开课程中心…"));
                navigate(QString::fromLatin1(kCourseCenterPrefix) + token, Stage::CourseCenter);
            });
        }, 140);   // ≈35 秒（间隔 250ms）
    }

    void afterCourseCenter()
    {
        qCDebug(lcChaoxing) << "course center loaded:" << safeUrlForLog(page->url());
        startPolling([this]() {
            if (pollsExhausted()) {
                stopPolling();
                fail(ErrorCode::UnexpectedShape,
                     QStringLiteral("学习通课程中心没有渲染出课程列表（模板改版或未登录）"));
                return;
            }
            runJs(QString::fromUtf8(kCountCourseLinksScript), [this](const QVariant &value) {
                if (value.toInt() <= 0) {
                    return;   // 下一轮
                }
                stopPolling();
                readHtmlThen([this](const QString &html) {
                    qCDebug(lcChaoxing) << "course center html bytes:" << html.size();
                    ParseIssues issues;
                    const QList<Course> courses =
                        ChaoxingParser::parseCourseList(html, timeutil::nowUtc(), &issues);
                    if (courses.isEmpty()) {
                        // 空列表无法区分"确实没有课程"与"未登录/改版"。
                        // 保守处理：报结构错误且**不写库**（避免空快照软删已有课程）。
                        fail(ErrorCode::UnexpectedShape,
                             QStringLiteral("学习通课程列表为空或未按实测模板渲染，本次不写库"));
                        return;
                    }
                    qCDebug(lcChaoxing) << "courses parsed:" << courses.size();
                    auto cb = courseCb;
                    CourseListResult result;
                    result.courses = courses;
                    end();
                    if (cb) {
                        cb(result);
                    }
                });
            });
        });
    }

    // ---- 作业 / 考试列表（同一套流程，脚本与缓存按 kind 切换）-----------------

    bool isExamKind() const noexcept { return listKind == ListKind::Exam; }

    QString listLabel() const
    {
        return isExamKind() ? QStringLiteral("考试") : QStringLiteral("作业");
    }

    QHash<QString, QString> &urlCache()
    {
        return isExamKind() ? examUrlCache : workUrlCache;
    }

    void afterCoursePage()
    {
        qCDebug(lcChaoxing) << "course page loaded:" << safeUrlForLog(page->url())
                            << "kind:" << (isExamKind() ? "exam" : "work");
        // 先切到"已点击"阶段：点击可能让顶层页面直接导航到列表页，
        // 那次导航的 loadFinished 不能再次触发点击（否则会死循环）。
        stage = Stage::CourseReady;
        // 课程页的导航栏同样由页面 JS 渲染：loadFinished 时刻可能还没有入口。
        // 因此点击也放进轮询：先等入口出现并点击，再等列表地址。
        startPolling([this]() {
            if (pollsExhausted()) {
                stopPolling();
                onTabTimeout();
                return;
            }
            // 窗口内找不到入口 → 判定该课程没有这个入口（快速跳过）
            if (!workTabClicked && pollAttempts > kEntrySearchTicks) {
                stopPolling();
                onTabTimeout();
                return;
            }
            // 入口刚出现时点击可能是空操作（事件尚未绑定），因此未拿到列表地址前：
            // 前 6 秒每秒点一次，之后每 4 秒补一次（总计最多约 10 次，拿到地址立即停止）。
            const bool clickFast = pollAttempts <= 24 && pollAttempts % 4 == 1;
            const bool clickSlow = pollAttempts % 16 == 1;
            if (clickFast || clickSlow) {
                const auto script = isExamKind() ? QString::fromUtf8(kClickExamTabScript)
                                                 : QString::fromUtf8(kClickWorkTabScript);
                runJs(script, [this](const QVariant &value) {
                    if (value.toString() == QLatin1String("clicked")) {
                        workTabClicked = true;
                        q->stageMessage(QStringLiteral("学习通：正在打开%1列表…").arg(listLabel()));
                    }
                    // notfound：入口还没渲染出来，下一轮再试
                });
            }
            const auto readScript = isExamKind() ? QString::fromUtf8(kReadExamListUrlScript)
                                                 : QString::fromUtf8(kReadWorkListUrlScript);
            runJs(readScript, [this](const QVariant &value) {
                const QString url = value.toString();
                if (url.isEmpty()) {
                    return;   // 下一轮
                }
                stopPolling();
                qCDebug(lcChaoxing) << "list url captured:" << safeUrlForLog(QUrl(url))
                                    << "kind:" << (isExamKind() ? "exam" : "work");
                if (url == page->url().toString()) {
                    // 顶层页面已经跳到列表页：直接读，不再重复导航
                    stage = Stage::ListPage;
                    afterListPage();
                } else {
                    navigate(url, Stage::ListPage);
                }
            });
        });
    }

    /**
     * 入口轮询耗尽：先取一次现场（入口是否出现过、有哪些 iframe），再决定语义。
     *  - 从未点到入口 → 判定"该课程没有这个入口"，返回空结果 + ParseIssue（跳过，不算失败）
     *  - 点到了但列表没出现 → 结构错误（可能改版），按失败处理
     */
    void onTabTimeout()
    {
        const auto diagnostic = isExamKind() ? QString::fromUtf8(kExamTabDiagnosticScript)
                                             : QString::fromUtf8(kWorkTabDiagnosticScript);
        runJs(diagnostic, [this](const QVariant &value) {
            qCDebug(lcChaoxing) << "tab timeout;" << "kind:" << (isExamKind() ? "exam" : "work")
                                << "clicked:" << workTabClicked
                                << "diagnostics:" << value.toString();
            if (workTabClicked) {
                fail(ErrorCode::UnexpectedShape,
                     QStringLiteral("点击「%1」后没有出现列表（可能已改版）").arg(listLabel()));
                return;
            }
            ListResult result;
            result.issues.append(ParseIssue(
                isExamKind() ? QStringLiteral("chaoxing.examtab")
                             : QStringLiteral("chaoxing.worktab"),
                isExamKind() ? QStringLiteral("course has no 考试 entry (likely no exam)")
                             : QStringLiteral("course has no 作业 entry (likely no homework)"),
                FieldState::Missing));
            auto cb = listCb;
            end();
            if (cb) {
                cb(result);
            }
        });
    }

    void afterListPage()
    {
        qCDebug(lcChaoxing) << "list page loaded:" << safeUrlForLog(page->url())
                            << "kind:" << (isExamKind() ? "exam" : "work");
        // 列表条目同样由页面 JS 渲染：等条目骨架出现后再读。
        startPolling([this]() {
            if (pollsExhausted()) {
                stopPolling();
                fail(ErrorCode::UnexpectedShape,
                     QStringLiteral("%1列表没有渲染出内容（模板改版或未登录）").arg(listLabel()));
                return;
            }
            const auto renderedScript = isExamKind() ? QString::fromUtf8(kExamListRenderedScript)
                                                     : QString::fromUtf8(kWorkListRenderedScript);
            runJs(renderedScript, [this](const QVariant &value) {
                if (!value.toBool()) {
                    return;   // 下一轮
                }
                stopPolling();
                readHtmlThen([this](const QString &html) {
                    qCDebug(lcChaoxing) << "list html bytes:" << html.size()
                                        << "kind:" << (isExamKind() ? "exam" : "work");
                    ParseIssues issues;
                    const QDateTime observedAt = timeutil::nowUtc();
                    const bool isExam = isExamKind();
                    const QList<Assignment> assignments =
                        isExam ? ChaoxingParser::parseExamList(html, currentCourse.courseId,
                                                               page->url().toString(), observedAt,
                                                               &issues)
                               : ChaoxingParser::parseWorkList(html, currentCourse.courseId,
                                                               observedAt, &issues);

                    const bool morePages = evidenceOfMorePages(html);
                    if (morePages) {
                        issues.append(ParseIssue(
                            QStringLiteral("chaoxing.pagination"),
                            QStringLiteral("list may span multiple pages; parameters not verified"),
                            FieldState::Invalid));
                    }

                    qCDebug(lcChaoxing) << "list items parsed:" << assignments.size()
                                        << "more pages:" << morePages
                                        << "kind:" << (isExam ? "exam" : "work");
                    ListResult result;
                    result.assignments = assignments;
                    result.issues = issues;
                    // 完整快照（可软删除）的判据：
                    //   - 没有分页嫌疑；且
                    //   - 解析到条目，或页面明确写着"暂无作业/暂无考试"（权威空列表）。
                    // 模板改版导致的"读不出内容"会带 chaoxing.worklist / chaoxing.examlist
                    // 的 Missing 问题，此时不算完整快照，绝不软删旧数据。
                    const QString unreadableField =
                        isExam ? QStringLiteral("chaoxing.examlist")
                               : QStringLiteral("chaoxing.worklist");
                    bool listUnreadable = false;
                    for (const ParseIssue &issue : issues) {
                        if (issue.field == unreadableField && issue.state == FieldState::Missing) {
                            listUnreadable = true;
                        }
                    }
                    result.snapshotComplete = !morePages && !listUnreadable;

                    // 缓存命中却读不出内容（例如地址过期返回"无权限的操作"）：
                    // 清掉这一条并回退到完整流程（课程页 → 点击 → 捕获新地址），
                    // 只回退一次，避免在真正改版时来回打转。
                    if (workByCachedUrl && listUnreadable) {
                        urlCache().remove(currentCourse.courseId);
                        if (!triedCacheFallback) {
                            triedCacheFallback = true;
                            workByCachedUrl = false;
                            workTabClicked = false;
                            qCDebug(lcChaoxing) << "cached list url invalid; full flow fallback"
                                                << "kind:" << (isExam ? "exam" : "work");
                            navigate(currentCourse.detailUrl, Stage::CoursePage);
                            return;   // 本次回调继续挂起，走完整流程后再交付结果
                        }
                    }
                    // 完整流程成功：记住这门课的列表地址，供本轮之后（含下一次同步）复用
                    if (!workByCachedUrl && !listUnreadable && !morePages
                        && page->url().isValid()) {
                        urlCache().insert(currentCourse.courseId, page->url().toString());
                    }

                    auto cb = listCb;
                    end();
                    if (cb) {
                        cb(result);
                    }
                });
            });
        });
    }

    // ---- 成员 -------------------------------------------------------------

    ChaoxingAdapter *q = nullptr;

    QTimer *timeout = nullptr;
    QTimer *poller = nullptr;
    std::function<void()> pollTick;
    int pollAttempts = 0;
    int pollMaxAttempts = kContentPollAttempts;

    Mode mode = Mode::None;
    Stage stage = Stage::Idle;
    bool expectingLoad = false;
    /// 本次要采集的列表类型（作业 / 考试）
    ListKind listKind = ListKind::Work;
    /// 本门课程的入口（作业/考试）是否已经点过（点击也要等页面渲染出来）
    bool workTabClicked = false;
    /// 会话内的作业列表地址缓存：courseId → work/list URL（见 startWork 的说明）
    QHash<QString, QString> workUrlCache;
    /// 会话内的考试列表地址缓存：courseId → exam-list URL（与作业同理）
    QHash<QString, QString> examUrlCache;
    /// 本次取数是否走的缓存地址（决定失败时要不要回退完整流程）
    bool workByCachedUrl = false;
    /// 本次取数是否已经因缓存失效回退过一次（防止来回打转）
    bool triedCacheFallback = false;
    quint64 generation = 0;

    Course currentCourse;
    CourseCallback courseCb;
    ListCallback listCb;
};

// ---------------------------------------------------------------------------
// 对外接口
// ---------------------------------------------------------------------------

ChaoxingAdapter::ChaoxingAdapter(QWebEngineProfile *profile, QWebEngineView *view, QObject *parent)
    : QObject(parent), d(std::make_unique<Impl>(this, profile, view))
{
}

ChaoxingAdapter::~ChaoxingAdapter() = default;

QWebEnginePage *ChaoxingAdapter::page() const
{
    return d->page;
}

void ChaoxingAdapter::openLoginPage()
{
    if (d->busy) {
        return;   // 任务进行中：不要打断状态机
    }
    d->page->load(QUrl(QString::fromLatin1(kHomeUrl)));
}

bool ChaoxingAdapter::isBusy() const
{
    return d->busy;
}

void ChaoxingAdapter::discoverCourses(CourseCallback callback)
{
    if (d->busy) {
        callback(CourseListResult{
            Error{ErrorCode::Transport, QStringLiteral("上一个学习通任务还没结束")},
            {}});
        return;
    }
    d->startCourses(std::move(callback));
}

void ChaoxingAdapter::fetchWorkList(const Course &course, ListCallback callback)
{
    if (d->busy) {
        callback(ListResult{
            Error{ErrorCode::Transport, QStringLiteral("上一个学习通任务还没结束")},
            {}, {}, false});
        return;
    }
    d->startList(course, Impl::ListKind::Work, std::move(callback));
}

void ChaoxingAdapter::fetchExamList(const Course &course, ListCallback callback)
{
    if (d->busy) {
        callback(ListResult{
            Error{ErrorCode::Transport, QStringLiteral("上一个学习通任务还没结束")},
            {}, {}, false});
        return;
    }
    d->startList(course, Impl::ListKind::Exam, std::move(callback));
}

void ChaoxingAdapter::cancel()
{
    d->cancel();
}

} // namespace njulms
