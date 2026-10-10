#include "web/WebApi.h"
#include "web/WebServer.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTest>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVector>

#include <atomic>

using namespace njulms;

/**
 * 只读前端服务（WebServer / WebApi）的契约测试。
 *
 * 覆盖两件事：
 *   1. 路由与 JSON 形状（含资源里的静态页面）；
 *   2. **线程约束**：所有数据回调都必须跑在主线程、且彼此不重叠。
 *      这条是「后端同步数据时前端来读会不会把数据搞坏 / 让程序崩掉」的
 *      直接验证：WebApi 用 BlockingQueuedConnection 把请求排进主线程，
 *      与界面操作、同步步骤天然串行。
 */
class WebServerTests : public QObject
{
    Q_OBJECT

private:
    /// 同步 GET：返回状态码与响应体
    static QPair<int, QByteArray> httpGet(quint16 port, const QString &path, int timeoutMs = 15000)
    {
        QNetworkAccessManager manager;
        QNetworkRequest request(
            QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(port).arg(path)));
        QNetworkReply *reply = manager.get(request);

        QEventLoop loop;
        QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();

        const QByteArray body = reply->readAll();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        reply->deleteLater();
        return {status, body};
    }

    /// 同步 POST：可选带访问令牌
    static QPair<int, QByteArray> httpPost(quint16 port, const QString &path,
                                           const QByteArray &body, const QByteArray &token,
                                           int timeoutMs = 15000)
    {
        QNetworkAccessManager manager;
        QNetworkRequest request(
            QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(port).arg(path)));
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        if (!token.isEmpty()) {
            request.setRawHeader("X-Auth-Token", token);
        }
        QNetworkReply *reply = manager.post(request, body);

        QEventLoop loop;
        QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();

        const QByteArray response = reply->readAll();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        reply->deleteLater();
        return {status, response};
    }

