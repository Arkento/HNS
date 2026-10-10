#include "adapters/NjuLmsAdapter.h"
#include "repository/SqliteRepository.h"
#include "scheduler/Scheduler.h"
#include "support/FakePageBridge.h"
#include "support/Fixtures.h"
#include "sync/SyncManager.h"

#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace njulms;
using namespace njulms::test;

/**
 * 定时调度测试。
 *
 * 需求文档特别要求注意 QTimer 的三个陷阱，这里逐条覆盖：
 *   1. 程序退出期间 QTimer 不运行 → checkStartupState 必须能发现超期
 *   2. 程序重启后要检查上次运行状态 → reapOrphanedRuns + orphanedRunDetected
 *   3. 不要同时启动多个相同任务 → 并发保护
 *
 * 注意：这些用例**不**真的等待一个小时，而是用最小允许周期（15 分钟）验证
 * 计时器状态、以及直接调用 triggerNow / checkStartupState 来验证逻辑。
 */
class SchedulerTests : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    int m_counter = 0;

    struct Harness
    {
        std::unique_ptr<SqliteRepository> repo;
        std::unique_ptr<FakePageBridge> bridge;
        std::unique_ptr<NjuLmsAdapter> adapter;
        std::unique_ptr<SyncManager> sync;
        std::unique_ptr<Scheduler> scheduler;
    };

    Harness makeHarness(const QString &suffix)
    {
        Harness h;
        h.repo = std::make_unique<SqliteRepository>();
        const QString path = QDir(m_dir.path())
                                 .filePath(QStringLiteral("sched%1_%2.db").arg(suffix).arg(++m_counter));
        QString error;
        const bool opened = h.repo->open(path, &error);
        Q_ASSERT_X(opened, "makeHarness", qPrintable(error));

        h.bridge = std::make_unique<FakePageBridge>();
        h.bridge->setResponse(QStringLiteral("/api/my-courses"), fixtures::myCoursesTwoCourses());
        h.bridge->setResponse(QStringLiteral("/api/courses/90001/homework-activities"),
                              fixtures::homeworkActivitiesThree());
        h.bridge->setResponse(QStringLiteral("/api/courses/90002/homework-activities"),
                              fixtures::homeworkActivitiesEmpty());

        h.adapter = std::make_unique<NjuLmsAdapter>(h.bridge.get());
        h.adapter->setMaxRetries(0);
        h.adapter->setRequestIntervalMs(0);

        h.sync = std::make_unique<SyncManager>(h.repo.get(), h.adapter.get());
        SyncManager::Config cfg;
        cfg.pauseBetweenCoursesMs = 0;
        cfg.abortAfterConsecutiveFailures = 0;
        h.sync->setConfig(cfg);

        h.scheduler = std::make_unique<Scheduler>(h.repo.get(), h.sync.get());
        return h;
    }

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

