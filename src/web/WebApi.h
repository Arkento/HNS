#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QThread>

#include <functional>
#include <utility>

namespace njulms {

/**
 * 写操作结果。
 *
 * HTTP 状态码与"给用户看的原因"一起带回去：前端直接把 message 显示出来，
 * 不需要自己拼错误文案（避免同一件事在两处写成两种说法）。
 */
struct WebActionResult
{
    bool ok = false;
    int status = 200;   ///< ok=false 时的 HTTP 状态码（400 / 401 / 409 / 500）
    QString message;    ///< 失败原因（面向用户）
    QJsonObject data;   ///< 可选：成功时返回的最新状态
};

/**
 * API 的数据来源与动作入口。
 *
 * 为什么用回调而不是直接依赖 MainWindow：这些回调**必须在主线程执行**
 * （SyncManager / AuthManager / SqliteRepository / WebEngine 都活在主线程），
 * 由 WebApi 负责把 HTTP 线程的调用搬到主线程；测试里可以直接塞假数据。
 *
 * 写操作同样走这里：前端"改周期"调用的就是桌面端那批控件背后的同一段逻辑，
 * 不存在两套状态机。
 */
struct WebSources
{
    std::function<QJsonObject()> status;
    std::function<QJsonArray()> courses;
    std::function<QJsonArray(const QString &courseId)> assignments;
    /// scope = "pending"（未提交，含已逾期）或 "submitted"（已提交）
    std::function<QJsonArray(const QString &scope)> assignmentsByScope;
    std::function<QString(int lines)> logTail;

    std::function<WebActionResult(const QJsonObject &patch)> patchSettings;
    std::function<WebActionResult(const QString &mode)> startSync;
    std::function<WebActionResult()> pauseSync;
    std::function<WebActionResult()> resumeSync;
    std::function<WebActionResult()> cancelSync;
    /// 清空采集数据（保留登录状态）；confirm 必须等于 "DELETE"
    std::function<WebActionResult(const QString &confirm)> purgeData;
};

/**
 * 主线程执行器：把 HTTP 线程的请求串行化到主线程。
 *
 * 为什么必须这样：后端对象都不是线程安全的。同步任务在跑的时候会连续读写
 * SQLite、切换 WebEngine 页面、更新 Scheduler 状态；如果 HTTP 线程直接读它们，
 * 就变成「同步写库的同时前端读库」，轻则读到半截数据，重则崩溃。
 *
 * 这里用 BlockingQueuedConnection 把所有访问排进主线程的事件循环，与界面操作、
 * 同步步骤天然串行：一次只有一个访问者，不存在并发读写。代价是主线程被长时间
 * 占用时请求会排队（本项目的数据量下可忽略）。
 */
class WebApi : public QObject
{
    Q_OBJECT

public:
    explicit WebApi(WebSources sources, QObject *parent = nullptr);

    QJsonObject status();
    QJsonArray courses();
    QJsonArray assignments(const QString &courseId);
    QJsonArray assignmentsByScope(const QString &scope);
    QString logTail(int lines);

    WebActionResult patchSettings(const QJsonObject &patch);
    WebActionResult startSync(const QString &mode);
    WebActionResult pauseSync();
    WebActionResult resumeSync();
    WebActionResult cancelSync();
    WebActionResult purgeData(const QString &confirm);

private:
    template <typename Fn>
    auto run(Fn &&fn) -> decltype(fn());

    WebSources m_sources;
};

template <typename Fn>
auto WebApi::run(Fn &&fn) -> decltype(fn())
{
    if (QThread::currentThread() == thread()) {
        return fn();   // 已经在主线程（例如测试里直接调用）：不必绕一圈
    }

    using Result = decltype(fn());
    Result result{};
    // 阻塞式投递：HTTP 线程在此等待，主线程执行完再返回
    QMetaObject::invokeMethod(
        this, [&fn, &result]() { result = fn(); }, Qt::BlockingQueuedConnection);
    return result;
}

} // namespace njulms
