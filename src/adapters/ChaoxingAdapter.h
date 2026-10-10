#pragma once

#include "models/Assignment.h"
#include "models/Course.h"

#include <QObject>
#include <QString>

#include <functional>
#include <memory>

QT_BEGIN_NAMESPACE
class QWebEnginePage;
class QWebEngineProfile;
class QWebEngineView;
QT_END_NAMESPACE

namespace njulms {

/**
 * 学习通（超星）网站适配器。
 *
 * 与 NjuLmsAdapter 的形态差异（都是刻意的）：
 *   - 智汇南雍有稳定的 JSON 接口，适配器只需「同源 fetch → JSON → Parser」；
 *   - 学习通**没有可用的 JSON 接口**（实测 2026-10-10，见 docs/06-学习通探测.md），
 *     课程列表与作业列表都是服务端渲染的 HTML，且作业列表地址由课程页 JS
 *     动态生成（iframe.src），只能通过真实浏览器导航 + DOM 交互取得。
 * 因此本适配器持有一只**独立的 QWebEnginePage**（与主页面共享同一个持久化
 * Profile，因此共享登录 Cookie），以异步状态机的方式完成：
 *
 *   首页(i.chaoxing.com) → 提取 visit/interaction?s=… → 课程中心(HTML)
 *   → 每门课程：课程页 → 点击「作业」→ 等待 iframe → 进入作业列表(HTML)
 *
 * 解析全部交给 ChaoxingParser；本类只负责「路径、参数、导航与等待」。
 *
 * ── 会话判定 ──────────────────────────────────────────────────────────────
 * 任一导航落到 passport*.chaoxing.com 即判定为需要登录（扫码），
 * 发出 loginRequired 并以 SessionExpired 结束本次调用。
 *
 * ── 超时与取消 ────────────────────────────────────────────────────────────
 * 每个导航阶段有独立超时（30 秒），超时按可重试的传输错误处理；
 * cancel() 之后悬挂的回调一律不再触发（generation 计数作废旧回调）。
 */
class ChaoxingAdapter : public QObject
{
    Q_OBJECT

public:
    /// 错误分类：语义与 NjuLmsAdapter::ErrorCode 对齐，便于上层统一处理
    enum class ErrorCode {
        None,
        SessionExpired,   ///< 落到登录页 → 需要扫码登录
        Transport,        ///< 导航失败 / 超时 → 可重试
        UnexpectedShape,  ///< 页面结构不符合实测模板（改版、空列表等）
    };

    struct Error
    {
        ErrorCode code = ErrorCode::None;
        QString message;

        bool ok() const noexcept { return code == ErrorCode::None; }
        bool needsLogin() const noexcept { return code == ErrorCode::SessionExpired; }
    };

    struct CourseListResult
    {
        Error error;
        QList<Course> courses;
    };

    struct WorkListResult
    {
        Error error;
        QList<Assignment> assignments;
        ParseIssues issues;
        /**
         * 本次作业列表能否证明是完整快照（决定能否做软删除）。
         * 分页线索出现时置 false —— 分页参数尚未实测确认，宁可保留旧数据。
         */
        bool snapshotComplete = false;
    };

    using CourseCallback = std::function<void(const CourseListResult &)>;
    using WorkCallback = std::function<void(const WorkListResult &)>;

    /**
     * @param profile 与主页面共享的持久化 Profile（Cookie / 登录态互通）
     * @param view    承载本适配器页面的视图。⚠️ 页面**必须**以视图为父构造
     *                （与 WebEnginePageBridge 一致）：实测在"页面先于视图存在"
     *                的构造顺序下，无可见窗口的页面会出现加载不完成、
     *                DOM 不渲染的问题（真机验证两次失败均源于此）。
     */
    ChaoxingAdapter(QWebEngineProfile *profile, QWebEngineView *view, QObject *parent = nullptr);
    ~ChaoxingAdapter() override;

    /// 供登录窗口显示（未登录时用户需要看到二维码页面）
    QWebEnginePage *page() const;

    /// 打开学习通首页（供"打开学习通并登录"按钮使用；任务进行中时忽略）
    void openLoginPage();

    bool isBusy() const;

    /// 获取课程列表（回调在主线程被调用；同一时刻只允许一个任务）
    void discoverCourses(CourseCallback callback);

    /// 获取一门课程的作业列表
    void fetchWorkList(const Course &course, WorkCallback callback);

    /// 取消进行中的任务：悬挂的回调不会再被触发
    void cancel();

signals:
    /// 需要人工扫码登录（页面将停留在 passport2.chaoxing.com）
    void loginRequired(const QString &reason);

    /// 阶段性进度文案（供状态栏/日志，不参与数据）
    void stageMessage(const QString &message);

private:
    class Impl;
    std::unique_ptr<Impl> d;
};

} // namespace njulms