private slots:

    void initTestCase() { QVERIFY(m_dir.isValid()); }

    // ---- 周期配置 -----------------------------------------------------------

    void intervalIsClampedToSafeRange()
    {
        Harness h = makeHarness(QStringLiteral("interval"));

        // 不允许配置成分钟级高频采集：需求要求「不要在规则尚未确认前启用高频自动采集」
        h.scheduler->setIntervalMinutes(1);
        QCOMPARE(h.scheduler->intervalMinutes(), Scheduler::kMinIntervalMinutes);

        h.scheduler->setIntervalMinutes(0);
        QCOMPARE(h.scheduler->intervalMinutes(), Scheduler::kMinIntervalMinutes);

        h.scheduler->setIntervalMinutes(-100);
        QCOMPARE(h.scheduler->intervalMinutes(), Scheduler::kMinIntervalMinutes);

        h.scheduler->setIntervalMinutes(10 * 24 * 60);
        QCOMPARE(h.scheduler->intervalMinutes(), Scheduler::kMaxIntervalMinutes);

        h.scheduler->setIntervalMinutes(120);
        QCOMPARE(h.scheduler->intervalMinutes(), 120);
    }

    void defaultIntervalIsOneHour()
    {
        Harness h = makeHarness(QStringLiteral("default"));
        QCOMPARE(h.scheduler->intervalMinutes(), Scheduler::kDefaultIntervalMinutes);
        QCOMPARE(Scheduler::kDefaultIntervalMinutes, 60);
    }

    // ---- 启用 / 停用 --------------------------------------------------------

    void enableAndDisableTogglesTimerState()
    {
        Harness h = makeHarness(QStringLiteral("toggle"));
        QSignalSpy enabledSpy(h.scheduler.get(), &Scheduler::enabledChanged);

        QVERIFY(!h.scheduler->isEnabled());
        QCOMPARE(h.scheduler->msUntilNextRun(), -1);
        QVERIFY(!h.scheduler->nextRunAt().isValid());

        h.scheduler->start();
        QVERIFY(h.scheduler->isEnabled());
        QCOMPARE(enabledSpy.count(), 1);
        QCOMPARE(enabledSpy.first().first().toBool(), true);

        // 启用后必须有明确的下一次触发时刻，供 UI 显示倒计时
        QVERIFY(h.scheduler->nextRunAt().isValid());
        QVERIFY(h.scheduler->msUntilNextRun() > 0);

        // 重复 start 不应重复发信号
        h.scheduler->start();
        QCOMPARE(enabledSpy.count(), 1);

        h.scheduler->stop();
        QVERIFY(!h.scheduler->isEnabled());
        QCOMPARE(enabledSpy.count(), 2);
        QCOMPARE(h.scheduler->msUntilNextRun(), -1);
        QVERIFY(!h.scheduler->nextRunAt().isValid());

        // 重复 stop 无害
        h.scheduler->stop();
        QCOMPARE(enabledSpy.count(), 2);
    }

    void nextRunAtMovesWhenIntervalChanges()
    {
        Harness h = makeHarness(QStringLiteral("rearm"));
        h.scheduler->setIntervalMinutes(60);
        h.scheduler->start();
        const qint64 oneHour = h.scheduler->msUntilNextRun();

        h.scheduler->setIntervalMinutes(15);
        const qint64 fifteenMin = h.scheduler->msUntilNextRun();

        QVERIFY(oneHour > fifteenMin);
        QVERIFY(fifteenMin > 0);
    }

    // ---- 并发保护 -----------------------------------------------------------

    /// v2：从未成功全量过（或距上次全量成功超过阈值）时，触发自动改跑全量对账
    void fullReconcileEscalatesToFullScan()
    {
        Harness h = makeHarness(QStringLiteral("reconcile"));
        h.scheduler->setMode(SyncManager::Mode::Incremental);
        h.scheduler->setFullReconcileHours(24);
        QCOMPARE(h.scheduler->fullReconcileHours(), 24);

        QSignalSpy finished(h.sync.get(), &SyncManager::runFinished);

        // 第一次：还没有「全量成功」标记 → 必须升级为全量
        QVERIFY(h.scheduler->triggerNow());
        QVERIFY(waitFor([&]() { return finished.count() > 0; }));
        const auto first = finished.first().first().value<SyncManager::RunSummary>();
        QCOMPARE(first.mode, SyncManager::Mode::Full);
        QCOMPARE(first.outcome, SyncManager::Outcome::Success);

        // 全量成功会写下对账标记
        QVERIFY(h.repo->findSyncState(SqliteRepository::fullReconcileTaskKey()).has_value());

        // 第二次：距上次全量成功只有几秒 → 回到增量
        QVERIFY(h.scheduler->triggerNow());
        QVERIFY(waitFor([&]() { return finished.count() > 1; }));
        const auto second = finished.at(1).first().value<SyncManager::RunSummary>();
        QCOMPARE(second.mode, SyncManager::Mode::Incremental);
    }

    void triggerNowStartsSync()
    {
        Harness h = makeHarness(QStringLiteral("trigger"));
        QSignalSpy triggeredSpy(h.scheduler.get(), &Scheduler::triggered);
        QSignalSpy finished(h.sync.get(), &SyncManager::runFinished);

        QString error;
        QVERIFY2(h.scheduler->triggerNow(&error), qPrintable(error));
        QCOMPARE(triggeredSpy.count(), 1);

        QVERIFY(waitFor([&]() { return finished.count() > 0; }));
        QCOMPARE(finished.first().first().value<SyncManager::RunSummary>().outcome,
                 SyncManager::Outcome::Success);
    }

    void triggerNowIsRejectedWhileSyncRunning()
    {
        Harness h = makeHarness(QStringLiteral("busy"));
        QSignalSpy skippedSpy(h.scheduler.get(), &Scheduler::skippedBecauseBusy);

        // FakePageBridge 是同步回调：一次 start() 会在返回前跑完整轮同步，
        // 因此要让第一轮「停在半途」才能验证并发拒绝。
        // 办法是让某门课返回 503 并允许重试，借重试的 QTimer 延迟造出运行窗口。
        h.adapter->setMaxRetries(1);
        h.adapter->setRequestIntervalMs(0);
        h.bridge->setResponse(QStringLiteral("/api/courses/90002/homework-activities"),
                              QStringLiteral("server error"), 503);

        QVERIFY(h.sync->start(SyncManager::Mode::Full));
        QVERIFY2(h.sync->isRunning(),
                 "sync must still be running while a course retry is pending");

        // 需求：不要同时启动多个相同的采集任务
        QString error;
        QVERIFY(!h.scheduler->triggerNow(&error));
        QVERIFY(error.contains(QStringLiteral("already running")));
        QCOMPARE(skippedSpy.count(), 1);

        QVERIFY(waitFor([&]() { return !h.sync->isRunning(); }));
    }

    // ---- 程序启动状态检查（QTimer 在退出期间不运行）------------------------

    void startupDetectsNeverSynced()
    {
        Harness h = makeHarness(QStringLiteral("firstrun"));
        QString summary;
        // 从未成功同步过 → 需要补一次
        QVERIFY(h.scheduler->checkStartupState(&summary));
        QVERIFY(summary.contains(QStringLiteral("尚未完成过任何同步")));
        QVERIFY(!h.scheduler->lastSuccessfulSync().isValid());
    }

    void startupReapsOrphanedRunAndReportsIt()
    {
        Harness h = makeHarness(QStringLiteral("orphan"));

        // 制造一个「上次被强制关闭」的遗留运行
        const qint64 runId = h.repo->startRun(QStringLiteral("sync"), QStringLiteral("full"));
        QVERIFY(runId > 0);
        const auto before = h.repo->findRun(runId);
        QVERIFY(before.has_value());
        QCOMPARE(before->status, CrawlRun::Status::Running);

        QSignalSpy orphanSpy(h.scheduler.get(), &Scheduler::orphanedRunDetected);

        QString summary;
        h.scheduler->checkStartupState(&summary);

        // 必须被识别并收尾
        QCOMPARE(orphanSpy.count(), 1);
        QCOMPARE(orphanSpy.first().first().toInt(), 1);
        QVERIFY(summary.contains(QStringLiteral("未完成")));

        const auto after = h.repo->findRun(runId);
        QVERIFY(after.has_value());
        QCOMPARE(after->status, CrawlRun::Status::Failed);
        QVERIFY(after->finishedAt.has_value());
    }

    void startupDetectsOverdueSync()
    {
        Harness h = makeHarness(QStringLiteral("overdue"));
        h.scheduler->setIntervalMinutes(Scheduler::kMinIntervalMinutes);

        // 写入一个「很久以前成功过」的同步状态
        SyncState state;
        state.taskKey = SqliteRepository::courseDiscoveryTaskKey();
        state.mode = QStringLiteral("full");
        state.cursor = QStringLiteral("completed");
        state.lastSuccessAt = QDateTime(QDate(2020, 1, 1), QTime(0, 0), QTimeZone::UTC);
        state.fullScanCompleted = true;
        state.updatedAt = state.lastSuccessAt.value();
        QString error;
        QVERIFY2(h.repo->upsertSyncState(state, &error), qPrintable(error));

        QSignalSpy overdueSpy(h.scheduler.get(), &Scheduler::overdueSince);

        QString summary;
        const bool needsCatchUp = h.scheduler->checkStartupState(&summary);

        QVERIFY(needsCatchUp);
        QCOMPARE(overdueSpy.count(), 1);
        QVERIFY(summary.contains(QStringLiteral("建议立即同步")));
        QCOMPARE(h.scheduler->lastSuccessfulSync(), state.lastSuccessAt.value());
    }

    void startupIsQuietWhenRecentlySynced()
    {
        Harness h = makeHarness(QStringLiteral("recent"));
        h.scheduler->setIntervalMinutes(Scheduler::kMinIntervalMinutes);

        SyncState state;
        state.taskKey = SqliteRepository::courseDiscoveryTaskKey();
        state.mode = QStringLiteral("full");
        state.lastSuccessAt = QDateTime::currentDateTimeUtc();  // 刚刚成功过
        state.fullScanCompleted = true;
        state.updatedAt = state.lastSuccessAt.value();
        h.repo->upsertSyncState(state);

        QSignalSpy overdueSpy(h.scheduler.get(), &Scheduler::overdueSince);
        QSignalSpy orphanSpy(h.scheduler.get(), &Scheduler::orphanedRunDetected);

        QString summary;
        const bool needsCatchUp = h.scheduler->checkStartupState(&summary);

        QVERIFY(!needsCatchUp);
        QCOMPARE(overdueSpy.count(), 0);
        QCOMPARE(orphanSpy.count(), 0);
        QVERIFY(summary.isEmpty());
    }

    void catchUpCanBeDisabled()
    {
        Harness h = makeHarness(QStringLiteral("nocatchup"));
        h.scheduler->setCatchUpOnStartup(false);

        QString summary;
        // 即使从未同步过，关闭补采后也不要求立即执行
        QVERIFY(!h.scheduler->checkStartupState(&summary));
        QVERIFY(!summary.isEmpty());  // 但仍然要如实说明情况
    }

    // ---- 模式 -----------------------------------------------------------------

    void defaultModeIsIncremental()
    {
        Harness h = makeHarness(QStringLiteral("mode"));
        QCOMPARE(h.scheduler->mode(), SyncManager::Mode::Incremental);

        h.scheduler->setMode(SyncManager::Mode::Full);
        QCOMPARE(h.scheduler->mode(), SyncManager::Mode::Full);
    }

    void triggerUsesConfiguredMode()
    {
        Harness h = makeHarness(QStringLiteral("mode2"));
        h.scheduler->setMode(SyncManager::Mode::Full);

        QSignalSpy finished(h.sync.get(), &SyncManager::runFinished);
        QVERIFY(h.scheduler->triggerNow());
        QVERIFY(waitFor([&]() { return finished.count() > 0; }));

        const auto summary = finished.first().first().value<SyncManager::RunSummary>();
        QCOMPARE(summary.mode, SyncManager::Mode::Full);
    }

    // ---- 停用后不再自动触发 -----------------------------------------------

    void disabledSchedulerDoesNotArmTimer()
    {
        Harness h = makeHarness(QStringLiteral("disabled"));
        h.scheduler->setIntervalMinutes(Scheduler::kMinIntervalMinutes);
        // 未调用 start()，计时器不得处于活动状态
        QVERIFY(!h.scheduler->isEnabled());
        QCOMPARE(h.scheduler->msUntilNextRun(), -1);
    }
};

QTEST_MAIN(SchedulerTests)
#include "SchedulerTests.moc"
