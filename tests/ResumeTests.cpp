#include "adapters/NjuLmsAdapter.h"
#include "parser/AssignmentParser.h"
#include "repository/SqliteRepository.h"
#include "support/FakePageBridge.h"
#include "support/Fixtures.h"

#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace njulms;
using namespace njulms::test;

namespace {

QList<Assignment> assignmentsFrom(const QString &json, const QString &courseId)
{
    const QJsonObject root = QJsonDocument::fromJson(json.toUtf8()).object();
    const QJsonArray items = root.value(QStringLiteral("homework_activities")).toArray();
    return AssignmentParser::parseAssignments(courseId, items, fixtures::observedBeforeDeadlines(), nullptr);
}

/**
 * 跑一次适配器调用并**等待回调**。
 *
 * 为什么必须等待：适配器的重试由 QTimer::singleShot 延迟触发，
 * 而 FakePageBridge 的首次回调是同步的。若调用后立即返回，
 * 回调还没发生，拿到的是默认构造的结果（其 error.ok() 为 true），
 * 重试用例会因此被误判为「请求成功」。
 */
template <typename Result, typename Starter>
Result runSync(Starter &&starter, int timeoutMs = 5000)
{
    std::optional<Result> captured;
    starter([&captured](const Result &r) { captured = r; });

    QElapsedTimer timer;
    timer.start();
    while (!captured.has_value() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return captured.value_or(Result{});
}

} // namespace

/**
 * 断点续传与异常恢复测试（需求文档第四节）。
 *
 * 覆盖场景：
 *   A 采集中途程序被强制关闭 → 重启后从最后一个已提交批次继续
 *   B 数据库事务失败 → 作业记录与断点一起回滚
 *   C 登录会话过期 → 暂停并提示登录，不得解析成空作业列表
 *   D 网络请求失败 → 有限次重试 + 递增等待，持续失败保留断点
 *   H 分页内容动态变化 → 无法证明完整时不得推进断点
 */
class ResumeTests : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    int m_dbCounter = 0;

    QString nextDbPath() { return QDir(m_dir.path()).filePath(QStringLiteral("resume%1.db").arg(++m_dbCounter)); }

private slots:

    void initTestCase() { QVERIFY(m_dir.isValid()); }

    // ---- 场景 A：中途强制关闭后恢复 ---------------------------------------

    void scenarioA_resumesFromLastCommittedBatch()
    {
        const QString path = nextDbPath();

        // ---- 第一次运行：写入前两门课的第 1 批，然后「进程被杀」 ----
        QString firstRunTaskKey = QStringLiteral("full-scan");
        qint64 runId = 0;
        {
            auto repo = std::make_unique<SqliteRepository>();
            QString error;
            QVERIFY2(repo->open(path, &error), qPrintable(error));
            // 模拟上次是被强制关闭的：先留下一个 running 的 run
            runId = repo->startRun(firstRunTaskKey, QStringLiteral("full"), &error);
            QVERIFY2(runId > 0, qPrintable(error));

            // 批次 1：课程 90001
            const BatchApplyResult r1 = repo->applyAssignmentSnapshot(
                QStringLiteral("90001"),
                assignmentsFrom(fixtures::homeworkActivitiesThree(), QStringLiteral("90001")),
                runId, 1, QStringLiteral(""), QStringLiteral("course:90001"));
            QVERIFY2(r1.ok, qPrintable(r1.error));
            QCOMPARE(r1.inserted, 3);

            // 故意不调用 finishRun，直接 close —— 等价于进程被强制结束
            repo->close();
        }

        // ---- 第二次运行：模拟重启 ----
        {
            auto repo = std::make_unique<SqliteRepository>();
            QString error;
            QVERIFY2(repo->open(path, &error), qPrintable(error));

            // 重启后必须识别到上次未完成的运行
            QCOMPARE(repo->reapOrphanedRuns(), 1);
            const auto reaped = repo->findRun(runId);
            QVERIFY(reaped.has_value());
            QCOMPARE(reaped->status, CrawlRun::Status::Failed);

            // 已提交批次的断点必须保留，可从它继续
            const auto state =
                repo->findSyncState(SqliteRepository::courseTaskKey(QStringLiteral("90001")));
            QVERIFY(state.has_value());
            QVERIFY(state->fullScanCompleted);
            QCOMPARE(state->cursor, QStringLiteral("course:90001"));

            // 已提交批次的数据必须完整保留（不重复、不丢失）
            QCOMPARE(repo->countAssignments(QStringLiteral("90001")), 3);

            // 从断点继续：新起一个运行，采集第二门课
            const qint64 run2 = repo->startRun(QStringLiteral("full-scan"),
                                               QStringLiteral("full"), &error);
            QVERIFY2(run2 > 0, qPrintable(error));

            // 重新采集第一门课（覆盖式快照）不应产生重复
            const BatchApplyResult again = repo->applyAssignmentSnapshot(
                QStringLiteral("90001"),
                assignmentsFrom(fixtures::homeworkActivitiesThree(), QStringLiteral("90001")),
                run2, 1, QStringLiteral("course:90001"), QStringLiteral("course:90002"));
            QVERIFY2(again.ok, qPrintable(again.error));
            QCOMPARE(again.inserted, 0);
            QCOMPARE(again.unchanged, 3);
            QCOMPARE(repo->countAssignments(QStringLiteral("90001")), 3);

            // 第二门课
            const BatchApplyResult r2 = repo->applyAssignmentSnapshot(
                QStringLiteral("90002"),
                assignmentsFrom(fixtures::homeworkActivitiesThree(), QStringLiteral("90002")),
                run2, 2, QStringLiteral("course:90001"), QStringLiteral("course:90002"));
            QVERIFY2(r2.ok, qPrintable(r2.error));

            // 全部批次完成后才标记成功
            QVERIFY(repo->finishRun(run2, CrawlRun::Status::Success));
            const auto done = repo->findRun(run2);
            QVERIFY(done.has_value());
            QCOMPARE(done->status, CrawlRun::Status::Success);
            QVERIFY(done->finishedAt.has_value());

            // 两门课的数据都在
            QCOMPARE(repo->countAssignments(QStringLiteral("90001")), 3);
            QCOMPARE(repo->countAssignments(QStringLiteral("90002")), 3);
        }
    }

