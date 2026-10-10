#include "AuthManager.h"

#include "utils/Logger.h"
#include "utils/TimeUtils.h"

namespace njulms {

const char *authStateKey(AuthState s) noexcept
{
    switch (s) {
    case AuthState::Unknown:       return "unknown";
    case AuthState::Authenticated: return "authenticated";
    case AuthState::NeedLogin:     return "need_login";
    case AuthState::Checking:      return "checking";
    }
    return "unknown";
}

QString authStateDisplay(AuthState s)
{
    switch (s) {
    case AuthState::Unknown:       return QStringLiteral("未知");
    case AuthState::Authenticated: return QStringLiteral("已登录");
    case AuthState::NeedLogin:     return QStringLiteral("需要登录");
    case AuthState::Checking:      return QStringLiteral("检查中…");
    }
    return QStringLiteral("未知");
}

class AuthManager::Impl
{
public:
    NjuLmsAdapter *adapter = nullptr;
    AuthState state = AuthState::Unknown;
    /// 进入 Checking 之前的状态。用于判断「本次是不是从失效中恢复」——
    /// 若不留存它，check() 已把状态改成 Checking，恢复事件就永远判断不出来。
    AuthState stateBeforeCheck = AuthState::Unknown;
    /// 本次「失效周期」内是否已经向用户发出过重新登录提示。
    ///
    /// 不能只靠 state == NeedLogin 去重：check() 会先把状态推进到 Checking，
    /// 于是「已经处于 NeedLogin 时再检查一次」会绕过该判断并重复弹窗（实测确认）。
    /// 只有在重新登录成功或 reset() 时才清掉这个标志。
    bool loginPromptIssued = false;
    QDateTime lastAuthenticatedAt;
    QDateTime lastExpiredAt;
    int lastKnownCourseCount = -1;
    int consecutiveFailures = 0;
    QString lastReason;
    bool checkInFlight = false;
};

AuthManager::AuthManager(NjuLmsAdapter *adapter, QObject *parent)
    : QObject(parent), d(std::make_unique<Impl>())
{
    Q_ASSERT(adapter != nullptr);
    d->adapter = adapter;

    // 适配器自己发现会话失效时（例如请求过程中返回 401），
    // 状态机必须同步跟进，否则会出现「适配器说失效、UI 说已登录」的矛盾状态。
    connect(d->adapter, &NjuLmsAdapter::sessionExpired, this, [this]() {
        markSessionExpired(QStringLiteral("采集过程中检测到会话失效"));
    });
}

AuthManager::~AuthManager() = default;

AuthState AuthManager::state() const
{
    return d->state;
}

bool AuthManager::isAuthenticated() const
{
    return d->state == AuthState::Authenticated;
}

QDateTime AuthManager::lastAuthenticatedAt() const
{
    return d->lastAuthenticatedAt;
}

QDateTime AuthManager::lastExpiredAt() const
{
    return d->lastExpiredAt;
}

int AuthManager::lastKnownCourseCount() const
{
    return d->lastKnownCourseCount;
}

int AuthManager::consecutiveFailures() const
{
    return d->consecutiveFailures;
}

bool AuthManager::needsManualLogin() const
{
    return d->state == AuthState::NeedLogin;
}

void AuthManager::setState(AuthState state)
{
    if (d->state == state) {
        return;
    }
    d->state = state;
    qCInfo(lcAuth) << "auth state ->" << authStateKey(state);
    emit stateChanged(state);
}

void AuthManager::reset()
{
    d->checkInFlight = false;
    d->lastKnownCourseCount = -1;
    d->consecutiveFailures = 0;
    d->loginPromptIssued = false;
    setState(AuthState::Unknown);
}

void AuthManager::markSessionExpired(const QString &reason)
{
    d->checkInFlight = false;

    d->lastExpiredAt = timeutil::nowUtc();
    d->lastReason = reason;
    setState(AuthState::NeedLogin);

    // 一个「失效周期」内只提示一次。用独立标志而不是 state 判断，
    // 因为 check() 会把状态短暂改成 Checking 再回到 NeedLogin。
    if (d->loginPromptIssued) {
        return;
    }
    d->loginPromptIssued = true;
    qCWarning(lcAuth) << "session requires manual login";
    emit loginRequired(reason);
}

void AuthManager::markAuthenticated(int courseCount)
{
    // 用 stateBeforeCheck 判断恢复：check() 会把状态推进到 Checking，
    // 因此不能拿 markAuthenticated 执行时的 state 去比较。
    const bool restored = (d->stateBeforeCheck == AuthState::NeedLogin);
    d->lastAuthenticatedAt = timeutil::nowUtc();
    d->lastKnownCourseCount = courseCount;
    d->consecutiveFailures = 0;
    d->lastReason.clear();
    d->checkInFlight = false;
    d->loginPromptIssued = false;  // 新一轮失效可以再次提示
    setState(AuthState::Authenticated);

    if (restored) {
        emit loginRestored(courseCount);
    }
}

void AuthManager::check(bool userInitiated)
{
    if (d->checkInFlight) {
        qCDebug(lcAuth) << "session check already in flight; ignoring duplicate request";
        return;
    }
    d->checkInFlight = true;
    d->stateBeforeCheck = d->state;
    setState(AuthState::Checking);

    if (userInitiated) {
        qCInfo(lcAuth) << "session check requested by user";
    }

    d->adapter->checkSession([this, userInitiated](const NjuLmsAdapter::SessionCheck &result) {
        d->checkInFlight = false;

        if (result.authenticated) {
            markAuthenticated(result.courseCount);
            emit checked(true, QStringLiteral("可访问 %1 门课程").arg(result.courseCount));
            return;
        }

        ++d->consecutiveFailures;

        // 区分「需要登录」与「其它失败」：
        //   需要登录 → 必须提示人工介入，不能反复重试；
        //   其它失败（网络/服务端）→ 属于可重试，不应误导用户去重新登录。
        if (result.error.needsLogin()) {
            markSessionExpired(result.error.message);
            emit checked(false, result.error.message);
            return;
        }

        // 非登录类失败：保持原状态（Unknown 或上一次的状态），只是报告失败。
        setState(d->state == AuthState::Checking ? AuthState::Unknown : d->state);
        d->lastReason = result.error.message;

        if (userInitiated) {
            qCWarning(lcAuth) << "session check failed (not a login problem):"
                              << result.error.message;
        }
        emit checked(false, result.error.message);
    });
}

} // namespace njulms