private slots:

    /// 路由、JSON 形状与静态页面
    void servesReadOnlyApi()
    {
        WebSources sources;
        sources.status = []() {
            QJsonObject data;
            data.insert(QStringLiteral("percent"), 42);
            return data;
        };
        sources.courses = []() {
            QJsonObject row;
            row.insert(QStringLiteral("id"), QStringLiteral("11311"));
            row.insert(QStringLiteral("name"), QStringLiteral("微积分I(第一层次)12班"));
            QJsonArray rows;
            rows.append(row);
            return rows;
        };
        sources.assignments = [](const QString &courseId) {
            QJsonArray rows;
            if (courseId == QLatin1String("11311")) {
                QJsonObject row;
                row.insert(QStringLiteral("title"), QStringLiteral("第四次作业"));
                rows.append(row);
            }
            return rows;
        };
        sources.logTail = [](int lines) { return QStringLiteral("line-%1").arg(lines); };

        WebApi api(sources);
        WebServer server(&api);

        QString error;
        QVERIFY2(server.start(0, &error), qPrintable(error));
        QVERIFY(server.isRunning());
        QVERIFY(server.port() > 0);

        // 健康检查
        const auto health = httpGet(server.port(), QStringLiteral("/api/health"));
        QCOMPARE(health.first, 200);
        QVERIFY(health.second.contains("\"ok\":true"));

        // 状态
        const auto status = httpGet(server.port(), QStringLiteral("/api/status"));
        QCOMPARE(status.first, 200);
        QCOMPARE(QJsonDocument::fromJson(status.second)
                     .object()
                     .value(QStringLiteral("data"))
                     .toObject()
                     .value(QStringLiteral("percent"))
                     .toInt(),
                 42);

        // 课程
        const auto courses = httpGet(server.port(), QStringLiteral("/api/courses"));
        QCOMPARE(courses.first, 200);
        QCOMPARE(QJsonDocument::fromJson(courses.second)
                     .object()
                     .value(QStringLiteral("data"))
                     .toArray()
                     .size(),
                 1);

        // 作业（路径参数）
        const auto assignments = httpGet(server.port(), QStringLiteral("/api/courses/11311/assignments"));
        QCOMPARE(assignments.first, 200);
        QCOMPARE(QJsonDocument::fromJson(assignments.second)
                     .object()
                     .value(QStringLiteral("data"))
                     .toArray()
                     .first()
                     .toObject()
                     .value(QStringLiteral("title"))
                     .toString(),
                 QStringLiteral("第四次作业"));

        // 日志（查询参数）
        const auto logs = httpGet(server.port(), QStringLiteral("/api/logs?tail=50"));
        QCOMPARE(logs.first, 200);
        QVERIFY(logs.second.contains("line-50"));

        // 静态页面与资源（验证 qrc 前缀与内容类型）
        const auto page = httpGet(server.port(), QStringLiteral("/"));
        QCOMPARE(page.first, 200);
        QVERIFY(page.second.contains("智汇南雍"));
        const auto css = httpGet(server.port(), QStringLiteral("/style.css"));
        QCOMPARE(css.first, 200);
        QVERIFY(css.second.contains("--accent"));
        // 桌面图标：加到主屏幕时 iOS 会用它
        const auto icon = httpGet(server.port(), QStringLiteral("/apple-touch-icon.png"));
        QCOMPARE(icon.first, 200);
        QVERIFY(icon.second.startsWith("\x89PNG"));
        // 离线外壳：PC 没开着时页面也能打开
        const auto worker = httpGet(server.port(), QStringLiteral("/service-worker.js"));
        QCOMPARE(worker.first, 200);
        QVERIFY(worker.second.contains("CACHE_NAME"));

        // 未注册路径
        const auto missing = httpGet(server.port(), QStringLiteral("/api/does-not-exist"));
        QCOMPARE(missing.first, 404);

        server.stop();
        QVERIFY(!server.isRunning());
    }

    /// 跨课程作业集合：scope 校验 + 说明随未提交列表一起下发
    void servesAssignmentsByScope()
    {
        WebSources sources;
        sources.assignmentsByScope = [](const QString &scope) {
            QJsonArray rows;
            if (scope == QLatin1String("pending")) {
                QJsonObject row;
                row.insert(QStringLiteral("courseId"), QStringLiteral("11311"));
                row.insert(QStringLiteral("courseName"), QStringLiteral("微积分I(第一层次)12班"));
                row.insert(QStringLiteral("title"), QStringLiteral("第四次作业"));
                row.insert(QStringLiteral("dueAt"), QStringLiteral("2026-10-15 23:59"));
                row.insert(QStringLiteral("dueRelative"), QStringLiteral("6 天 2 小时后截止"));
                row.insert(QStringLiteral("descriptionText"),
                           QStringLiteral("普通物理学（第八版）上册 1-23"));
                rows.append(row);
            }
            return rows;
        };

        WebApi api(sources);
        WebServer server(&api);
        QString error;
        QVERIFY2(server.start(0, &error), qPrintable(error));

        const auto pending = httpGet(server.port(), QStringLiteral("/api/assignments?scope=pending"));
        QCOMPARE(pending.first, 200);
        const QJsonArray pendingRows = QJsonDocument::fromJson(pending.second)
                                           .object()
                                           .value(QStringLiteral("data"))
                                           .toArray();
        QCOMPARE(pendingRows.size(), 1);
        QVERIFY(pendingRows.first().toObject().contains(QStringLiteral("descriptionText")));

        const auto submitted =
            httpGet(server.port(), QStringLiteral("/api/assignments?scope=submitted"));
        QCOMPARE(submitted.first, 200);
        QCOMPARE(QJsonDocument::fromJson(submitted.second)
                     .object()
                     .value(QStringLiteral("data"))
                     .toArray()
                     .size(),
                 0);

        const auto badScope = httpGet(server.port(), QStringLiteral("/api/assignments?scope=whatever"));
        QCOMPARE(badScope.first, 400);

        server.stop();
    }

    /// 写操作：令牌校验 + 后端裁决（同步进行中改设置必须被拒绝）
    void writeActionsRequireTokenAndRespectBackendVerdicts()
    {
        WebSources sources;
        sources.patchSettings = [](const QJsonObject &patch) -> WebActionResult {
            if (!patch.contains(QStringLiteral("scheduleIntervalMinutes"))) {
                return WebActionResult{true, 200, {}, QJsonObject{{QStringLiteral("applied"), true}}};
            }
            if (patch.value(QStringLiteral("scheduleIntervalMinutes")).toInt() < 15) {
                return WebActionResult{false, 409,
                                       QStringLiteral("正在同步，定时周期与开关请在同步结束后再修改"),
                                       {}};
            }
            return WebActionResult{true, 200, {}, QJsonObject{{QStringLiteral("applied"), true}}};
        };
        sources.startSync = [](const QString &mode) -> WebActionResult {
            if (mode != QLatin1String("full") && mode != QLatin1String("incremental")) {
                return WebActionResult{false, 400,
                                       QStringLiteral("mode 只能是 full 或 incremental"), {}};
            }
            return WebActionResult{true, 200, {},
                                   QJsonObject{{QStringLiteral("started"), mode}}};
        };
        sources.pauseSync = []() -> WebActionResult {
            return WebActionResult{false, 409, QStringLiteral("当前没有正在进行的同步"), {}};
        };
        sources.purgeData = [](const QString &confirm) -> WebActionResult {
            if (confirm != QLatin1String("DELETE")) {
                return WebActionResult{false, 400,
                                       QStringLiteral("缺少确认标记（confirm 必须为 DELETE）"), {}};
            }
            return WebActionResult{true, 200, {}, QJsonObject{{QStringLiteral("removed"), 42}}};
        };

        WebApi api(sources);
        WebServer server(&api);
        server.setAuthToken(QStringLiteral("secret-token"));
        QString error;
        QVERIFY2(server.start(0, &error), qPrintable(error));

        const QByteArray fullBody = R"({"mode":"full"})";

        // 没有令牌 / 令牌错误 → 401
        QCOMPARE(httpPost(server.port(), QStringLiteral("/api/sync"), fullBody, QByteArray()).first,
                 401);
        QCOMPARE(httpPost(server.port(), QStringLiteral("/api/sync"), fullBody, "wrong-token").first,
                 401);

        // 正确令牌 → 200，并且回执里带上后端返回的数据
        const auto accepted =
            httpPost(server.port(), QStringLiteral("/api/sync"), fullBody, "secret-token");
        QCOMPARE(accepted.first, 200);
        QVERIFY(accepted.second.contains("full"));

        // 后端拒绝（模拟"正在同步时改周期"）→ 409 + 原因原样透传
        const auto rejected = httpPost(server.port(), QStringLiteral("/api/settings"),
                                       R"({"scheduleIntervalMinutes":5})", "secret-token");
        QCOMPARE(rejected.first, 409);
        QVERIFY(rejected.second.contains("正在同步"));

        // 参数错误由后端裁决 → 400
        QCOMPARE(httpPost(server.port(), QStringLiteral("/api/sync"),
                          R"({"mode":"nope"})", "secret-token")
                     .first,
                 400);

        // 暂停：后端说没在跑 → 409
        QCOMPARE(httpPost(server.port(), QStringLiteral("/api/sync/pause"), QByteArray(),
                          "secret-token")
                     .first,
                 409);

        // 正常设置写入 → 200
        QCOMPARE(httpPost(server.port(), QStringLiteral("/api/settings"),
                          R"({"quickIncremental":true})", "secret-token")
                     .first,
                 200);

        // 删除数据：确认串不对 → 400；确认正确 → 200 且带回删除行数
        QCOMPARE(httpPost(server.port(), QStringLiteral("/api/data/purge"),
                          R"({"confirm":"yes"})", "secret-token")
                     .first,
                 400);
        const auto purged = httpPost(server.port(), QStringLiteral("/api/data/purge"),
                                     R"({"confirm":"DELETE"})", "secret-token");
        QCOMPARE(purged.first, 200);
        QVERIFY(purged.second.contains("42"));
        // 删除数据同样需要令牌
        QCOMPARE(httpPost(server.port(), QStringLiteral("/api/data/purge"),
                          R"({"confirm":"DELETE"})", QByteArray())
                     .first,
                 401);

        server.stop();
    }

    /// 并发请求也必须串行地跑在主线程里（不重叠、不换线程）
    void callbacksAreSerializedOnMainThread()
    {
        const QThread *mainThread = QThread::currentThread();
        std::atomic<int> inFlight{0};
        std::atomic<int> overlaps{0};
        std::atomic<int> offThread{0};
        std::atomic<int> calls{0};

        WebSources sources;
        sources.status = [&]() {
            if (QThread::currentThread() != mainThread) {
                ++offThread;
            }
            if (inFlight.fetch_add(1) != 0) {
                ++overlaps;   // 上一个还没返回就又进来一个 = 没有串行
            }
            QThread::msleep(25);   // 模拟在主线程里做事
            inFlight.fetch_sub(1);
            ++calls;
            QJsonObject data;
            data.insert(QStringLiteral("ok"), true);
            return data;
        };

        WebApi api(sources);
        WebServer server(&api);

        QString error;
        QVERIFY2(server.start(0, &error), qPrintable(error));

        constexpr int kRequests = 12;
        QNetworkAccessManager manager;
        QVector<QNetworkReply *> replies;
        replies.reserve(kRequests);
        const QUrl url(QStringLiteral("http://127.0.0.1:%1/api/status").arg(server.port()));
        for (int i = 0; i < kRequests; ++i) {
            replies.append(manager.get(QNetworkRequest(url)));
        }

        int finished = 0;
        QEventLoop loop;
        QTimer::singleShot(30000, &loop, &QEventLoop::quit);
        for (QNetworkReply *reply : replies) {
            connect(reply, &QNetworkReply::finished, &loop, [&finished, &loop, &replies]() {
                if (++finished == static_cast<int>(replies.size())) {
                    loop.quit();
                }
            });
        }

        QElapsedTimer elapsed;
        elapsed.start();
        loop.exec();

        QCOMPARE(finished, kRequests);
        QCOMPARE(offThread.load(), 0);   // 全部在主线程
        QCOMPARE(overlaps.load(), 0);    // 没有任何一次重叠
        QCOMPARE(calls.load(), kRequests);
        // 12 次 × 25 ms 串行 ≈ 300 ms；并行的话只要 ~25 ms
        QVERIFY2(elapsed.elapsed() >= 250,
                 qPrintable(QStringLiteral("耗时 %1 ms，看起来没有串行").arg(elapsed.elapsed())));
        for (QNetworkReply *reply : replies) {
            QCOMPARE(reply->error(), QNetworkReply::NoError);
            reply->deleteLater();
        }

        server.stop();
    }
};

QTEST_MAIN(WebServerTests)
#include "WebServerTests.moc"