    void scenarioA_batchNumbersAreRecordedInOrder()
    {
        const QString path = nextDbPath();
        auto repo = std::make_unique<SqliteRepository>();
        QString error;
        QVERIFY2(repo->open(path, &error), qPrintable(error));

        const qint64 runId = repo->startRun(QStringLiteral("full-scan"), QStringLiteral("full"));
        QVERIFY(runId > 0);

        for (int batch = 1; batch <= 3; ++batch) {
            const QString courseId = QStringLiteral("9000%1").arg(batch);
            const BatchApplyResult r = repo->applyAssignmentSnapshot(
                courseId,
                assignmentsFrom(fixtures::homeworkActivitiesThree(), courseId),
                runId, batch, QStringLiteral("batch%1").arg(batch - 1),
                QStringLiteral("batch%1").arg(batch));
            QVERIFY2(r.ok, qPrintable(r.error));
        }

        const QList<CrawlBatch> batches = repo->listBatches(runId);
        QCOMPARE(batches.size(), 3);
        for (int i = 0; i < batches.size(); ++i) {
            QCOMPARE(batches.at(i).batchNo, i + 1);
            QCOMPARE(batches.at(i).status, CrawlBatch::Status::Committed);
            QVERIFY(batches.at(i).committedAt.has_value());
        }

        const auto last = repo->findLastCommittedBatch(runId);
        QVERIFY(last.has_value());
        QCOMPARE(last->batchNo, 3);
        QCOMPARE(last->cursorAfter, QStringLiteral("batch3"));
    }

    // ---- 场景 B：事务失败时数据与断点一起回滚 ------------------------------

