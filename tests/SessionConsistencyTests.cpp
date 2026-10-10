#include "adapters/NjuLmsAdapter.h"
#include "auth/AuthManager.h"
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

using namespace njulms;
using namespace njulms::test;

/**
 * 会话失效的一致性测试。
 *
 * ── 为什么需要这一组 ──────────────────────────────────────────────────────
 * 同一次会话失效会被**两条独立路径**观察到：
 *   1. AuthManager 监听适配器的 sessionExpired 信号；
 *   2. SyncManager 自己做会话检查 / 课程采集时也会发现。
 *
 * 如果两边各自向 UI 发一次「需要重新登录」，用户会连续看到 2–3 个弹窗。
 * 实测确认存在这个竞态（AuthManager、SyncManager::loginRequired、
 * SyncManager::runFinished 三处都曾弹窗）。
 *
 * UI 弹窗本身难以在单元测试里断言，但**信号数量**可以，
 * 因此这里把「一次失效只产生一次通知」固化成回归测试。
 */
class SessionConsistencyTests : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    int m_counter = 0;

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

    /// 与 MainWindow 相同的接线方式：AuthManager 与 SyncManager 共用一个适配器
    struct Harness
    {
        std::unique_ptr<SqliteRepository> repo;
        std::unique_ptr<FakePageBridge> bridge;
        std::unique_ptr<NjuLmsAdapter> adapter;
        std::unique_ptr<AuthManager> auth;
        std::unique_ptr<SyncManager> sync;
    };

    Harness makeHarness(const QString &suffix)
    {
        Harness h;
        h.repo = std::make_unique<SqliteRepository>();
        const QString path = QDir(m_dir.path())
                                 .filePath(QStringLiteral("sess%1_%2.db").arg(suffix).arg(++m_counter));
        QString error;
        const bool opened = h.repo->open(path, &error);
        Q_ASSERT_X(opened, "makeHarness", qPrintable(error));

        h.bridge = std::make_unique<FakePageBridge>();
        h.adapter = std::make_unique<NjuLmsAdapter>(h.bridge.get());
        h.adapter->setMaxRetries(0);
        h.adapter->setRequestIntervalMs(0);

        h.auth = std::make_unique<AuthManager>(h.adapter.get());

        h.sync = std::make_unique<SyncManager>(h.repo.get(), h.adapter.get());
        SyncManager::Config cfg;
        cfg.pauseBetweenCoursesMs = 0;
        cfg.abortAfterConsecutiveFailures = 0;
        h.sync->setConfig(cfg);
        return h;
    }

