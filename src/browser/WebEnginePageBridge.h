#pragma once

#include "adapters/IPageBridge.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <memory>

QT_BEGIN_NAMESPACE
class QWebEnginePage;
class QWebEngineProfile;
class QWebEngineView;
QT_END_NAMESPACE

namespace njulms {

/**
 * 基于 Qt WebEngine 的 IPageBridge 实现：浏览器会话与登录模块。
 *
 * 职责边界（需求文档第三节第 1 点）：
 *   - 用 Qt WebEngine 打开智汇南雍，允许用户在窗口内**手动**完成统一身份认证
 *   - 用持久化 Profile 保存网站正常使用所需的会话数据
 *   - 检测是否仍停留在登录页 / 目标页面是否可访问
 *   - 向适配器提供受控的页面访问能力（只允许同源请求）
 *
 * ── 安全约束（需求第五节）─────────────────────────────────────────────────
 *   - 不在源码中保存账号、密码、验证码或会话令牌
 *   - 不绕过认证流程，不伪造他人身份，不做验证码识别
 *   - 不把 Cookie / 令牌 / 完整认证跳转 URL 写入日志（日志经 utils/Redact）
 *   - Profile 目录等同于账号凭证，必须视为敏感信息
 *
 * ── 实现方式说明 ──────────────────────────────────────────────────────────
 * 请求通过「在页面上下文里执行同源 fetch」完成，并经 QWebChannel 把结果回传 C++。
 * 这样做的三个理由：
 *   1. 天然复用浏览器的真实会话，无需在 C++ 里注入或伪造 Cookie；
 *   2. 站点会检测 DevTools 并清空页面渲染，但**不影响**接口请求，因此不依赖 DOM 解析；
 *   3. 不需要引入任何 HTML 解析库。
 */
class WebEnginePageBridge : public IPageBridge
{
    Q_OBJECT

public:
    explicit WebEnginePageBridge(QObject *parent = nullptr);
    ~WebEnginePageBridge() override;

    /**
     * 站点配置：默认智汇南雍。
     *
     * 接入新站点（如雨课堂）时，用同一套"页面上下文同源请求"能力即可——
     * 在 initialize() 之前 setSiteConfig() 一次，首页地址、相对路径基准与
     * 认证主机判定都会跟着走；不调用则保持原有行为（南大 LMS）。
     */
    struct SiteConfig
    {
        QString origin;         ///< 同源基准：相对路径请求都拼在它后面
        QString homeUrl;        ///< 首页 / 登录入口
        QStringList authHosts;  ///< 认证主机：进入这些主机视为"停留在登录流程"
    };

    /// 设置站点配置（必须在 initialize() 之前调用）
    void setSiteConfig(const SiteConfig &config);

    /// 创建（或复用）持久化 Profile。profileDir 为空时使用 defaultProfileDir()。
    /// 必须在 loadHome() 之前调用。
    bool initialize(const QString &profileDir = QString(), QString *error = nullptr);

    /// 承载网页的视图。调用方（MainWindow）负责把它放进布局并显示。
    QWebEngineView *view() const;

    /// 共享的持久化 Profile。学习通适配器用它创建自己的页面（同一份 Cookie）。
    /// 未成功 initialize() 时返回 nullptr。
    QWebEngineProfile *profile() const;

    /// 加载站点首页（登录入口）
    void loadHome();

    /// 加载任意同源路径
    void loadPath(const QString &path);

    /// 当前是否已经离开认证主机（即疑似已登录）
    /// ⚠️ 仅凭 URL 判断**不足以**确认登录；权威判定由 NjuLmsAdapter::checkSession
    ///    通过接口响应完成（需求明确要求不得仅凭首页加载成功就认为已登录）。
    bool looksAuthenticated() const;

    /// 当前是否停留在统一身份认证相关页面
    bool isOnAuthHost() const;

    /// 默认 Profile 目录：环境变量 NJULMS_PROFILE_DIR 优先，否则可执行文件同级 profile/
    static QString defaultProfileDir();

    // ---- IPageBridge ----
    void request(const Request &req, Callback callback) override;
    bool isReady() const override;
    QString currentUrl() const override;

    /// 注入到页面里的请求包装脚本（首次请求时执行）
    static QString requestScript();

signals:
    /// 页面地址变化（已脱敏：只发主机名与路径，不含查询串，避免泄漏认证参数）
    void safeUrlChanged(const QString &safeUrl);

    /// 进入了认证页面，需要用户手动登录
    void loginPageReached();

    /// Profile 无法初始化（例如目录不可写）
    void profileError(const QString &reason);

private:
    /// 读取并缓存 qwebchannel.js 与 fetch 包装器
    void installTransportScript();

    /// 在页面里（主世界）注入上面缓存的脚本。幂等，可重复调用。
    void ensurePageScripts();

    class Impl;
    std::unique_ptr<Impl> d;
};

} // namespace njulms