    void scenarioB_rollbackRevertsDataAndCheckpoint()
    {
        const QString path = nextDbPath();
        auto repo = std::make_unique<SqliteRepository>();
        QString error;
        QVERIFY2(repo->open(path, &error), qPrintable(error));

        const QString courseId = QStringLiteral("90001");
        const qint64 runId = repo->startRun(QStringLiteral("full-scan"), QStringLiteral("full"));

        // 先有一次成功的写入，作为「事务之前」的基线
        QVERIFY(repo->applyAssignmentSnapshot(
                     courseId,
                     assignmentsFrom(fixtures::homeworkActivitiesThree(), courseId),
                     runId, 1, QStringLiteral("a"), QStringLiteral("b"))
                     .ok);

        const auto baselineState =
            repo->findSyncState(SqliteRepository::courseTaskKey(courseId));
        QVERIFY(baselineState.has_value());
        const QString baselineCursor = baselineState->cursor;

        // 显式事务中写入改动，然后回滚
        QVERIFY(repo->beginTransaction());
        const BatchApplyResult modified = repo->applyAssignmentSnapshot(
            courseId,
            assignmentsFrom(fixtures::homeworkActivitiesDeadlineChanged(), courseId),
            runId, 2, QStringLiteral("b"), QStringLiteral("c"));
        QVERIFY2(modified.ok, qPrintable(modified.error));
        QVERIFY(repo->rollback());

        // 数据回到基线：截止时间仍是原值，条数不变
        const auto after = repo->findAssignment(courseId, QStringLiteral("homework-70001"));
        QVERIFY(after.has_value());
        QCOMPARE(after->dueAt.require(),
                 QDateTime(QDate(2026, 9, 24), QTime(15, 59, 0), QTimeZone::UTC));

        // 断点也回到基线，绝不能出现「数据没写但断点前进」
        const auto rolledBackState =
            repo->findSyncState(SqliteRepository::courseTaskKey(courseId));
        QVERIFY(rolledBackState.has_value());
        QCOMPARE(rolledBackState->cursor, baselineCursor);

        // 批次 2 不应存在
        const QList<CrawlBatch> batches = repo->listBatches(runId);
        QCOMPARE(batches.size(), 1);
        QCOMPARE(batches.first().batchNo, 1);
    }

    void scenarioB_failedWriteLeavesNoPartialBatch()
    {
        const QString path = nextDbPath();
        auto repo = std::make_unique<SqliteRepository>();
        QString error;
        QVERIFY2(repo->open(path, &error), qPrintable(error));

        const qint64 runId = repo->startRun(QStringLiteral("full-scan"), QStringLiteral("full"));

        // 空 courseId 会让本次批次在写入前失败
        const BatchApplyResult bad = repo->applyAssignmentSnapshot(
            QString(), assignmentsFrom(fixtures::homeworkActivitiesThree(), QStringLiteral("x")),
            runId, 1, QStringLiteral("p"), QStringLiteral("q"));
        QVERIFY(!bad.ok);

        // 不得留下批次记录，也不得推进断点
        QVERIFY(repo->listBatches(runId).isEmpty());
        QVERIFY(!repo->inTransaction());
    }

    // ---- 场景 C：会话过期 ---------------------------------------------------

    void scenarioC_expiredSessionIsNotAnEmptyList()
    {
        FakePageBridge bridge;
        // 站点会话失效：返回登录页 HTML，且 HTTP 状态码仍是 200
        bridge.setResponse(QStringLiteral("/api/my-courses"), fixtures::loginPageHtml(), 200);

        NjuLmsAdapter adapter(&bridge);
        QSignalSpy expiredSpy(&adapter, &NjuLmsAdapter::sessionExpired);

        const auto check = runSync<NjuLmsAdapter::SessionCheck>(
            [&](auto cb) { adapter.checkSession(cb); });

        QVERIFY(!check.authenticated);
        QVERIFY(check.error.needsLogin());
        QCOMPARE(check.error.code, NjuLmsAdapter::ErrorCode::SessionExpired);
        QCOMPARE(check.courseCount, 0);
        QCOMPARE(expiredSpy.count(), 1);

        // 关键：绝不能把登录页解析成「0 门课程的合法列表」
        const auto discovered = runSync<NjuLmsAdapter::CourseListResult>(
            [&](auto cb) { adapter.discoverCourses(cb); });
        QVERIFY(discovered.courses.isEmpty());
        QVERIFY(!discovered.error.ok());
        QVERIFY(discovered.error.needsLogin());
    }

    void scenarioC_expiredSessionOnAssignmentFetchSignalsLogin()
    {
        FakePageBridge bridge;
        bridge.setResponse(QStringLiteral("/api/courses/90001/homework-activities"),
                           fixtures::loginPageHtml(), 200);

        NjuLmsAdapter adapter(&bridge);
        QSignalSpy expiredSpy(&adapter, &NjuLmsAdapter::sessionExpired);

        const auto res = runSync<NjuLmsAdapter::AssignmentListResult>(
            [&](auto cb) { adapter.fetchAssignments(QStringLiteral("90001"), cb); });

        QVERIFY(res.assignments.isEmpty());
        QVERIFY(res.error.needsLogin());
        QCOMPARE(expiredSpy.count(), 1);
        // 会话失效时不得声称快照完整
        QVERIFY(!res.snapshotComplete);
    }