private slots:

    void initTestCase() { QVERIFY(m_dir.isValid()); }

    // ---- 核心不变量：一次失效只通知一次 ------------------------------------

    void oneExpiryProducesExactlyOneLoginPrompt()
    {
        Harness h = makeHarness(QStringLiteral("once"));
        // 会话失效：站点返回登录页 HTML，且 HTTP 状态码仍是 200
        h.bridge->setResponse(QStringLiteral("/api/my-courses"), fixtures::loginPageHtml(), 200);

        QSignalSpy authPrompt(h.auth.get(), &AuthManager::loginRequired);
        QSignalSpy syncPrompt(h.sync.get(), &SyncManager::loginRequired);

        h.auth->check(true);
        QCOMPARE(authPrompt.count(), 1);

        // 用户再次点击「检查登录状态」：状态已经是 NeedLogin，
        // 不应因为重复检查而再次弹出提示
        h.auth->check(true);
        QCOMPARE(authPrompt.count(), 1);

        // 适配器又一次报告失效（例如某个后台请求收到 401）：仍然只提示一次
        emit h.adapter->sessionExpired();
        QCOMPARE(authPrompt.count(), 1);

        // SyncManager 这一侧可以有自己的信号（用于日志/状态栏），
        // 但它是另一种语义，UI 那边不再据此弹窗。
        QVERIFY(syncPrompt.count() <= 1);
    }

    void syncFailureDuringExpiryEmitsOnlyOneAuthPrompt()
    {
        // 覆盖「采集途中失效」这条路径：适配器会发现失效，
        // SyncManager 也会因为检查会话失败而收尾。
        Harness h = makeHarness(QStringLiteral("midsync"));
        h.bridge->setResponse(QStringLiteral("/api/my-courses"), fixtures::myCoursesTwoCourses());
        h.bridge->setResponse(QStringLiteral("/api/courses/90001/homework-activities"),
                              fixtures::homeworkActivitiesThree());
        h.bridge->setResponse(QStringLiteral("/api/courses/90002/homework-activities"),
                              fixtures::loginPageHtml(), 200);

        QSignalSpy authPrompt(h.auth.get(), &AuthManager::loginRequired);
        QSignalSpy finished(h.sync.get(), &SyncManager::runFinished);

        QVERIFY(h.sync->start(SyncManager::Mode::Full));
        QVERIFY(waitFor([&]() { return finished.count() > 0; }));

        // 无论内部经过几条路径，面向用户的「请重新登录」只应有一次
        QCOMPARE(authPrompt.count(), 1);
    }

    // ---- 两个组件的状态不得互相矛盾 ----------------------------------------

    void authAndSyncAgreeAfterExpiry()
    {
        Harness h = makeHarness(QStringLiteral("agree"));
        h.bridge->setResponse(QStringLiteral("/api/my-courses"), fixtures::myCoursesTwoCourses());
        h.bridge->setResponse(QStringLiteral("/api/courses/90001/homework-activities"),
                              fixtures::homeworkActivitiesThree());
        h.bridge->setResponse(QStringLiteral("/api/courses/90002/homework-activities"),
                              fixtures::loginPageHtml(), 200);

        QSignalSpy finished(h.sync.get(), &SyncManager::runFinished);
        QVERIFY(h.sync->start(SyncManager::Mode::Full));
        QVERIFY(waitFor([&]() { return finished.count() > 0; }));

        const auto summary = finished.first().first().value<SyncManager::RunSummary>();
        QCOMPARE(summary.outcome, SyncManager::Outcome::WaitingForLogin);

        // 关键不变量：AuthManager 必须同步进入 NeedLogin，
        // 否则会出现「同步说失效、界面说已登录」的矛盾
        QCOMPARE(h.auth->state(), AuthState::NeedLogin);
        QVERIFY(h.auth->needsManualLogin());
        QVERIFY(!h.auth->isAuthenticated());

        // 终态必须落库为 need_login 而不是普通 failed
        const auto run = h.repo->findRun(summary.runId);
        QVERIFY(run.has_value());
        QCOMPARE(run->status, CrawlRun::Status::NeedLogin);
    }

    void expiredSessionDoesNotDestroyExistingData()
    {
        // 需求场景 C 的数据侧保证：失效不得把登录页解析成空作业列表，
        // 更不得用空值覆盖库里已有的有效数据。
        Harness h = makeHarness(QStringLiteral("keepdata"));
        h.bridge->setResponse(QStringLiteral("/api/my-courses"), fixtures::myCoursesTwoCourses());
        h.bridge->setResponse(QStringLiteral("/api/courses/90001/homework-activities"),
                              fixtures::homeworkActivitiesThree());
        h.bridge->setResponse(QStringLiteral("/api/courses/90002/homework-activities"),
                              fixtures::homeworkActivitiesEmpty());

        {
            QSignalSpy finished(h.sync.get(), &SyncManager::runFinished);
            QVERIFY(h.sync->start(SyncManager::Mode::Full));
            QVERIFY(waitFor([&]() { return finished.count() > 0; }));
            QCOMPARE(finished.first().first().value<SyncManager::RunSummary>().outcome,
                     SyncManager::Outcome::Success);
        }

        const auto before = h.repo->findAssignment(QStringLiteral("90001"),
                                                   QStringLiteral("homework-70001"));
        QVERIFY(before.has_value());
        QVERIFY(before->title.hasValue());
        const int countBefore = h.repo->countAssignments(QStringLiteral("90001"));
        QCOMPARE(countBefore, 3);

        // 会话失效后再次同步
        h.bridge->setResponse(QStringLiteral("/api/my-courses"), fixtures::loginPageHtml(), 200);
        h.bridge->setResponse(QStringLiteral("/api/courses/90001/homework-activities"),
                              fixtures::loginPageHtml(), 200);

        {
            QSignalSpy finished(h.sync.get(), &SyncManager::runFinished);
            QVERIFY(h.sync->start(SyncManager::Mode::Incremental));
            QVERIFY(waitFor([&]() { return finished.count() > 0; }));
            QCOMPARE(finished.first().first().value<SyncManager::RunSummary>().outcome,
                     SyncManager::Outcome::WaitingForLogin);
        }

        // 记录数不变，字段值不变
        QCOMPARE(h.repo->countAssignments(QStringLiteral("90001")), countBefore);
        const auto after = h.repo->findAssignment(QStringLiteral("90001"),
                                                  QStringLiteral("homework-70001"));
        QVERIFY(after.has_value());
        QVERIFY2(after->title.hasValue(),
                 "an expired session must not clear a valid stored title");
        QCOMPARE(after->title.require(), before->title.require());

        // 课程列表也不得被清空
        QCOMPARE(h.repo->listCourses().size(), 2);
    }

    // ---- 登录恢复后两侧状态同步 -------------------------------------------

    void bothSidesRecoverAfterRelogin()
    {
        Harness h = makeHarness(QStringLiteral("recover"));
        h.bridge->setResponse(QStringLiteral("/api/my-courses"), fixtures::loginPageHtml(), 200);
        h.bridge->setResponse(QStringLiteral("/api/courses/90001/homework-activities"),
                              fixtures::homeworkActivitiesThree());
        h.bridge->setResponse(QStringLiteral("/api/courses/90002/homework-activities"),
                              fixtures::homeworkActivitiesEmpty());

        // 先失效一次
        {
            QSignalSpy finished(h.sync.get(), &SyncManager::runFinished);
            QVERIFY(h.sync->start(SyncManager::Mode::Full));
            QVERIFY(waitFor([&]() { return finished.count() > 0; }));
            QCOMPARE(h.auth->state(), AuthState::NeedLogin);
        }

        // 用户完成登录：接口恢复可用
        h.bridge->setResponse(QStringLiteral("/api/my-courses"), fixtures::myCoursesTwoCourses());

        QSignalSpy restored(h.auth.get(), &AuthManager::loginRestored);
        h.auth->check(true);

        QCOMPARE(h.auth->state(), AuthState::Authenticated);
        QCOMPARE(restored.count(), 1);

        // 恢复后必须能正常采集
        {
            QSignalSpy finished(h.sync.get(), &SyncManager::runFinished);
            QVERIFY(h.sync->start(SyncManager::Mode::Full));
            QVERIFY(waitFor([&]() { return finished.count() > 0; }));
            QCOMPARE(finished.first().first().value<SyncManager::RunSummary>().outcome,
                     SyncManager::Outcome::Success);
        }
        QCOMPARE(h.repo->countAssignments(QStringLiteral("90001")), 3);
    }

    void expiredSessionIsNotRecordedAsPlainFailure()
    {
        // WaitingForLogin 与 Failed 的处理方式完全不同，必须区分开
        Harness h = makeHarness(QStringLiteral("notfail"));
        h.bridge->setResponse(QStringLiteral("/api/my-courses"), fixtures::loginPageHtml(), 200);

        QSignalSpy finished(h.sync.get(), &SyncManager::runFinished);
        QVERIFY(h.sync->start(SyncManager::Mode::Full));
        QVERIFY(waitFor([&]() { return finished.count() > 0; }));

        const auto summary = finished.first().first().value<SyncManager::RunSummary>();
        QVERIFY(summary.outcome != SyncManager::Outcome::Failed);

        const QList<CrawlRun> runs = h.repo->listRecentRuns(1);
        QCOMPARE(runs.size(), 1);
        QVERIFY2(runs.first().status != CrawlRun::Status::Failed,
                 "an expired session must be recorded as need_login, not as a plain failure");
    }
};

QTEST_MAIN(SessionConsistencyTests)
#include "SessionConsistencyTests.moc"
