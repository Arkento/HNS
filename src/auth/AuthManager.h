#pragma once

#include "adapters/NjuLmsAdapter.h"

#include <QDateTime>
#include <QObject>
#include <QString>

#include <memory>

namespace njulms {

/**
 * 认证状态。
 *
 * 需求文档明确要求：**不要仅凭首页加载成功就判断用户已登录**，
 * 因此状态只能由「接口响应能否解析为 JSON」来推进，不能由页面加载事件推进。
 */
enum class AuthState {
    Unknown,        ///< 尚未检查
    Authenticated,  ///< 已登录且接口可用
    NeedLogin,      ///< 会话失效或尚未登录，需要人工完成统一身份认证
    Checking,       ///< 正在检查
};

const char *authStateKey(AuthState s) noexcept;
QString authStateDisplay(AuthState s);

/**
 * 认证状态管理。
 *
 * 职责边界（需求文档第三节第 1 点中「认证状态管理」的部分）：
 *   - 维护并可查询当前 AuthState
 *   - 用适配器的接口探测会话（而不是看页面加载成功）
 *   - 会话失效时**暂停采集并提示重新登录**，而不是把登录页当成空数据
 *   - 记住会话失效发生的时间，供 UI 显示
 *
 * **不负责**：发起登录（那是用户在浏览器里手动完成的）、保存凭据（本项目不保存）、
 * 直接访问数据库或页面。
 *
 * 与 SyncManager 的分工：本模块只回答「现在能不能采」，
 * SyncManager 负责编排采集流程；两者通过信号连接，避免互相修改内部状态。
 */
class AuthManager : public QObject
{
    Q_OBJECT

public:
    explicit AuthManager(NjuLmsAdapter *adapter, QObject *parent = nullptr);
    ~AuthManager() override;

    // ---- 查询 ---------------------------------------------------------------

    AuthState state() const;
    bool isAuthenticated() const;

    /// 上次成功确认登录的时刻（UTC）
    QDateTime lastAuthenticatedAt() const;

    /// 上次检测到会话失效的时刻（UTC）
    QDateTime lastExpiredAt() const;

    /// 上次探测得到的可访问课程数；-1 表示未知
    int lastKnownCourseCount() const;

    /// 连续探测失败次数（用于 UI 提示与退避）
    int consecutiveFailures() const;

    /// 是否需要人工重新登录
    bool needsManualLogin() const;

    // ---- 操作 ---------------------------------------------------------------

    /**
     * 探测一次会话状态。异步，不阻塞 UI。
     * @param userInitiated 是否由用户点击触发（影响日志级别与提示方式）
     */
    void check(bool userInitiated = false);

    /// 标记为「需要重新登录」。由 SyncManager 在采集过程中发现会话失效时调用，
    /// 使状态机与实际发生的事保持一致，避免两处各自维护一份状态。
    void markSessionExpired(const QString &reason);

    /// 标记为已登录（用户在浏览器里完成登录后，UI 主动确认时使用）。
    /// ⚠️ 调用方必须先通过 check() 拿到 authenticated == true 的结论，
    /// 不要仅凭页面跳转就调用它。
    void markAuthenticated(int courseCount);

    /// 重置为 Unknown（例如切换了 Profile）
    void reset();

signals:
    /// 状态发生变化
    void stateChanged(njulms::AuthState state);

    /// 会话失效，需要人工重新登录。reason 已经过脱敏，可直接显示。
    void loginRequired(const QString &reason);

    /// 重新登录成功
    void loginRestored(int courseCount);

    /// 一次探测完成（无论成功失败），供 UI 刷新显示
    void checked(bool authenticated, const QString &detail);

private:
    void setState(AuthState state);

    class Impl;
    std::unique_ptr<Impl> d;
};

} // namespace njulms