    void scenarioC_genuinelyEmptyCourseListIsNotAnError()
    {
        // 与上面对照：可解析 JSON 且 courses 为空 → 合法状态，不是会话失效
        FakePageBridge bridge;
        bridge.setResponse(QStringLiteral("/api/my-courses"), fixtures::myCoursesEmpty(), 200);

        NjuLmsAdapter adapter(&bridge);
        QSignalSpy expiredSpy(&adapter, &NjuLmsAdapter::sessionExpired);

        const auto check = runSync<NjuLmsAdapter::SessionCheck>(
            [&](auto cb) { adapter.checkSession(cb); });

        QVERIFY(check.error.ok());
        QVERIFY(check.authenticated);
        QCOMPARE(check.courseCount, 0);
        QCOMPARE(expiredSpy.count(), 0);
    }

    void scenarioC_http401IsTreatedAsLoginRequired()
    {
        FakePageBridge bridge;
        bridge.setResponse(QStringLiteral("/api/my-courses"), QStringLiteral("{}"), 401);

        NjuLmsAdapter adapter(&bridge);
        QSignalSpy expiredSpy(&adapter, &NjuLmsAdapter::sessionExpired);

        const auto check = runSync<NjuLmsAdapter::SessionCheck>(
            [&](auto cb) { adapter.checkSession(cb); });

        QVERIFY(!check.authenticated);
        QVERIFY(check.error.needsLogin());
        QCOMPARE(expiredSpy.count(), 1);
    }

    // ---- 场景 D：网络失败 → 有限次重试 -------------------------------------

    void scenarioD_transportFailureIsRetriedThenReported()
    {
        FakePageBridge bridge;
        bridge.setTransportFailure(QStringLiteral("/api/my-courses"),
                                   QStringLiteral("simulated network outage"));

        NjuLmsAdapter adapter(&bridge);
        adapter.setMaxRetries(2);          // 1 次原始 + 2 次重试 = 3 次尝试
        adapter.setRequestIntervalMs(1);   // 测试中不真的等待

        QSignalSpy retrySpy(&adapter, &NjuLmsAdapter::retrying);
        QSignalSpy expiredSpy(&adapter, &NjuLmsAdapter::sessionExpired);

        const auto res = runSync<NjuLmsAdapter::CourseListResult>(
            [&](auto cb) { adapter.discoverCourses(cb); });

        QVERIFY(!res.error.ok());
        QCOMPARE(res.error.code, NjuLmsAdapter::ErrorCode::Transport);
        QVERIFY(res.error.retryable());
        // 传输失败不是登录问题，不得误报为需要重新登录
        QCOMPARE(expiredSpy.count(), 0);
        QVERIFY(retrySpy.count() >= 1);
        QCOMPARE(bridge.requestCount(), 3);
    }

    void scenarioD_serverErrorIsRetried()
    {
        FakePageBridge bridge;
        bridge.setResponse(QStringLiteral("/api/my-courses"),
                           QStringLiteral("upstream unavailable"), 503);

        NjuLmsAdapter adapter(&bridge);
        adapter.setMaxRetries(1);
        adapter.setRequestIntervalMs(1);

        const auto res = runSync<NjuLmsAdapter::CourseListResult>(
            [&](auto cb) { adapter.discoverCourses(cb); });

        QVERIFY(!res.error.ok());
        QCOMPARE(res.error.code, NjuLmsAdapter::ErrorCode::HttpStatus);
        QCOMPARE(res.error.httpStatus, 503);
        QCOMPARE(bridge.requestCount(), 2);
    }

    // ---- 场景 H：无法证明完整时不得推进断点 --------------------------------

    void scenarioH_truncatedPageMarksSnapshotIncomplete()
    {
        FakePageBridge bridge;
        // 服务端夹取了 page_size：只回 10 条，但 total=25
        bridge.setResponse(QStringLiteral("/api/courses/90001/homework-activities"),
                           fixtures::homeworkActivitiesTruncated(), 200);

        NjuLmsAdapter adapter(&bridge);
        adapter.setMaxRetries(0);

        const auto res = runSync<NjuLmsAdapter::AssignmentListResult>(
            [&](auto cb) { adapter.fetchAssignments(QStringLiteral("90001"), cb); });

        QVERIFY(res.error.ok());
        QCOMPARE(res.assignments.size(), 10);
        QCOMPARE(res.reportedTotal, 25);
        QVERIFY(res.paginated);
        // 关键：不确定是否完整 → 不得标记成功
        QVERIFY(!res.snapshotComplete);
    }

