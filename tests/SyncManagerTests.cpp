#include "adapters/NjuLmsAdapter.h"
#include "repository/SqliteRepository.h"
#include "support/FakePageBridge.h"
#include "support/Fixtures.h"
#include "sync/SyncManager.h"

#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

using namespace njulms;
using namespace njulms::test;

/**
 * SyncManager 端到端测试。
 *
 * 通过 FakePageBridge 回放真实结构的响应，驱动完整的
 * 「会话检查 → 课程发现 → 逐课程采集 → 入库 → 断点 → 终态」流程，
 * 全程不需要网络、不需要真实账号、不需要 Qt WebEngine。
 *
 * 这里重点验证需求第四节场景 A/B/C/D 在**编排层**的表现，
 * 以及四种终态（成功 / 部分完成 / 失败 / 等待登录）的判定是否正确。
 */
class SyncManagerTests : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    int m_counter = 0;

    /// 跑事件循环直到条件成立或超时
    static bool waitFor(const std::function<bool()> &predicate, int timeoutMs = 5000)
    {
        QElapsedTimer timer;
        timer.start();
        while (!predicate()) {
            if (timer.elapsed() > timeoutMs) {
                return false;
            }
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }
        return true;
    }

    /// 组装一套完整的测试环境
    struct Harness
    {
        std::unique_ptr<SqliteRepository> repo;
        std::unique_ptr<FakePageBridge> bridge;
        std::unique_ptr<NjuLmsAdapter> adapter;
        std::unique_ptr<SyncManager> sync;
    };

    Harness makeHarness(const QString &suffix)
    {
        Harness h;
        h.repo = std::make_unique<SqliteRepository>();
        const QString path =
            QDir(m_dir.path()).filePath(QStringLiteral("sync%1_%2.db").arg(suffix).arg(++m_counter));
        QString error;
        const bool opened = h.repo->open(path, &error);
        Q_ASSERT_X(opened, "makeHarness", qPrintable(error));

        h.bridge = std::make_unique<FakePageBridge>();
        h.adapter = std::make_unique<NjuLmsAdapter>(h.bridge.get());
        h.adapter->setMaxRetries(0);
        h.adapter->setRequestIntervalMs(0);

        h.sync = std::make_unique<SyncManager>(h.repo.get(), h.adapter.get());
        SyncManager::Config cfg;
        cfg.pauseBetweenCoursesMs = 0;   // 测试中不需要限速等待
        cfg.abortAfterConsecutiveFailures = 0;
        h.sync->setConfig(cfg);
        return h;
    }

    /// 为两门课程注册标准响应
    static void registerTwoCourses(FakePageBridge &bridge)
    {
        bridge.setResponse(QStringLiteral("/api/my-courses"), fixtures::myCoursesTwoCourses());
        bridge.setResponse(QStringLiteral("/api/courses/90001/homework-activities"),
                           fixtures::homeworkActivitiesThree());
        bridge.setResponse(QStringLiteral("/api/courses/90002/homework-activities"),
                           fixtures::homeworkActivitiesThree());
    }

    /// 跑一次完整全量并断言成功（v2 快速路径需要先有「全量基线」）
    static void runFullScanToSuccess(Harness &h)
    {
        QSignalSpy finished(h.sync.get(), &SyncManager::runFinished);
        QVERIFY(h.sync->start(SyncManager::Mode::Full));
        QVERIFY(waitFor([&]() { return finished.count() > 0; }));
        QCOMPARE(finished.first().first().value<SyncManager::RunSummary>().outcome,
                 SyncManager::Outcome::Success);
    }

