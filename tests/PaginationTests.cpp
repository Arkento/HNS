#include "adapters/NjuLmsAdapter.h"
#include "support/FakePageBridge.h"
#include "support/Fixtures.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QSet>
#include <QSignalSpy>
#include <QTest>

using namespace njulms;
using namespace njulms::test;

/**
 * 适配器的分页行为测试（需求场景 H）。
 *
 * 背景：实测 `page_size` 在 10 / 50 / 200 时被服务端原样接受，因此正常情况下
 * 一次请求就能取全量。但**如果服务端夹取了 page_size**（返回的条数少于 total），
 * 就必须翻页，否则会静默漏掉后面的作业。
 *
 * 这里验证三件事：
 *   1. 返回条数与 total 不一致时，会自动继续翻页；
 *   2. 各页结果按稳定标识合并，且去重（防翻页期间列表变化导致重复）；
 *   3. 合并后数量与 total 一致才算完整；不一致则明确标记不完整（断点不推进）。
 */
class PaginationTests : public QObject
{
    Q_OBJECT

private:
    static bool waitFor(const std::function<bool()> &predicate, int timeoutMs = 5000)
    {
        QElapsedTimer timer;
        timer.start();
        while (!predicate()) {
            if (timer.elapsed() > timeoutMs) {
                return false;
            }
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        }
        return true;
    }

    /// 让 bridge 对指定 courseId 的分页请求按 page 参数返回不同内容
    static void installPages(FakePageBridge &bridge, const QString &courseId,
                             const QList<QString> &pages)
    {
        // FakePageBridge 按「去掉查询串的路径」匹配，因此这里需要按 page 区分。
        // 它的 setSequentialResponses 正好满足需求：第 n 次调用取第 n 个响应。
        QList<FakePageBridge::Entry> entries;
        entries.reserve(pages.size());
        for (const QString &body : pages) {
            entries.append(FakePageBridge::Entry{body, 200, true});
        }
        bridge.setSequentialResponses(
            QStringLiteral("/api/courses/%1/homework-activities").arg(courseId), entries);
    }

    static NjuLmsAdapter::AssignmentListResult fetch(FakePageBridge &bridge, const QString &courseId)
    {
        NjuLmsAdapter adapter(&bridge);
        adapter.setMaxRetries(0);
        adapter.setRequestIntervalMs(0);

        std::optional<NjuLmsAdapter::AssignmentListResult> captured;
        adapter.fetchAssignments(courseId,
                                 [&](const NjuLmsAdapter::AssignmentListResult &r) {
                                     captured = r;
                                 });
        waitFor([&]() { return captured.has_value(); });
        return captured.value_or(NjuLmsAdapter::AssignmentListResult{});
    }

private slots:

    void singlePageIsComplete()
    {
        FakePageBridge bridge;
        bridge.setResponse(QStringLiteral("/api/courses/90001/homework-activities"),
                           fixtures::homeworkActivitiesThree());
        const auto res = fetch(bridge, QStringLiteral("90001"));

        QVERIFY(res.error.ok());
        QCOMPARE(res.assignments.size(), 3);
        QCOMPARE(res.reportedTotal, 3);
        QVERIFY(!res.paginated);
        QVERIFY(res.snapshotComplete);
        QCOMPARE(bridge.requestCount(), 1);
    }

    void multiPageIsFollowedAndMerged()
    {
        // 2.5 页：25 条，每页 10 条
        FakePageBridge bridge;
        installPages(bridge, QStringLiteral("90001"),
                     {fixtures::homeworkPage1(), fixtures::homeworkPage2(),
                      fixtures::homeworkPage3()});

        const auto res = fetch(bridge, QStringLiteral("90001"));

        QVERIFY(res.error.ok());
        // 关键：25 条全部拿到，且被判定为完整
        QCOMPARE(res.assignments.size(), 25);
        QCOMPARE(res.reportedTotal, 25);
        QVERIFY(res.paginated);
        QVERIFY2(res.snapshotComplete, "25 merged items equal the reported total, so it is complete");
        // 三次翻页请求
        QCOMPARE(bridge.requestCount(), 3);

        // 稳定标识必须唯一（去重生效）
        QSet<QString> ids;
        for (const Assignment &a : res.assignments) {
            ids.insert(a.sourceId);
        }
        QCOMPARE(ids.size(), res.assignments.size());
    }