    void scenarioH_completeSnapshotIsProven()
    {
        FakePageBridge bridge;
        bridge.setResponse(QStringLiteral("/api/courses/90001/homework-activities"),
                           fixtures::homeworkActivitiesThree(), 200);

        NjuLmsAdapter adapter(&bridge);
        adapter.setMaxRetries(0);

        const auto res = runSync<NjuLmsAdapter::AssignmentListResult>(
            [&](auto cb) { adapter.fetchAssignments(QStringLiteral("90001"), cb); });

        QVERIFY(res.error.ok());
        QCOMPARE(res.assignments.size(), 3);
        QCOMPARE(res.reportedTotal, 3);
        QVERIFY(res.snapshotComplete);
    }

    void scenarioH_activitiesEndpointNeverClaimsCompleteness()
    {
        // 实测 /activities 不返回任何分页元数据 → 无法证明完整
        FakePageBridge bridge;
        bridge.setResponse(QStringLiteral("/api/courses/90001/activities"),
                           fixtures::activitiesMixed(), 200);

        NjuLmsAdapter adapter(&bridge);
        adapter.setMaxRetries(0);

        const auto res = runSync<NjuLmsAdapter::CourseTaskResult>(
            [&](auto cb) { adapter.fetchCourseTasks(QStringLiteral("90001"), cb); });

        QVERIFY(res.error.ok());
        QCOMPARE(res.homeworkCount, 1);
        QCOMPARE(res.skippedOtherTypes, 1);
        QVERIFY2(!res.snapshotComplete,
                 "no paging metadata means completeness cannot be proven");
    }

    // ---- 适配器请求形状 -----------------------------------------------------

    void adapter_sendsMeasuredPathsAndMethods()
    {
        FakePageBridge bridge;
        bridge.setResponse(QStringLiteral("/api/my-courses"), fixtures::myCoursesTwoCourses());
        bridge.setResponse(QStringLiteral("/api/courses/90001/homework-activities"),
                           fixtures::homeworkActivitiesThree());

        NjuLmsAdapter adapter(&bridge);
        adapter.setMaxRetries(0);

        runSync<NjuLmsAdapter::CourseListResult>([&](auto cb) { adapter.discoverCourses(cb); });
        runSync<NjuLmsAdapter::AssignmentListResult>(
            [&](auto cb) { adapter.fetchAssignments(QStringLiteral("90001"), cb); });

        QCOMPARE(bridge.requestCount(), 2);

        // 课程列表：POST /api/my-courses，body 为 {}
        QCOMPARE(bridge.receivedRequests().at(0).path, QStringLiteral("/api/my-courses"));
        QCOMPARE(bridge.receivedRequests().at(0).method, IPageBridge::Request::Method::Post);
        QCOMPARE(bridge.receivedRequests().at(0).jsonBody, QStringLiteral("{}"));

        // 作业列表：GET，且带 page/page_size 参数
        const IPageBridge::Request &second = bridge.receivedRequests().at(1);
        QCOMPARE(second.method, IPageBridge::Request::Method::Get);
        QVERIFY(second.path.startsWith(QStringLiteral("/api/courses/90001/homework-activities")));
        QVERIFY(second.path.contains(QStringLiteral("page=1")));
        QVERIFY(second.path.contains(QStringLiteral("page_size=")));
    }

    void adapter_unsupportedCallsDoNotFabricateData()
    {
        FakePageBridge bridge;
        NjuLmsAdapter adapter(&bridge);

        // 作业详情：没有经过验证的接口，必须明确返回 Unsupported 而不是编造 URL 与数据
        const auto detail = runSync<NjuLmsAdapter::AssignmentListResult>(
            [&](auto cb) {
                adapter.fetchAssignmentDetail(QStringLiteral("90001"), QStringLiteral("70001"), cb);
            });
        QCOMPARE(detail.error.code, NjuLmsAdapter::ErrorCode::Unsupported);
        QVERIFY(detail.assignments.isEmpty());
        QCOMPARE(bridge.requestCount(), 0);  // 未发出任何请求
    }

    void adapter_notReadyBridgeFailsGracefully()
    {
        FakePageBridge bridge;
        bridge.setReady(false);

        NjuLmsAdapter adapter(&bridge);
        const auto res = runSync<NjuLmsAdapter::CourseListResult>(
            [&](auto cb) { adapter.discoverCourses(cb); });

        QVERIFY(!res.error.ok());
        QCOMPARE(res.error.code, NjuLmsAdapter::ErrorCode::Transport);
        QCOMPARE(bridge.requestCount(), 0);
    }
};

QTEST_MAIN(ResumeTests)
#include "ResumeTests.moc"