private slots:

    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        // 信号携带的自定义类型必须注册，否则 QSignalSpy 取不到值
        qRegisterMetaType<SyncManager::Mode>("njulms::SyncManager::Mode");
        qRegisterMetaType<SyncManager::Outcome>("njulms::SyncManager::Outcome");
        qRegisterMetaType<SyncManager::CourseOutcome>("njulms::SyncManager::CourseOutcome");
        qRegisterMetaType<SyncManager::RunSummary>("njulms::SyncManager::RunSummary");
        qRegisterMetaType<QList<ParseIssue>>("QList<njulms::ParseIssue>");
    }

    // ---- 正常全量采集 -------------------------------------------------------

    void fullScan_succeedsAndPersistsEverything()
    {
        Harness h = makeHarness(QStringLiteral("full"));
        registerTwoCourses(*h.bridge);

        QSignalSpy finished(h.sync.get(), &SyncManager::runFinished);
        QString error;
        QVERIFY2(h.sync->start(SyncManager::Mode::Full, &error), qPrintable(error));
        QVERIFY(waitFor([&]() { return finished.count() > 0; }));

        const auto summary =
            finished.first().first().value<SyncManager::RunSummary>();
        QCOMPARE(summary.outcome, SyncManager::Outcome::Success);
        QCOMPARE(summary.coursesTotal, 2);
        QCOMPARE(summary.coursesSucceeded, 2);
        QCOMPARE(summary.coursesFailed, 0);
        QCOMPARE(summary.assignmentsInserted, 6);   // 每门课 3 条
        QVERIFY(summary.runId > 0);

        // 数据库里确实有数据
        QCOMPARE(h.repo->countAssignments(QStringLiteral("90001")), 3);
        QCOMPARE(h.repo->countAssignments(QStringLiteral("90002")), 3);
        QCOMPARE(h.repo->listCourses().size(), 2);

        // 运行记录被正确收尾
        const auto run = h.repo->findRun(summary.runId);
        QVERIFY(run.has_value());
        QCOMPARE(run->status, CrawlRun::Status::Success);
        QVERIFY(run->finishedAt.has_value());

        // 全量完成后才把 full_scan_completed 标记为真
        const auto state =
            h.repo->findSyncState(SqliteRepository::courseDiscoveryTaskKey());
        QVERIFY(state.has_value());
        QVERIFY(state->fullScanCompleted);
    }

    void fullScan_isIdempotentAcrossRuns()
    {
        // 场景 E/A 的编排层验证：重复整轮同步不产生重复记录
        Harness h = makeHarness(QStringLiteral("idem"));
        registerTwoCourses(*h.bridge);

        for (int round = 0; round < 2; ++round) {
            QSignalSpy finished(h.sync.get(), &SyncManager::runFinished);
            QVERIFY(h.sync->start(SyncManager::Mode::Full));
            QVERIFY(waitFor([&]() { return finished.count() > 0; }));
            QCOMPARE(finished.first().first().value<SyncManager::RunSummary>().outcome,
                     SyncManager::Outcome::Success);
        }

        // 第二轮不应新增任何记录
        QCOMPARE(h.repo->countAssignments(QStringLiteral("90001")), 3);
        QCOMPARE(h.repo->countAssignments(QStringLiteral("90002")), 3);
        QCOMPARE(h.repo->listAssignments(QStringLiteral("90001"), true).size(), 3);
        QCOMPARE(h.repo->listAssignments(QStringLiteral("90002"), true).size(), 3);
    }

    void incremental_updatesChangedDeadline()
    {
        Harness h = makeHarness(QStringLiteral("incr"));
        registerTwoCourses(*h.bridge);

        {
            QSignalSpy finished(h.sync.get(), &SyncManager::runFinished);
            QVERIFY(h.sync->start(SyncManager::Mode::Full));
            QVERIFY(waitFor([&]() { return finished.count() > 0; }));
        }

        // 站点侧把课程 90001 的作业截止时间改了，且只剩这一条作业
        h.bridge->setResponse(QStringLiteral("/api/courses/90001/homework-activities"),
                              fixtures::homeworkActivitiesDeadlineChanged());

        QSignalSpy finished(h.sync.get(), &SyncManager::runFinished);
        QVERIFY(h.sync->start(SyncManager::Mode::Incremental));
        QVERIFY(waitFor([&]() { return finished.count() > 0; }));

        const auto summary = finished.first().first().value<SyncManager::RunSummary>();
        QCOMPARE(summary.outcome, SyncManager::Outcome::Success);
        QCOMPARE(summary.assignmentsUpdated, 1);   // 只有 90001 的那条变化
        QCOMPARE(summary.assignmentsInserted, 0);

        // 更新的是原记录，不产生重复
        const auto changed = h.repo->findAssignment(QStringLiteral("90001"),
                                                    QStringLiteral("homework-70001"));
        QVERIFY(changed.has_value());
        QCOMPARE(changed->dueAt.require(),
                 QDateTime(QDate(2026, 10, 15), QTime(15, 59, 0), QTimeZone::UTC));
        QCOMPARE(h.repo->countAssignments(QStringLiteral("90001")), 1);

        // 消失的两条被标记为 missing，而不是删除
        QCOMPARE(h.repo->listAssignments(QStringLiteral("90001"), true).size(), 3);
    }

    // ---- 场景 C：会话失效 ---------------------------------------------------

    void sessionExpired_yieldsWaitingForLoginAndKeepsData()
    {
        // 先成功跑一轮，留下基线数据
        Harness h = makeHarness(QStringLiteral("expire"));
        registerTwoCourses(*h.bridge);

        {
            QSignalSpy finished(h.sync.get(), &SyncManager::runFinished);
            QVERIFY(h.sync->start(SyncManager::Mode::Full));
            QVERIFY(waitFor([&]() { return finished.count() > 0; }));
        }
        const int before = h.repo->countAssignments(QStringLiteral("90001"));
        QCOMPARE(before, 3);

        // 会话失效：登录页 HTML + 200
        h.bridge->setResponse(QStringLiteral("/api/my-courses"), fixtures::loginPageHtml(), 200);

        QSignalSpy finished(h.sync.get(), &SyncManager::runFinished);
        QSignalSpy loginSpy(h.sync.get(), &SyncManager::loginRequired);

        QVERIFY(h.sync->start(SyncManager::Mode::Incremental));
        QVERIFY(waitFor([&]() { return finished.count() > 0; }));

        const auto summary = finished.first().first().value<SyncManager::RunSummary>();
        QCOMPARE(summary.outcome, SyncManager::Outcome::WaitingForLogin);
        QVERIFY(loginSpy.count() >= 1);

        // 终态必须记为 need_login 而不是普通 failed
        const auto run = h.repo->findRun(summary.runId);
        QVERIFY(run.has_value());
        QCOMPARE(run->status, CrawlRun::Status::NeedLogin);

        // 关键：库中数据完全没有被破坏（登录页没有被解析成空作业列表）
        QCOMPARE(h.repo->countAssignments(QStringLiteral("90001")), before);
        QCOMPARE(h.repo->listCourses().size(), 2);

        // 不得因为会话失效就把 full_scan_completed 标记为完成/未完成
        const auto state = h.repo->findSyncState(SqliteRepository::courseDiscoveryTaskKey());
        QVERIFY(state.has_value());
        QVERIFY(state->fullScanCompleted);   // 仍是上一轮成功时的结论
    }

    void sessionExpiredDuringCourseStopsRemainingCourses()
    {
        Harness h = makeHarness(QStringLiteral("expire-mid"));
        h.bridge->setResponse(QStringLiteral("/api/my-courses"), fixtures::myCoursesTwoCourses());
        // 第一门课正常，第二门课会话失效
        h.bridge->setResponse(QStringLiteral("/api/courses/90001/homework-activities"),
                              fixtures::homeworkActivitiesThree());
        h.bridge->setResponse(QStringLiteral("/api/courses/90002/homework-activities"),
                              fixtures::loginPageHtml(), 200);

        QSignalSpy finished(h.sync.get(), &SyncManager::runFinished);
        QVERIFY(h.sync->start(SyncManager::Mode::Full));
        QVERIFY(waitFor([&]() { return finished.count() > 0; }));

        const auto summary = finished.first().first().value<SyncManager::RunSummary>();
        QCOMPARE(summary.outcome, SyncManager::Outcome::WaitingForLogin);

        // 第一门课的数据已经提交（不会因为后面的失败而被丢弃）
        QCOMPARE(h.repo->countAssignments(QStringLiteral("90001")), 3);
        // 不能把未完成的整轮标记为全量完成
        const auto state = h.repo->findSyncState(SqliteRepository::courseDiscoveryTaskKey());
        QVERIFY(state.has_value());
        QVERIFY(!state->fullScanCompleted);
    }

    // ---- 部分完成 / 失败 ----------------------------------------------------

    void partialOutcomeWhenSomeCoursesFail()
    {
        Harness h = makeHarness(QStringLiteral("partial"));
        h.bridge->setResponse(QStringLiteral("/api/my-courses"), fixtures::myCoursesTwoCourses());
        h.bridge->setResponse(QStringLiteral("/api/courses/90001/homework-activities"),
                              fixtures::homeworkActivitiesThree());
        // 第二门课返回 500（可重试但重试次数为 0）
        h.bridge->setResponse(QStringLiteral("/api/courses/90002/homework-activities"),
                              QStringLiteral("server error"), 500);

        QSignalSpy finished(h.sync.get(), &SyncManager::runFinished);
        QVERIFY(h.sync->start(SyncManager::Mode::Full));
        QVERIFY(waitFor([&]() { return finished.count() > 0; }));

        const auto summary = finished.first().first().value<SyncManager::RunSummary>();
        QCOMPARE(summary.outcome, SyncManager::Outcome::Partial);
        QCOMPARE(summary.coursesSucceeded, 1);
        QCOMPARE(summary.coursesFailed, 1);
        QCOMPARE(summary.assignmentsInserted, 3);

        const auto run = h.repo->findRun(summary.runId);
        QVERIFY(run.has_value());
        QCOMPARE(run->status, CrawlRun::Status::Partial);

        // 部分完成时不得声称全量扫描已完成
        const auto state = h.repo->findSyncState(SqliteRepository::courseDiscoveryTaskKey());
        QVERIFY(state.has_value());
        QVERIFY(!state->fullScanCompleted);
    }

    void failedOutcomeWhenAllCoursesFail()
    {
        Harness h = makeHarness(QStringLiteral("allfail"));
        h.bridge->setResponse(QStringLiteral("/api/my-courses"), fixtures::myCoursesTwoCourses());
        h.bridge->setTransportFailure(QStringLiteral("/api/courses/90001/homework-activities"),
                                      QStringLiteral("network down"));
        h.bridge->setTransportFailure(QStringLiteral("/api/courses/90002/homework-activities"),
                                      QStringLiteral("network down"));

        QSignalSpy finished(h.sync.get(), &SyncManager::runFinished);
        QVERIFY(h.sync->start(SyncManager::Mode::Full));
        QVERIFY(waitFor([&]() { return finished.count() > 0; }));

        const auto summary = finished.first().first().value<SyncManager::RunSummary>();
        QCOMPARE(summary.outcome, SyncManager::Outcome::Failed);
        QCOMPARE(summary.coursesSucceeded, 0);
        QCOMPARE(summary.coursesFailed, 2);

        // 课程列表本身已经入库（发现的课程不因采集失败而丢失）
        QCOMPARE(h.repo->listCourses().size(), 2);
    }

    void abortAfterConsecutiveFailures()
    {
        Harness h = makeHarness(QStringLiteral("abort"));
        SyncManager::Config cfg;
        cfg.pauseBetweenCoursesMs = 0;
        cfg.abortAfterConsecutiveFailures = 2;
        h.sync->setConfig(cfg);

        h.bridge->setResponse(QStringLiteral("/api/my-courses"), fixtures::myCoursesTwoCourses());
        h.bridge->setTransportFailure(QStringLiteral("/api/courses/90001/homework-activities"),
                                      QStringLiteral("down"));
        h.bridge->setTransportFailure(QStringLiteral("/api/courses/90002/homework-activities"),
                                      QStringLiteral("down"));

        QSignalSpy finished(h.sync.get(), &SyncManager::runFinished);
        QVERIFY(h.sync->start(SyncManager::Mode::Full));
        QVERIFY(waitFor([&]() { return finished.count() > 0; }));

        const auto summary = finished.first().first().value<SyncManager::RunSummary>();
        QCOMPARE(summary.outcome, SyncManager::Outcome::Failed);
        QCOMPARE(summary.coursesFailed, 2);
        QVERIFY(summary.message.contains(QStringLiteral("连续")));
    }

    // ---- 空课程列表 ---------------------------------------------------------

    void emptyCourseListIsSuccess()
    {
        Harness h = makeHarness(QStringLiteral("empty"));
        h.bridge->setResponse(QStringLiteral("/api/my-courses"), fixtures::myCoursesEmpty());

        QSignalSpy finished(h.sync.get(), &SyncManager::runFinished);
        QVERIFY(h.sync->start(SyncManager::Mode::Full));
        QVERIFY(waitFor([&]() { return finished.count() > 0; }));

        const auto summary = finished.first().first().value<SyncManager::RunSummary>();
        // 「一门课都没有」是合法状态，不是错误
        QCOMPARE(summary.outcome, SyncManager::Outcome::Success);
        QCOMPARE(summary.coursesTotal, 0);
        QCOMPARE(h.repo->listCourses().size(), 0);
    }

    // ---- 并发保护 -----------------------------------------------------------

    void startRejectedWhileRunning()
    {
        Harness h = makeHarness(QStringLiteral("concurrent"));
        registerTwoCourses(*h.bridge);

        // 需要让第一轮同步「停在半途」，才能验证并发拒绝。
        // FakePageBridge 是同步回调，一次 start() 会在返回前跑完整轮，
        // 因此这里让某门课程返回 503 并允许重试，借重试的 QTimer 延迟
        // 制造出「任务仍在运行」的窗口。
        h.adapter->setMaxRetries(1);
        h.adapter->setRequestIntervalMs(0);
        h.bridge->setResponse(QStringLiteral("/api/courses/90002/homework-activities"),
                              QStringLiteral("server error"), 503);

        QSignalSpy finished(h.sync.get(), &SyncManager::runFinished);
        QVERIFY(h.sync->start(SyncManager::Mode::Full));

        // 此刻第一门课已处理完，第二门课正在等待重试定时器 → 任务仍在运行
        QVERIFY2(h.sync->isRunning(),
                 "sync must still be running while a course retry is pending");

        // 需求：不要同时启动多个相同的采集任务
        QString error;
        QVERIFY(!h.sync->start(SyncManager::Mode::Incremental, &error));
        QVERIFY(error.contains(QStringLiteral("already running")));

        QVERIFY(waitFor([&]() { return finished.count() > 0; }));
        QVERIFY(!h.sync->isRunning());
    }

    void startRejectedWhenDatabaseClosed()
    {
        auto repo = std::make_unique<SqliteRepository>();  // 未 open
        FakePageBridge bridge;
        NjuLmsAdapter adapter(&bridge);
        SyncManager sync(repo.get(), &adapter);

        QString error;
        QVERIFY(!sync.start(SyncManager::Mode::Full, &error));
        QVERIFY(error.contains(QStringLiteral("not open")));
        QVERIFY(!sync.isRunning());
    }

    // ---- 断点与批次的编排层表现 --------------------------------------------

    void eachCourseProducesOneCommittedBatch()
    {
        Harness h = makeHarness(QStringLiteral("batches"));
        registerTwoCourses(*h.bridge);

        QSignalSpy finished(h.sync.get(), &SyncManager::runFinished);
        QVERIFY(h.sync->start(SyncManager::Mode::Full));
        QVERIFY(waitFor([&]() { return finished.count() > 0; }));

        const auto summary = finished.first().first().value<SyncManager::RunSummary>();
        const QList<CrawlBatch> batches = h.repo->listBatches(summary.runId);
        QCOMPARE(batches.size(), 2);
        for (const CrawlBatch &b : batches) {
            QCOMPARE(b.status, CrawlBatch::Status::Committed);
            QVERIFY(b.committedAt.has_value());
        }

        // 每门课程都有自己的断点记录
        QVERIFY(h.repo->findSyncState(SqliteRepository::courseTaskKey(QStringLiteral("90001")))
                    .has_value());
        QVERIFY(h.repo->findSyncState(SqliteRepository::courseTaskKey(QStringLiteral("90002")))
                    .has_value());
    }

    // ---- 场景 F 在编排层的可观测性 -----------------------------------------

    void preservedFieldsAreReportedPerCourse()
    {
        // 第一轮：完整数据
        Harness h = makeHarness(QStringLiteral("preserve"));
        h.bridge->setResponse(QStringLiteral("/api/my-courses"), fixtures::myCoursesTwoCourses());
        h.bridge->setResponse(QStringLiteral("/api/courses/90001/homework-activities"),
                              fixtures::homeworkActivitiesThree());
        h.bridge->setResponse(QStringLiteral("/api/courses/90002/homework-activities"),
                              fixtures::homeworkActivitiesEmpty());

        {
            QSignalSpy finished(h.sync.get(), &SyncManager::runFinished);
            QVERIFY(h.sync->start(SyncManager::Mode::Full));
            QVERIFY(waitFor([&]() { return finished.count() > 0; }));
        }

        // 第二轮：90001 的标题/截止时间整个消失
        h.bridge->setResponse(QStringLiteral("/api/courses/90001/homework-activities"),
                              fixtures::homeworkActivitiesFieldsDropped());

        QSignalSpy courseFinished(h.sync.get(), &SyncManager::courseFinished);
        QVERIFY(h.sync->start(SyncManager::Mode::Incremental));
        QVERIFY(waitFor([&]() { return !h.sync->isRunning(); }));

        // 找到 90001 的那条 outcome，必须报告被保护的字段
        std::optional<SyncManager::CourseOutcome> target;
        for (const QList<QVariant> &args : courseFinished) {
            const auto outcome = args.first().value<SyncManager::CourseOutcome>();
            if (outcome.courseId == QLatin1String("90001")) {
                target = outcome;
            }
        }
        QVERIFY(target.has_value());
        QVERIFY(target->ok);
        QVERIFY(target->preservedFields.contains(QStringLiteral("title")));
        QVERIFY(target->preservedFields.contains(QStringLiteral("dueAt")));

        // 库中有效值仍在
        const auto kept = h.repo->findAssignment(QStringLiteral("90001"),
                                                 QStringLiteral("homework-70001"));
        QVERIFY(kept.has_value());
        QVERIFY(kept->title.hasValue());
        QCOMPARE(kept->title.require(), QStringLiteral("测试作业一"));
    }

    // ---- 暂停 / 取消 --------------------------------------------------------

    void cancelKeepsCommittedWork()
    {
        Harness h = makeHarness(QStringLiteral("cancel"));
        registerTwoCourses(*h.bridge);

        // 与 startRejectedWhileRunning 同理：需要制造一个「任务仍在运行」的窗口
        // 才能测试取消。让第二门课返回 503 并允许重试，用重试延迟形成窗口。
        h.adapter->setMaxRetries(1);
        h.adapter->setRequestIntervalMs(0);
        h.bridge->setResponse(QStringLiteral("/api/courses/90002/homework-activities"),
                              QStringLiteral("server error"), 503);

        QSignalSpy finished(h.sync.get(), &SyncManager::runFinished);
        QVERIFY(h.sync->start(SyncManager::Mode::Full));
        QVERIFY(h.sync->isRunning());

        h.sync->cancel();
        QVERIFY(waitFor([&]() { return finished.count() > 0; }));

        const auto summary = finished.first().first().value<SyncManager::RunSummary>();
        QCOMPARE(summary.outcome, SyncManager::Outcome::Cancelled);

        const auto run = h.repo->findRun(summary.runId);
        QVERIFY(run.has_value());
        QCOMPARE(run->status, CrawlRun::Status::Cancelled);
        // 取消不得标记全量完成
        const auto state = h.repo->findSyncState(SqliteRepository::courseDiscoveryTaskKey());
        if (state.has_value()) {
            QVERIFY(!state->fullScanCompleted);
        }
    }

    void cancelOnIdleIsHarmless()
    {
        Harness h = makeHarness(QStringLiteral("idlecancel"));
        h.sync->cancel();
        h.sync->pause();
        h.sync->resume();
        QVERIFY(!h.sync->isRunning());
        QCOMPARE(h.sync->currentCourseIndex(), 0);
        QVERIFY(h.sync->currentCourseName().isEmpty());
    }

    // ---- 暂停 / 继续（此前只测了取消）--------------------------------------

    void pauseThenResumeCompletesTheRun()
    {
        // 需求：暂停后「当前课程完成、后续课程不再开始」，继续后能接着做完。
        Harness h = makeHarness(QStringLiteral("pauseresume"));
        registerTwoCourses(*h.bridge);

        // 课程之间必须存在一个真实的异步窗口，pause() 才可能生效：
        // makeHarness 默认 pauseBetweenCoursesMs = 0，而 FakePageBridge 是同步回调，
        // 那样整轮同步会在 start() 返回前跑完，pause() 只会落在终态之后变成空操作。
        // 真实运行时桥是异步的，这里用 30ms 课程间隔模拟同一语义。
        SyncManager::Config cfg;
        cfg.pauseBetweenCoursesMs = 30;
        cfg.abortAfterConsecutiveFailures = 0;
        h.sync->setConfig(cfg);

        QSignalSpy finished(h.sync.get(), &SyncManager::runFinished);
        QSignalSpy courseFinished(h.sync.get(), &SyncManager::courseFinished);

        QVERIFY(h.sync->start(SyncManager::Mode::Full));

        // 第一门课已处理完（首轮同步是同步回调），此刻请求暂停。
        // 下一门课不会开始，任务保持运行态等待 resume()。
        h.sync->pause();
        QVERIFY(waitFor([&]() { return h.sync->isPaused(); }));
        QVERIFY2(h.sync->isRunning(), "pause is not a terminal state");
        QVERIFY2(finished.count() == 0, "pause must not finish the run");
        QCOMPARE(courseFinished.count(), 1);   // 只有第一门课完成
        QCOMPARE(h.repo->countAssignments(QStringLiteral("90001")), 3);
        QCOMPARE(h.repo->countAssignments(QStringLiteral("90002")), 0);

        // 继续：剩余课程被处理完，并以成功收尾
        h.sync->resume();
        QVERIFY(waitFor([&]() { return finished.count() > 0; }));

        const auto summary = finished.first().first().value<SyncManager::RunSummary>();
        QCOMPARE(summary.outcome, SyncManager::Outcome::Success);
        QCOMPARE(summary.coursesSucceeded, 2);
        QCOMPARE(h.repo->countAssignments(QStringLiteral("90002")), 3);
        QVERIFY(!h.sync->isRunning());
    }

    void pauseOnIdleIsHarmlessAndDoesNotFinishAnything()
    {
        Harness h = makeHarness(QStringLiteral("pausidle"));
        QSignalSpy finished(h.sync.get(), &SyncManager::runFinished);

        h.sync->pause();
        h.sync->resume();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);

        QVERIFY(!h.sync->isRunning());
        QCOMPARE(finished.count(), 0);
    }

    void resumeWithoutPauseIsIgnored()
    {
        Harness h = makeHarness(QStringLiteral("resumenopause"));
        registerTwoCourses(*h.bridge);

        QSignalSpy finished(h.sync.get(), &SyncManager::runFinished);
        QVERIFY(h.sync->start(SyncManager::Mode::Full));
        h.sync->resume();   // 没有暂停过，应当是空操作

        QVERIFY(waitFor([&]() { return finished.count() > 0; }));
        QCOMPARE(finished.first().first().value<SyncManager::RunSummary>().outcome,
                 SyncManager::Outcome::Success);
    }

    void pausedRunKeepsCheckpointsForRecovery()
    {
        // 暂停不是失败：已提交批次的断点必须保留，供下次恢复使用
        Harness h = makeHarness(QStringLiteral("pauseckpt"));
        registerTwoCourses(*h.bridge);

        // 同上：留出课程之间的异步窗口，才能观察到「暂停中」这个中间态。
        SyncManager::Config cfg;
        cfg.pauseBetweenCoursesMs = 30;
        cfg.abortAfterConsecutiveFailures = 0;
        h.sync->setConfig(cfg);

        QSignalSpy courseFinished(h.sync.get(), &SyncManager::courseFinished);
        QVERIFY(h.sync->start(SyncManager::Mode::Full));
        h.sync->pause();
        QVERIFY(waitFor([&]() { return h.sync->isPaused(); }));

        // 已完成的课程留下了批次记录与断点
        const auto state =
            h.repo->findSyncState(SqliteRepository::courseTaskKey(QStringLiteral("90001")));
        QVERIFY2(state.has_value(), "a paused run must keep the checkpoint of finished courses");
        QVERIFY(state->fullScanCompleted);

        // 未开始的课程没有断点
        QVERIFY(!h.repo->findSyncState(SqliteRepository::courseTaskKey(QStringLiteral("90002")))
                     .has_value());

        // 不得因为暂停就把整轮标记为全量完成
        const auto discovery =
            h.repo->findSyncState(SqliteRepository::courseDiscoveryTaskKey());
        if (discovery.has_value()) {
            QVERIFY(!discovery->fullScanCompleted);
        }

        h.sync->resume();
        QVERIFY(waitFor([&]() { return !h.sync->isRunning(); }));
    }

    // ---- 作业数量与解析异常的统计 -------------------------------------------

    void parseIssuesAreCountedAndReported()
    {
        Harness h = makeHarness(QStringLiteral("issues"));
        h.bridge->setResponse(QStringLiteral("/api/my-courses"), fixtures::myCoursesTwoCourses());
        h.bridge->setResponse(QStringLiteral("/api/courses/90001/homework-activities"),
                              fixtures::homeworkActivitiesBadTime());
        h.bridge->setResponse(QStringLiteral("/api/courses/90002/homework-activities"),
                              fixtures::homeworkActivitiesEmpty());

        QSignalSpy issuesSpy(h.sync.get(), &SyncManager::parseIssuesReported);
        QSignalSpy finished(h.sync.get(), &SyncManager::runFinished);

        QVERIFY(h.sync->start(SyncManager::Mode::Full));
        QVERIFY(waitFor([&]() { return finished.count() > 0; }));

        QVERIFY(issuesSpy.count() >= 1);
        const auto summary = finished.first().first().value<SyncManager::RunSummary>();
        QVERIFY(summary.parseIssueCount >= 1);

        // 解析异常必须落库，便于之后重试
        const QList<ParseErrorRecord> errors = h.repo->listRecentParseErrors();
        QVERIFY(!errors.isEmpty());
        QCOMPARE(errors.first().field, QStringLiteral("assignment.deadline"));
    }

    void incompleteSnapshotIsCounted()
    {
        Harness h = makeHarness(QStringLiteral("incomplete"));
        h.bridge->setResponse(QStringLiteral("/api/my-courses"), fixtures::myCoursesTwoCourses());
        // 服务端夹取 page_size：只回 10 条但 total=25 → 无法证明完整
        h.bridge->setResponse(QStringLiteral("/api/courses/90001/homework-activities"),
                              fixtures::homeworkActivitiesTruncated());
        h.bridge->setResponse(QStringLiteral("/api/courses/90002/homework-activities"),
                              fixtures::homeworkActivitiesEmpty());

        QSignalSpy finished(h.sync.get(), &SyncManager::runFinished);
        QVERIFY(h.sync->start(SyncManager::Mode::Full));
        QVERIFY(waitFor([&]() { return finished.count() > 0; }));

        const auto summary = finished.first().first().value<SyncManager::RunSummary>();
        // 仍然采集入库（保证不漏），但必须如实报告「无法证明完整」
        QCOMPARE(summary.coursesIncomplete, 1);
        QCOMPARE(h.repo->countAssignments(QStringLiteral("90001")), 10);
    }

    /// v2：快照不完整时不得误软删除已有作业，整轮降级为 partial
    void incompleteSnapshotDoesNotSoftDeleteExistingAssignments()
    {
        Harness h = makeHarness(QStringLiteral("incomplete-v2"));
        registerTwoCourses(*h.bridge);

        {
            QSignalSpy finished(h.sync.get(), &SyncManager::runFinished);
            QVERIFY(h.sync->start(SyncManager::Mode::Full));
            QVERIFY(waitFor([&]() { return finished.count() > 0; }));
            QCOMPARE(finished.first().first().value<SyncManager::RunSummary>().outcome,
                     SyncManager::Outcome::Success);
        }
        QCOMPARE(h.repo->countAssignments(QStringLiteral("90001")), 3);

        // 第二轮：90001 返回被服务端夹取的分页响应（10 条 / total=25）→ 无法证明完整
        h.bridge->setResponse(QStringLiteral("/api/courses/90001/homework-activities"),
                              fixtures::homeworkActivitiesTruncated());

        QSignalSpy finished(h.sync.get(), &SyncManager::runFinished);
        QVERIFY(h.sync->start(SyncManager::Mode::Incremental));
        QVERIFY(waitFor([&]() { return finished.count() > 0; }));

        const auto summary = finished.first().first().value<SyncManager::RunSummary>();
        // 出现不完整快照 → 整轮不能算成功
        QCOMPARE(summary.outcome, SyncManager::Outcome::Partial);
        QCOMPARE(summary.coursesIncomplete, 1);

        // 本轮取到的 10 条照常入库（保证不漏），但原有的 3 条**绝不能**被误判成「消失」：
        // 所以总数 = 3（原有，仍在场）+ 10（本轮新取到）
        QCOMPARE(h.repo->countAssignments(QStringLiteral("90001")), 13);
        for (const QString &sourceId :
             {QStringLiteral("homework-70001"), QStringLiteral("homework-70002"),
              QStringLiteral("homework-70003")}) {
            const auto existing = h.repo->findAssignment(QStringLiteral("90001"), sourceId);
            QVERIFY2(existing.has_value(), qPrintable(sourceId));
            QVERIFY2(existing->present, qPrintable(sourceId));   // present 仍为 1
        }

        // 关键：这门课的断点不得被标记完成
        const auto state =
            h.repo->findSyncState(SqliteRepository::courseTaskKey(QStringLiteral("90001")));
        QVERIFY(state.has_value());
        QVERIFY(!state->fullScanCompleted);
    }

    // ---- v2：快速增量（用 /api/todos 当变更信号）-----------------------------

    /// 待办没有变化 → 常态只发 2 个请求，全部课程跳过
    void quickIncrementalSkipsUnchangedCourses()
    {
        Harness h = makeHarness(QStringLiteral("quick-skip"));
        registerTwoCourses(*h.bridge);
        runFullScanToSuccess(h);

        h.bridge->setResponse(QStringLiteral("/api/todos"), fixtures::todosMatchingHomeworkThree());
        h.bridge->clearReceived();

        QSignalSpy finished(h.sync.get(), &SyncManager::runFinished);
        QVERIFY(h.sync->start(SyncManager::Mode::Incremental));
        QVERIFY(waitFor([&]() { return finished.count() > 0; }));

        const auto summary = finished.first().first().value<SyncManager::RunSummary>();
        QCOMPARE(summary.outcome, SyncManager::Outcome::Success);
        QVERIFY(summary.quickPath);
        QCOMPARE(summary.coursesTotal, 2);
        QCOMPARE(summary.coursesSkipped, 2);
        QCOMPARE(summary.coursesSucceeded, 0);

        // 关键：没有对任何课程发作业请求（只有 my-courses + todos 两个请求）
        for (const IPageBridge::Request &req : h.bridge->receivedRequests()) {
            QVERIFY2(!req.path.contains(QLatin1String("homework-activities")),
                     qPrintable(req.path));
        }
        QCOMPARE(h.repo->countAssignments(QStringLiteral("90001")), 3);
    }

    /// 只有待办里出现变化的课程才深扫（另一门课保持跳过）
    ///
    /// 回归（2026-10-10 用户报的 bug）：快速路径的状态必须**每轮开始前清空**。
    /// 曾经的现象：跑过一次"待办无变化"的增量之后，再点「开始全量采集」，
    /// 因为 quickDiffActive / quickChangedCourses 还留着上一轮的值，
    /// 全量会把所有课程都跳过，摘要还显示"快速检查完成：N 门课程均无变化"。
    void fullScanAfterQuickIncrementalStillFetchesEveryCourse()
    {
        Harness h = makeHarness(QStringLiteral("full-after-quick"));
        registerTwoCourses(*h.bridge);
        runFullScanToSuccess(h);   // 建立全量基线（快速路径的前提）

        // 第一轮：增量 + 快速路径，待办无变化 → 两门课都被跳过
        h.bridge->setResponse(QStringLiteral("/api/todos"), fixtures::todosMatchingHomeworkThree());
        h.bridge->clearReceived();
        {
            QSignalSpy finished(h.sync.get(), &SyncManager::runFinished);
            QVERIFY(h.sync->start(SyncManager::Mode::Incremental));
            QVERIFY(waitFor([&]() { return finished.count() > 0; }));
            const auto summary = finished.first().first().value<SyncManager::RunSummary>();
            QVERIFY(summary.quickPath);
            QCOMPARE(summary.coursesSkipped, 2);
        }

        // 第二轮：全量采集 —— 必须真的重扫每一门课，不允许有任何跳过
        h.bridge->clearReceived();
        QSignalSpy finished(h.sync.get(), &SyncManager::runFinished);
        QVERIFY(h.sync->start(SyncManager::Mode::Full));
        QVERIFY(waitFor([&]() { return finished.count() > 0; }));

        const auto summary = finished.first().first().value<SyncManager::RunSummary>();
        QCOMPARE(summary.outcome, SyncManager::Outcome::Success);
        QVERIFY(!summary.quickPath);
        QCOMPARE(summary.coursesSkipped, 0);
        QCOMPARE(summary.coursesSucceeded, 2);

        int homeworkRequests = 0;
        for (const IPageBridge::Request &req : h.bridge->receivedRequests()) {
            if (req.path.contains(QLatin1String("homework-activities"))) {
                ++homeworkRequests;
            }
        }
        QCOMPARE(homeworkRequests, 2);   // 两门课都发了作业请求
    }

    void quickIncrementalDeepScansOnlyChangedCourse()
    {
        Harness h = makeHarness(QStringLiteral("quick-changed"));
        registerTwoCourses(*h.bridge);
        runFullScanToSuccess(h);

        // 90001 的某条待办截止时间被改了 → 该课程需要深扫
        h.bridge->setResponse(QStringLiteral("/api/todos"),
                              fixtures::todosResponse({
                                  {QStringLiteral("90001"), QStringLiteral("70001"),
                                   QStringLiteral("2026-10-15T15:59:00Z")},
                                  {QStringLiteral("90001"), QStringLiteral("70002"),
                                   QStringLiteral("2026-10-01T15:59:00Z")},
                                  {QStringLiteral("90001"), QStringLiteral("70003"),
                                   QStringLiteral("2026-10-08T15:59:00Z")},
                                  {QStringLiteral("90002"), QStringLiteral("70001"),
                                   QStringLiteral("2026-09-24T15:59:00Z")},
                                  {QStringLiteral("90002"), QStringLiteral("70002"),
                                   QStringLiteral("2026-10-01T15:59:00Z")},
                                  {QStringLiteral("90002"), QStringLiteral("70003"),
                                   QStringLiteral("2026-10-08T15:59:00Z")},
                              }));
        h.bridge->clearReceived();

        QSignalSpy finished(h.sync.get(), &SyncManager::runFinished);
        QVERIFY(h.sync->start(SyncManager::Mode::Incremental));
        QVERIFY(waitFor([&]() { return finished.count() > 0; }));

        const auto summary = finished.first().first().value<SyncManager::RunSummary>();
        QCOMPARE(summary.outcome, SyncManager::Outcome::Success);
        QVERIFY(summary.quickPath);
        QCOMPARE(summary.coursesSkipped, 1);
        QCOMPARE(summary.coursesSucceeded, 1);

        int scanned90001 = 0;
        int scanned90002 = 0;
        for (const IPageBridge::Request &req : h.bridge->receivedRequests()) {
            if (req.path.contains(QLatin1String("/api/courses/90001/homework-activities"))) {
                ++scanned90001;
            }
            if (req.path.contains(QLatin1String("/api/courses/90002/homework-activities"))) {
                ++scanned90002;
            }
        }
        QCOMPARE(scanned90001, 1);
        QCOMPARE(scanned90002, 0);
    }

    /// 待办"消失"（例如已提交）也要重扫对应课程
    void quickIncrementalDetectsDisappearedTodo()
    {
        Harness h = makeHarness(QStringLiteral("quick-disappear"));
        registerTwoCourses(*h.bridge);
        runFullScanToSuccess(h);

        // 第一轮快速检查：待办齐全 → 建立快照（全部跳过）
        h.bridge->setResponse(QStringLiteral("/api/todos"), fixtures::todosMatchingHomeworkThree());
        {
            QSignalSpy finished(h.sync.get(), &SyncManager::runFinished);
            QVERIFY(h.sync->start(SyncManager::Mode::Incremental));
            QVERIFY(waitFor([&]() { return finished.count() > 0; }));
            QCOMPARE(finished.first().first().value<SyncManager::RunSummary>().coursesSkipped, 2);
        }
        QVERIFY(h.repo->findSyncState(SqliteRepository::todoSnapshotTaskKey()).has_value());

        // 第二轮：90002 的待办全部消失 → 只重扫 90002
        h.bridge->setResponse(QStringLiteral("/api/todos"), fixtures::todosWith90002Cleared());
        h.bridge->clearReceived();

        QSignalSpy finished(h.sync.get(), &SyncManager::runFinished);
        QVERIFY(h.sync->start(SyncManager::Mode::Incremental));
        QVERIFY(waitFor([&]() { return finished.count() > 0; }));

        const auto summary = finished.first().first().value<SyncManager::RunSummary>();
        QCOMPARE(summary.outcome, SyncManager::Outcome::Success);
        QCOMPARE(summary.coursesSkipped, 1);
        QCOMPARE(summary.coursesSucceeded, 1);

        int scanned90001 = 0;
        int scanned90002 = 0;
        for (const IPageBridge::Request &req : h.bridge->receivedRequests()) {
            if (req.path.contains(QLatin1String("/api/courses/90001/homework-activities"))) {
                ++scanned90001;
            }
            if (req.path.contains(QLatin1String("/api/courses/90002/homework-activities"))) {
                ++scanned90002;
            }
        }
        QCOMPARE(scanned90001, 0);
        QCOMPARE(scanned90002, 1);
    }

    /// 待办接口不可用（404）→ 必须回退到完整扫描，且**不得**被当成"没有待办"
    void quickIncrementalFallsBackWhenTodoEndpointUnavailable()
    {
        Harness h = makeHarness(QStringLiteral("quick-fallback"));
        registerTwoCourses(*h.bridge);
        runFullScanToSuccess(h);

        h.bridge->setResponse(QStringLiteral("/api/todos"), QStringLiteral("not found"), 404);
        h.bridge->clearReceived();

        QSignalSpy finished(h.sync.get(), &SyncManager::runFinished);
        QVERIFY(h.sync->start(SyncManager::Mode::Incremental));
        QVERIFY(waitFor([&]() { return finished.count() > 0; }));

        const auto summary = finished.first().first().value<SyncManager::RunSummary>();
        QCOMPARE(summary.outcome, SyncManager::Outcome::Success);
        QVERIFY(!summary.quickPath);          // 回退
        QCOMPARE(summary.coursesSkipped, 0);  // 全部课程都扫
        QCOMPARE(summary.coursesSucceeded, 2);
    }

    /// 还没有全量基线时，增量同步必须先完整扫一遍（不能只依赖待办信号）
    void quickIncrementalRequiresFullBaseline()
    {
        Harness h = makeHarness(QStringLiteral("quick-nobaseline"));
        registerTwoCourses(*h.bridge);
        h.bridge->setResponse(QStringLiteral("/api/todos"), fixtures::todosMatchingHomeworkThree());
        h.bridge->clearReceived();

        QSignalSpy finished(h.sync.get(), &SyncManager::runFinished);
        QVERIFY(h.sync->start(SyncManager::Mode::Incremental));
        QVERIFY(waitFor([&]() { return finished.count() > 0; }));

        const auto summary = finished.first().first().value<SyncManager::RunSummary>();
        QCOMPARE(summary.outcome, SyncManager::Outcome::Success);
        QVERIFY(!summary.quickPath);      // 没有基线 → 没走快速路径
        QCOMPARE(summary.coursesSkipped, 0);
        QCOMPARE(h.repo->countAssignments(QStringLiteral("90001")), 3);
        QCOMPARE(h.repo->countAssignments(QStringLiteral("90002")), 3);
    }
};

QTEST_MAIN(SyncManagerTests)
#include "SyncManagerTests.moc"