    void repeatedPageContentIsDeduplicated()
    {
        // 模拟「服务端忽略 page 参数」或「列表在翻页期间前移」：
        // 三页返回相同内容。按稳定标识合并后只有 10 条，
        // 与 total=25 不符 → 必须判定为不完整，而不是凑数或无限翻页。
        FakePageBridge bridge;
        installPages(bridge, QStringLiteral("90001"),
                     {fixtures::homeworkPage1(), fixtures::homeworkPage1(),
                      fixtures::homeworkPage1()});

        const auto res = fetch(bridge, QStringLiteral("90001"));

        QVERIFY(res.error.ok());
        QCOMPARE(res.assignments.size(), 10);   // 去重后只剩 10 条
        QCOMPARE(res.reportedTotal, 25);
        QVERIFY2(!res.snapshotComplete,
                 "deduplicated count does not match total, so completeness must not be claimed");
    }

    void paginationStopsAtSafetyLimit()
    {
        // 每页都返回 10 条但 total 声称有 100000 条：
        // 必须在上限处停下，不能无限翻页。
        FakePageBridge bridge;
        const QString endless =
            QStringLiteral(R"({"homework_activities":[%1],"page":1,"page_size":10,)"
                           R"("pages":100000,"total":1000000,"start":1,"end":10})")
                .arg(fixtures::homeworkPage1()
                         .section(QStringLiteral("\"homework_activities\":["), 1)
                         .section(QStringLiteral("],\"page\""), 0, 0));
        bridge.setResponse(QStringLiteral("/api/courses/90001/homework-activities"), endless);

        const auto res = fetch(bridge, QStringLiteral("90001"));

        QVERIFY(res.error.ok());
        // 安全上限 100 页
        QCOMPARE(bridge.requestCount(), 100);
        QVERIFY2(!res.snapshotComplete, "a never-ending listing must never be called complete");
    }

    void pageWithoutTotalIsNeverComplete()
    {
        // 服务端没有分页元数据时无法证明完整（这也是 /activities 接口的情形）
        FakePageBridge bridge;
        bridge.setResponse(QStringLiteral("/api/courses/90001/homework-activities"),
                           QStringLiteral(R"({"homework_activities":[
                               {"id":70001,"title":"x","type":"homework",
                                "data":{"description":"d"}}]})"));
        const auto res = fetch(bridge, QStringLiteral("90001"));

        QVERIFY(res.error.ok());
        QCOMPARE(res.assignments.size(), 1);
        QCOMPARE(res.reportedTotal, -1);
        QVERIFY2(!res.snapshotComplete, "without a reported total, completeness cannot be proven");
    }

    void sessionExpiryOnSecondPageIsReported()
    {
        // 第一页正常、第二页会话失效：必须报告需要登录，而不是把残缺结果当完整
        FakePageBridge bridge;
        bridge.setSequentialResponses(
            QStringLiteral("/api/courses/90001/homework-activities"),
            {FakePageBridge::Entry{fixtures::homeworkPage1(), 200, true},
             FakePageBridge::Entry{fixtures::loginPageHtml(), 200, true}});

        NjuLmsAdapter adapter(&bridge);
        adapter.setMaxRetries(0);
        adapter.setRequestIntervalMs(0);
        QSignalSpy expired(&adapter, &NjuLmsAdapter::sessionExpired);

        std::optional<NjuLmsAdapter::AssignmentListResult> captured;
        adapter.fetchAssignments(QStringLiteral("90001"),
                                 [&](const NjuLmsAdapter::AssignmentListResult &r) {
                                     captured = r;
                                 });
        waitFor([&]() { return captured.has_value(); });

        QVERIFY(captured.has_value());
        QVERIFY(!captured->error.ok());
        QVERIFY(captured->error.needsLogin());
        QVERIFY(!captured->snapshotComplete);
        QCOMPARE(expired.count(), 1);
    }
};

QTEST_MAIN(PaginationTests)
#include "PaginationTests.moc"
