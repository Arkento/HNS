/**
 * 真机验证工具（阶段 2 的验证任务 V1–V3）。
 *
 * 这个程序**不是**产品的一部分，而是一次性的验证装置：它把真实的
 * WebEnginePageBridge + NjuLmsAdapter 接起来，对一个已登录的持久化
 * Profile 做最小请求，用来回答三个写代码前必须回答的问题：
 *
 *   V1  Qt WebEngine 能否在站点页面上下文里执行 runJavaScript 里的 fetch？
 *       （站点会检测 DevTools 并清空页面渲染，需要确认接口请求是否受影响）
 *   V2  会话失效时接口到底返回什么？状态码与正文形态各是什么？
 *       （决定「会话失效」的判定条件，直接影响需求场景 C）
 *   V3  page_size 的实际上限是多少？是否会被服务端夹取？
 *       （决定分页能否被压平成单次快照）
 *
 * 请求量刻意压到最小：1 次导航 + 约 6 次接口调用，无并发。
 *
 * 用法：
 *   set NJULMS_PROFILE_DIR=<持久化 Profile 目录>
 *   njulms_verify.exe [courseId]
 *
 * 输出只包含状态码、字节数、字段名与聚合统计；
 * **不会**输出 Cookie、令牌、认证 URL 或任何作业正文。
 */
#include "adapters/NjuLmsAdapter.h"
#include "browser/WebEnginePageBridge.h"
#include "repository/SqliteRepository.h"
#include "utils/ChromiumLogging.h"
#include "utils/Logger.h"
#include "utils/Redact.h"
#include "utils/TimeUtils.h"

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTextStream>
#include <QTimer>
#include <QUrl>
#include <QWebEnginePage>
#include <QWebEngineView>

using namespace njulms;

namespace {

QTextStream &out()
{
    static QTextStream stream(stdout);
    return stream;
}

/// 跑事件循环直到条件成立或超时
bool waitFor(const std::function<bool()> &predicate, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate()) {
        if (timer.elapsed() > timeoutMs) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    return true;
}

void section(const QString &title)
{
    out() << "\n=== " << title << " ===\n";
    out().flush();
}

} // namespace

