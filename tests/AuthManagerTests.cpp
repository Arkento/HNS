#include "auth/AuthManager.h"
#include "support/FakePageBridge.h"
#include "support/Fixtures.h"

#include <QSignalSpy>
#include <QTest>

using namespace njulms;
using namespace njulms::test;

/**
 * 认证状态机测试。
 *
 * 重点验证需求里两处容易被做错的要求：
 *   1. 不得仅凭页面加载成功就认为已登录（判定必须来自接口响应）；
 *   2. 会话失效必须进入「需要重新登录」，而不是被当成空的课程列表，
 *      也不是被当成普通的网络失败。
 */
class AuthManagerTests : public QObject
{
    Q_OBJECT

private slots:

    void initialStateIsUnknown()
    {
        FakePageBridge bridge;
        NjuLmsAdapter adapter(&bridge);
        AuthManager auth(&adapter);

        QCOMPARE(auth.state(), AuthState::Unknown);
        QVERIFY(!auth.isAuthenticated());
        QVERIFY(!auth.needsManualLogin());
        QCOMPARE(auth.lastKnownCourseCount(), -1);
        QVERIFY(!auth.lastAuthenticatedAt().isValid());
    }

    void successfulCheckBecomesAuthenticated()
    {
        FakePageBridge bridge;
        bridge.setResponse(QStringLiteral("/api/my-courses"), fixtures::myCoursesTwoCourses());
        NjuLmsAdapter adapter(&bridge);
        adapter.setMaxRetries(0);
        AuthManager auth(&adapter);

        QSignalSpy stateSpy(&auth, &AuthManager::stateChanged);
        QSignalSpy checkedSpy(&auth, &AuthManager::checked);

        auth.check(true);

        QCOMPARE(auth.state(), AuthState::Authenticated);
        QVERIFY(auth.isAuthenticated());
        QCOMPARE(auth.lastKnownCourseCount(), 2);
        QVERIFY(auth.lastAuthenticatedAt().isValid());
        QCOMPARE(auth.consecutiveFailures(), 0);
        QCOMPARE(checkedSpy.count(), 1);
        QCOMPARE(checkedSpy.first().at(0).toBool(), true);
        // 至少经历过 Checking → Authenticated
        QVERIFY(stateSpy.count() >= 2);
    }

    void loginPageHtmlBecomesNeedLogin()
    {
        // 场景 C 的核心：站点返回登录页 HTML 且状态码仍是 200
        FakePageBridge bridge;
        bridge.setResponse(QStringLiteral("/api/my-courses"), fixtures::loginPageHtml(), 200);
        NjuLmsAdapter adapter(&bridge);
        adapter.setMaxRetries(0);
        AuthManager auth(&adapter);

        QSignalSpy loginSpy(&auth, &AuthManager::loginRequired);
        auth.check(true);

        QCOMPARE(auth.state(), AuthState::NeedLogin);
        QVERIFY(auth.needsManualLogin());
        QVERIFY(!auth.isAuthenticated());
        QCOMPARE(loginSpy.count(), 1);
        QVERIFY(auth.lastExpiredAt().isValid());
    }

    void http401BecomesNeedLogin()
    {
        FakePageBridge bridge;
        bridge.setResponse(QStringLiteral("/api/my-courses"), QStringLiteral("{}"), 401);
        NjuLmsAdapter adapter(&bridge);
        adapter.setMaxRetries(0);
        AuthManager auth(&adapter);

        auth.check(true);
        QCOMPARE(auth.state(), AuthState::NeedLogin);
    }

    void emptyCourseListStillCountsAsAuthenticated()
    {
        // 与「会话失效」严格区分：能解析出 JSON 且 courses 为空数组是合法状态
        FakePageBridge bridge;
        bridge.setResponse(QStringLiteral("/api/my-courses"), fixtures::myCoursesEmpty());
        NjuLmsAdapter adapter(&bridge);
        adapter.setMaxRetries(0);
        AuthManager auth(&adapter);

        QSignalSpy loginSpy(&auth, &AuthManager::loginRequired);
        auth.check(true);

        QCOMPARE(auth.state(), AuthState::Authenticated);
        QCOMPARE(auth.lastKnownCourseCount(), 0);
        QCOMPARE(loginSpy.count(), 0);
    }