int main(int argc, char *argv[])
{
    // 页面自身的 console.log 会以 INFO:CONSOLE 打到 stderr，污染验证报告
    quietChromiumInfoLogs();

    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);  // WebEngine 要求
    QApplication app(argc, argv);

    QCoreApplication::setOrganizationName(QStringLiteral("NjuLmsCrawler"));
    QCoreApplication::setApplicationName(QStringLiteral("NjuLmsCrawlerVerify"));
    Logger::install();

    const QStringList args = QCoreApplication::arguments();
    const QString courseIdArg = args.size() > 1 ? args.at(1) : QString();

    section(QStringLiteral("环境"));
    out() << "profile dir : " << WebEnginePageBridge::defaultProfileDir() << "\n";
    out() << "course arg  : " << (courseIdArg.isEmpty() ? QStringLiteral("(none)") : courseIdArg)
          << "\n";
    out().flush();

    WebEnginePageBridge bridge;
    QString error;
    if (!bridge.initialize(QString(), &error)) {
        out() << "FATAL: profile init failed: " << error << "\n";
        return 2;
    }

    bool loadOk = false;
    QObject::connect(&bridge, &IPageBridge::loadFinished, [&](bool ok, const QString &url) {
        loadOk = true;
        out() << "loadFinished ok=" << (ok ? QStringLiteral("true") : QStringLiteral("false")) << " url=" << url << "\n";
        out().flush();
    });

    NjuLmsAdapter adapter(&bridge);
    adapter.setMaxRetries(0);          // 验证阶段不重试，避免放大请求量
    adapter.setRequestIntervalMs(600); // 保持克制

    section(QStringLiteral("V1: 加载站点首页"));
    bridge.loadHome();
    if (!waitFor([&]() { return loadOk; }, 60000)) {
        out() << "TIMEOUT: page did not finish loading in 60s\n";
        return 3;
    }
    // 让 SPA 的初始化请求跑完
    waitFor([]() { return false; }, 4000);

    out() << "final url (redacted): " << redact::url(bridge.currentUrl()) << "\n";
    out() << "isOnAuthHost        : " << (bridge.isOnAuthHost() ? QStringLiteral("true") : QStringLiteral("false")) << "\n";
    out() << "looksAuthenticated  : " << (bridge.looksAuthenticated() ? QStringLiteral("true") : QStringLiteral("false")) << "\n";
    out().flush();

    // ---------------------------------------------------------------
    section(QStringLiteral("V1b: 页面全局对象诊断"));
    // ---------------------------------------------------------------
    // 直接问页面：qwebchannel.js 注入成功了吗？qt.webChannelTransport 在不在？
    // 若 hasQWebChannel 为 false，说明 DocumentCreation 注入没生效，
    // 结果回传通道永远建立不起来，请求只会以超时收场。
    {
        bool probeDone = false;
        QString probeResult;
        bridge.view()->page()->runJavaScript(
            QStringLiteral("(function(){ return JSON.stringify({"
                           "hasQt:(typeof qt!=='undefined'),"
                           "hasTransport:!!(window.qt&&window.qt.webChannelTransport),"
                           "hasQWebChannel:(typeof QWebChannel!=='undefined'),"
                           "hasBridge:(typeof window.__njulmsTransport!=='undefined'),"
                           "hasRequestFn:(typeof window.__njulmsRequest==='function')"
                           "}); })();"),
            [&](const QVariant &v) {
                probeResult = v.toString();
                probeDone = true;
            });
        if (waitFor([&]() { return probeDone; }, 20000)) {
            out() << "page globals: " << probeResult << "\n";
        } else {
            out() << "page globals: TIMEOUT (runJavaScript itself did not return)\n";
        }

        // 再做一次真实请求，并报告包装器的返回值与未决请求数：
        //   started           → 请求已发出，等通道回传
        //   wrapper-missing   → requestScript() 没有执行
        //   transport-missing → 页面里没有 qt.webChannelTransport
        // 若返回 started 但 pending 长期不减，说明 invokeMethod 的回执没回来。
        bool probe2Done = false;
        QString probe2;
        bridge.view()->page()->runJavaScript(
            QStringLiteral(
                "(function(){"
                "  var out = {};"
                "  out.wrapper = (typeof window.__njulmsRequest === 'function');"
                "  if (out.wrapper) {"
                "    window.__njulmsRequest('probe-1', '/api/my-courses', 'POST', '{}');"
                "    out.pendingAfter = Object.keys(window.__njulmsPending).length;"
                "    out.channelReady = window.__njulmsChannelReady;"
                "  }"
                "  return JSON.stringify(out);"
                "})();"),
            [&](const QVariant &v) {
                probe2 = v.toString();
                probe2Done = true;
            });
        if (waitFor([&]() { return probe2Done; }, 20000)) {
            out() << "invoke probe : " << probe2 << "\n";
        }
        // 给通道一点时间回传，再看 pending 是否被消费
        waitFor([]() { return false; }, 4000);

        // 关键诊断：onmessage 到底有没有被触发过？
        // rawMessages 一直为 0 说明 Qt 根本没往页面投递回执，
        // 问题在消息格式而不是 fetch 本身。
        bool probe3Done = false;
        QString probe3;
        bridge.view()->page()->runJavaScript(
            QStringLiteral(
                "JSON.stringify({pending:Object.keys(window.__njulmsPending).length,"
                "rawMessages:(window.__njulmsRawMessages||0),"
                "lastRaw:(window.__njulmsLastRaw||'').slice(0,120),"
                "hasOnmessage:!!(window.qt&&window.qt.webChannelTransport&&"
                "window.qt.webChannelTransport.onmessage)})"),
            [&](const QVariant &v) {
                probe3 = v.toString();
                probe3Done = true;
            });
        if (waitFor([&]() { return probe3Done; }, 20000)) {
            out() << "after 4s     : " << probe3 << "\n";
        }
        out().flush();
    }

    // ---------------------------------------------------------------
    section(QStringLiteral("V1: 页面内 fetch 能否工作（checkSession）"));
    // ---------------------------------------------------------------
    bool sessionDone = false;
    NjuLmsAdapter::SessionCheck sessionResult;
    QObject::connect(&adapter, &NjuLmsAdapter::sessionExpired, []() {
        out() << "signal: sessionExpired\n";
        out().flush();
    });

    adapter.checkSession([&](const NjuLmsAdapter::SessionCheck &r) {
        sessionResult = r;
        sessionDone = true;
    });
    if (!waitFor([&]() { return sessionDone; }, 45000)) {
        out() << "RESULT V1: FAIL - checkSession never called back "
                 "(runJavaScript/fetch is blocked or the channel is broken)\n";
        return 4;
    }

    out() << "authenticated : " << (sessionResult.authenticated ? QStringLiteral("true") : QStringLiteral("false")) << "\n";
    out() << "error code    : " << static_cast<int>(sessionResult.error.code) << "\n";
    out() << "error message : " << sessionResult.error.message << "\n";
    out() << "course count  : " << sessionResult.courseCount << "\n";
    out().flush();

    if (!sessionResult.authenticated) {
        out() << "\nRESULT V1: PARTIAL - fetch executed but session is not usable.\n"
                 "           This is the V2 case (session invalid). Inspect the error above:\n"
                 "           'NotJson' means the site returned HTML, which is exactly the\n"
                 "           condition that must be classified as NeedsLogin.\n";
        return 0;
    }
    out() << "\nRESULT V1: PASS - runJavaScript + fetch works against the live site.\n";

    // ---------------------------------------------------------------
    section(QStringLiteral("V3: 课程列表"));
    // ---------------------------------------------------------------
    bool coursesDone = false;
    NjuLmsAdapter::CourseListResult courseResult;
    adapter.discoverCourses([&](const NjuLmsAdapter::CourseListResult &r) {
        courseResult = r;
        coursesDone = true;
    });
    if (!waitFor([&]() { return coursesDone; }, 45000)) {
        out() << "TIMEOUT waiting for discoverCourses\n";
        return 5;
    }
    out() << "courses: " << courseResult.courses.size() << "\n";
    QString firstCourseId = courseIdArg;
    if (!courseResult.courses.isEmpty()) {
        const Course &c = courseResult.courses.first();
        if (firstCourseId.isEmpty()) {
            firstCourseId = c.courseId;
        }
        // 只打印字段形状，不打印全部课程名（减少个人信息输出）
        out() << "first course: id=" << redact::courseId(c.courseId)
              << " hasName=" << (!c.courseName.isEmpty() ? QStringLiteral("true") : QStringLiteral("false"))
              << " hasUrl=" << (!c.detailUrl.isEmpty() ? QStringLiteral("true") : QStringLiteral("false"))
              << " teachers=" << c.instructors.size() << "\n";
        out() << "semesterCode=" << c.semesterCode.value_or(QStringLiteral("(null)"))
              << " yearCode=" << c.academicYearCode.value_or(QStringLiteral("(null)")) << "\n";
    }
    out().flush();

    if (firstCourseId.isEmpty()) {
        out() << "no course available; cannot probe assignment endpoints\n";
        return 0;
    }

    // ---------------------------------------------------------------
    section(QStringLiteral("V3: page_size 上限与完整性校验"));
    // ---------------------------------------------------------------
    // 直接走适配器之外的最小 fetch，分别用不同 page_size 观察 total/pages 变化。
    // 用 IPageBridge 而不是适配器，是为了能自由改查询串。
    const QList<int> pageSizes = {10, 50, 200, 500, 1000};
    for (int ps : pageSizes) {
        const QString path = QStringLiteral("/api/courses/%1/homework-activities?page=1&page_size=%2")
                                 .arg(firstCourseId)
                                 .arg(ps);
        bool done = false;
        IPageBridge::Response resp;
        bridge.request(IPageBridge::Request::get(path), [&](const IPageBridge::Response &r) {
            resp = r;
            done = true;
        });
        if (!waitFor([&]() { return done; }, 45000)) {
            out() << "page_size=" << ps << " TIMEOUT\n";
            continue;
        }

        QString summary;
        if (resp.transportOk && resp.httpStatus == 200) {
            const QJsonObject root = QJsonDocument::fromJson(resp.body.toUtf8()).object();
            const QJsonArray items = root.value(QStringLiteral("homework_activities")).toArray();
            summary = QStringLiteral("status=%1 bytes=%2 items=%3 reportedTotal=%4 "
                                     "reportedPageSize=%5 pages=%6")
                          .arg(resp.httpStatus)
                          .arg(resp.body.size())
                          .arg(items.size())
                          .arg(root.value(QStringLiteral("total")).toInt(-1))
                          .arg(root.value(QStringLiteral("page_size")).toInt(-1))
                          .arg(root.value(QStringLiteral("pages")).toInt(-1));
        } else {
            summary = QStringLiteral("status=%1 transportOk=%2 error=%3")
                          .arg(resp.httpStatus)
                          .arg(resp.transportOk ? QStringLiteral("true") : QStringLiteral("false"))
                          .arg(resp.error);
        }
        out() << "page_size=" << ps << " -> " << summary << "\n";
        out().flush();
    }

    // ---------------------------------------------------------------
    section(QStringLiteral("适配器视角：fetchAssignments 的完整性结论"));
    // ---------------------------------------------------------------
    bool assignDone = false;
    NjuLmsAdapter::AssignmentListResult assignResult;
    adapter.fetchAssignments(firstCourseId,
                             [&](const NjuLmsAdapter::AssignmentListResult &r) {
                                 assignResult = r;
                                 assignDone = true;
                             });
    if (waitFor([&]() { return assignDone; }, 45000)) {
        out() << "parsed=" << assignResult.assignments.size()
              << " reportedTotal=" << assignResult.reportedTotal
              << " snapshotComplete=" << (assignResult.snapshotComplete ? QStringLiteral("true") : QStringLiteral("false"))
              << " issues=" << assignResult.issues.size() << "\n";
        if (!assignResult.assignments.isEmpty()) {
            const Assignment &a = assignResult.assignments.first();
            out() << "first assignment field states:\n";
            out() << "  sourceId     = " << a.sourceId << "\n";
            out() << "  title        = " << fieldStateName(a.title.state()) << "\n";
            out() << "  description  = " << fieldStateName(a.description.state()) << "\n";
            out() << "  dueAt        = " << fieldStateName(a.dueAt.state()) << "\n";
            out() << "  publishedAt  = " << fieldStateName(a.publishedAt.state()) << "\n";
            out() << "  status       = " << fieldStateName(a.status.state())
                  << " (" << assignmentStatusKey(a.status.hasValue() ? a.status.require()
                                                                    : AssignmentStatus::Unknown)
                  << ")\n";
            out() << "  submitted    = " << fieldStateName(a.submitted.state()) << "\n";
            out() << "  score        = " << fieldStateName(a.score.state()) << "\n";
        }
    } else {
        out() << "TIMEOUT waiting for fetchAssignments\n";
    }
    out().flush();

    // ---------------------------------------------------------------
    section(QStringLiteral("V5/V6: 逐课程扫描（全部课程）"));
    // ---------------------------------------------------------------
    // 目标：
    //   - 确认每门课程的作业列表都能解析，且完整性结论与响应里的 total 一致；
    //   - 找出第一门「有作业」的课程，打印字段状态（V5/V6 的真机证据）；
    //   - 验证 page_size 被压平后（服务端上限 100）不会漏项。
    // 请求保持串行；适配器自身有 600ms 间隔，13 门课约十几次请求。
    {
        int coursesSeen = 0;
        int coursesWithAssignments = 0;
        int totalAssignments = 0;
        bool detailsPrinted = false;

        for (const Course &c : std::as_const(courseResult.courses)) {
            ++coursesSeen;

            bool done = false;
            NjuLmsAdapter::AssignmentListResult result;
            adapter.fetchAssignments(c.courseId,
                                     [&](const NjuLmsAdapter::AssignmentListResult &r) {
                                         result = r;
                                         done = true;
                                     });
            if (!waitFor([&]() { return done; }, 60000)) {
                out() << "course " << redact::courseId(c.courseId) << " : TIMEOUT\n";
                out().flush();
                continue;
            }

            out() << "course " << redact::courseId(c.courseId)
                  << " : parsed=" << result.assignments.size()
                  << " reportedTotal=" << result.reportedTotal
                  << " complete="
                  << (result.snapshotComplete ? QStringLiteral("true") : QStringLiteral("false"))
                  << " issues=" << result.issues.size()
                  << " error="
                  << (result.error.ok() ? QStringLiteral("none") : result.error.message) << "\n";
            out().flush();

            if (!result.assignments.isEmpty()) {
                ++coursesWithAssignments;
                totalAssignments += result.assignments.size();

                if (!detailsPrinted) {
                    detailsPrinted = true;
                    const Assignment &a = result.assignments.first();
                    out() << "  (first course with assignments; field states)\n";
                    out() << "  sourceId     = " << a.sourceId << "\n";
                    out() << "  title        = " << fieldStateName(a.title.state()) << "\n";
                    out() << "  description  = " << fieldStateName(a.description.state()) << "\n";
                    out() << "  dueAt        = " << fieldStateName(a.dueAt.state()) << "\n";
                    out() << "  publishedAt  = " << fieldStateName(a.publishedAt.state()) << "\n";
                    out() << "  status       = " << fieldStateName(a.status.state()) << "\n";
                    out() << "  submitted    = " << fieldStateName(a.submitted.state()) << "\n";
                    out() << "  score        = " << fieldStateName(a.score.state()) << "\n";
                    out() << "  detailUrl    = "
                          << (a.detailUrl.hasValue() ? redact::url(a.detailUrl.require())
                                                     : QStringLiteral("(none)"))
                          << "\n";
                    out().flush();
                }
            }
        }

        out() << "summary: courses=" << coursesSeen
              << " withAssignments=" << coursesWithAssignments
              << " assignments=" << totalAssignments << "\n";
        out().flush();
    }

    // ---------------------------------------------------------------
    section(QStringLiteral("候选聚合接口探测：从站点前端 JS 里提取 /api/ 路径"));
    // ---------------------------------------------------------------
    // 目的：判断有没有「一次拿到跨课程数据」的接口。若有，常态增量同步可以从
    // N+1 个请求（1 个课程列表 + 每门课一个）降到 1–3 个：
    // 先用它判断"哪些课程可能变了"，再只对变化的课程做深度扫描。
    //
    // 做法完全只读：从页面 memory 里取已加载的 /static/*.js 资源列表，
    // 把其中前几个 bundle 拉下来做静态文本扫描（和人工看 DevTools 的 Sources 一样），
    // 不调用任何未确认用途的接口。
    {
        bool listed = false;
        QStringList resources;
        bridge.view()->page()->runJavaScript(
            QStringLiteral("JSON.stringify(performance.getEntriesByType('resource')"
                           ".map(function(e){return e.name;}))"),
            [&](const QVariant &v) {
                const QJsonArray array = QJsonDocument::fromJson(v.toString().toUtf8()).array();
                for (const QJsonValue &item : array) {
                    resources << item.toString();
                }
                listed = true;
            });
        if (!waitFor([&]() { return listed; }, 20000)) {
            out() << "resource list: TIMEOUT\n";
        }

        QStringList bundles;
        for (const QString &url : std::as_const(resources)) {
            if (url.contains(QLatin1String("/static/")) && url.contains(QLatin1String(".js")) &&
                !bundles.contains(url)) {
                bundles << url;
            }
        }
        out() << "loaded js bundles: " << bundles.size() << "\n";

        QStringList apiPaths;
        const QRegularExpression apiPattern(QStringLiteral("/(?:api|ntf)/[A-Za-z0-9_\\-/{}.$:]+"));
        int fetched = 0;
        int scannedBytes = 0;
        constexpr int kMaxBundles = 60;          // 最多扫 60 个 chunk
        constexpr int kMaxScanBytes = 8 * 1024 * 1024;   // 累计不超过 8 MB，避免拖太久
        for (const QString &url : std::as_const(bundles)) {
            if (fetched >= kMaxBundles || scannedBytes >= kMaxScanBytes) {
                break;
            }
            const QUrl bundleUrl(url);
            if (bundleUrl.host() != QLatin1String("lms.nju.edu.cn")) {
                continue;
            }
            QString path = bundleUrl.path();
            if (!bundleUrl.query().isEmpty()) {
                path += QLatin1Char('?') + bundleUrl.query();
            }

            bool done = false;
            IPageBridge::Response resp;
            bridge.request(IPageBridge::Request::get(path), [&](const IPageBridge::Response &r) {
                resp = r;
                done = true;
            });
            if (!waitFor([&]() { return done; }, 60000)) {
                out() << "  bundle TIMEOUT: " << path << "\n";
                continue;
            }
            ++fetched;
            if (!resp.transportOk || resp.httpStatus != 200) {
                out() << "  bundle status=" << resp.httpStatus << " bytes=" << resp.body.size()
                      << " : " << path << "\n";
                continue;
            }
            out() << "  scanned " << path << " (" << resp.body.size() << " bytes)\n";
            scannedBytes += resp.body.size();

            auto matches = apiPattern.globalMatch(resp.body);
            while (matches.hasNext()) {
                const QString candidate = matches.next().captured(0);
                if (!apiPaths.contains(candidate)) {
                    apiPaths << candidate;
                }
            }
        }

        apiPaths.sort();
        out() << "distinct api paths found: " << apiPaths.size() << "\n";
        int printed = 0;
        for (const QString &candidate : std::as_const(apiPaths)) {
            if (printed++ >= 400) {
                out() << "  ... (more omitted)\n";
                break;
            }
            out() << "  " << candidate << "\n";
        }

        // 关键词汇总：一眼看出有没有"跨课程/待办/截止/日历"这类可能的聚合接口
        const QStringList keywords = {
            QStringLiteral("todo"),        QStringLiteral("deadline"),
            QStringLiteral("homework"),    QStringLiteral("calendar"),
            QStringLiteral("activit"),     QStringLiteral("upcoming"),
            QStringLiteral("bulletin"),    QStringLiteral("notification"),
            QStringLiteral("my-courses"),  QStringLiteral("user/"),
        };
        out() << "keyword summary:\n";
        for (const QString &keyword : std::as_const(keywords)) {
            QStringList hits;
            for (const QString &candidate : std::as_const(apiPaths)) {
                if (candidate.contains(keyword, Qt::CaseInsensitive)) {
                    hits << candidate;
                }
            }
            out() << "  " << keyword << " (" << hits.size() << "): "
                  << hits.join(QLatin1Char(' ')) << "\n";
        }

        // ---- 直接试探若干候选聚合路径（只读 GET；404 同样是有价值的信息）----
        const QStringList candidates = {
            QStringLiteral("/api/todo-list"),
            QStringLiteral("/api/todos"),
            QStringLiteral("/api/todo_list"),
            QStringLiteral("/api/activities?type=homework"),
            QStringLiteral("/api/course/activities"),
            QStringLiteral("/api/course/%1/activities").arg(firstCourseId),
            QStringLiteral("/api/upcoming"),
            QStringLiteral("/api/calendar/activities"),
        };
        out() << "candidate aggregate paths:\n";
        for (const QString &candidate : std::as_const(candidates)) {
            bool done = false;
            IPageBridge::Response resp;
            bridge.request(IPageBridge::Request::get(candidate),
                           [&](const IPageBridge::Response &r) {
                               resp = r;
                               done = true;
                           });
            if (!waitFor([&]() { return done; }, 30000)) {
                out() << "  " << candidate << " -> TIMEOUT\n";
                continue;
            }

            QString shape;
            if (resp.transportOk && resp.httpStatus == 200) {
                const QJsonDocument doc = QJsonDocument::fromJson(resp.body.toUtf8());
                if (doc.isObject()) {
                    shape = QStringLiteral("keys=") + doc.object().keys().join(QLatin1Char(','));
                } else if (doc.isArray()) {
                    shape = QStringLiteral("array len=%1").arg(doc.array().size());
                } else {
                    shape = QStringLiteral("non-json");
                }
            } else {
                shape = resp.error.isEmpty() ? QStringLiteral("(no error text)") : resp.error;
            }
            out() << "  " << candidate << " -> status=" << resp.httpStatus
                  << " bytes=" << resp.body.size() << " " << shape << "\n";
            out().flush();
        }

        // ---- /api/todos 细看：这是目前唯一命中的跨课程聚合接口 ----
        // 只看结构（条数 / 字段名 / 类型集合），不打印任何标题或正文。
        {
            bool done = false;
            IPageBridge::Response resp;
            bridge.request(IPageBridge::Request::get(QStringLiteral("/api/todos")),
                           [&](const IPageBridge::Response &r) {
                               resp = r;
                               done = true;
                           });
            if (waitFor([&]() { return done; }, 30000) && resp.httpStatus == 200) {
                const QJsonObject root = QJsonDocument::fromJson(resp.body.toUtf8()).object();
                const QJsonArray todos = root.value(QStringLiteral("todo_list")).toArray();
                out() << "  /api/todos detail: items=" << todos.size() << "\n";
                if (!todos.isEmpty()) {
                    const QJsonObject first = todos.first().toObject();
                    out() << "    first item keys: " << first.keys().join(QLatin1Char(',')) << "\n";
                    // end_time 的格式决定「截止时间变化」能否被识别（值本身只是时间，不含正文）
                    const QString rawEnd = first.value(QStringLiteral("end_time")).toString();
                    out() << "    end_time raw: " << rawEnd << " parsedOk: "
                          << (timeutil::parseSiteTime(rawEnd).has_value() ? QStringLiteral("true")
                                                                          : QStringLiteral("false"))
                          << "\n";
                    QStringList types;
                    for (const QJsonValue &value : todos) {
                        const QString type =
                            value.toObject().value(QStringLiteral("type")).toString();
                        if (!type.isEmpty() && !types.contains(type)) {
                            types << type;
                        }
                    }
                    out() << "    distinct types: " << types.join(QLatin1Char(',')) << "\n";
                }
            }
            out().flush();
        }
        out().flush();
    }

    section(QStringLiteral("完成"));
    out() << "V1 answered, V3 answered, V2 requires an expired session (see report).\n";
    out().flush();
    return 0;
}