    void transportFailureIsNotTreatedAsLoginProblem()
    {
        // 网络故障时误导用户去重新登录是错的：那是可重试错误
        FakePageBridge bridge;
        bridge.setTransportFailure(QStringLiteral("/api/my-courses"), QStringLiteral("network down"));
        NjuLmsAdapter adapter(&bridge);
        adapter.setMaxRetries(0);
        AuthManager auth(&adapter);

        QSignalSpy loginSpy(&auth, &AuthManager::loginRequired);
        auth.check(true);

        QVERIFY(auth.state() != AuthState::Authenticated);
        QVERIFY2(auth.state() != AuthState::NeedLogin,
                 "a transport failure must not be reported as a login problem");
        QCOMPARE(loginSpy.count(), 0);
        QCOMPARE(auth.consecutiveFailures(), 1);
    }

    void activeSyncDetectionExpiresSession()
    {
        // 适配器在采集途中报告失效 → 状态机必须同步，
        // 避免出现「适配器说失效、UI 说已登录」的矛盾
        FakePageBridge bridge;
        bridge.setResponse(QStringLiteral("/api/my-courses"), fixtures::myCoursesTwoCourses());
        NjuLmsAdapter adapter(&bridge);
        adapter.setMaxRetries(0);
        AuthManager auth(&adapter);

        auth.check(true);
        QVERIFY(auth.isAuthenticated());

        // 直接让适配器发出失效信号（等价于采集途中遇到 401）
        emit adapter.sessionExpired();

        QCOMPARE(auth.state(), AuthState::NeedLogin);
        QVERIFY(auth.needsManualLogin());
    }

    void restoringLoginEmitsLoginRestored()
    {
        FakePageBridge bridge;
        bridge.setResponse(QStringLiteral("/api/my-courses"), fixtures::loginPageHtml(), 200);
        NjuLmsAdapter adapter(&bridge);
        adapter.setMaxRetries(0);
        AuthManager auth(&adapter);

        auth.check(true);
        QCOMPARE(auth.state(), AuthState::NeedLogin);

        // 用户完成登录后，接口恢复可用
        bridge.setResponse(QStringLiteral("/api/my-courses"), fixtures::myCoursesTwoCourses());
        QSignalSpy restoredSpy(&auth, &AuthManager::loginRestored);
        auth.check(true);

        QCOMPARE(auth.state(), AuthState::Authenticated);
        QCOMPARE(auth.lastKnownCourseCount(), 2);
        QCOMPARE(restoredSpy.count(), 1);
        QCOMPARE(auth.consecutiveFailures(), 0);
    }

    void duplicateCheckWhileInFlightIsIgnored()
    {
        // FakePageBridge 是同步回调，因此这里构造一个「阻塞」的桥来验证去重
        class BlockingBridge : public FakePageBridge
        {
        public:
            void request(const Request &req, Callback callback) override
            {
                m_saved = callback;
                ++m_calls;
                Q_UNUSED(req)
            }
            void release(const QString &body)
            {
                if (m_saved) {
                    Response res;
                    res.transportOk = true;
                    res.httpStatus = 200;
                    res.body = body;
                    m_saved(res);
                    m_saved = nullptr;
                }
            }
            int calls() const { return m_calls; }

        private:
            Callback m_saved;
            int m_calls = 0;
        };

        BlockingBridge bridge;
        NjuLmsAdapter adapter(&bridge);
        adapter.setMaxRetries(0);
        AuthManager auth(&adapter);

        auth.check(false);            // 第一次：挂在半途
        QCOMPARE(auth.state(), AuthState::Checking);
        auth.check(false);            // 第二次：应被忽略
        QCOMPARE(bridge.calls(), 1);

        bridge.release(fixtures::myCoursesTwoCourses());
        QCOMPARE(auth.state(), AuthState::Authenticated);
    }

    void resetReturnsToUnknown()
    {
        FakePageBridge bridge;
        bridge.setResponse(QStringLiteral("/api/my-courses"), fixtures::myCoursesTwoCourses());
        NjuLmsAdapter adapter(&bridge);
        adapter.setMaxRetries(0);
        AuthManager auth(&adapter);

        auth.check(true);
        QVERIFY(auth.isAuthenticated());

        auth.reset();
        QCOMPARE(auth.state(), AuthState::Unknown);
        QCOMPARE(auth.lastKnownCourseCount(), -1);
        QVERIFY(!auth.isAuthenticated());
    }

    void stateKeysAndDisplayAreStable()
    {
        // 存储键必须稳定（不随界面语言变化）
        QCOMPARE(QString::fromLatin1(authStateKey(AuthState::Authenticated)),
                 QStringLiteral("authenticated"));
        QCOMPARE(QString::fromLatin1(authStateKey(AuthState::NeedLogin)),
                 QStringLiteral("need_login"));
        QVERIFY(!authStateDisplay(AuthState::NeedLogin).isEmpty());
    }
};

QTEST_MAIN(AuthManagerTests)
#include "AuthManagerTests.moc"
