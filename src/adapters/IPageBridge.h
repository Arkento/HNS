#pragma once

#include <QObject>
#include <QString>

#include <functional>

namespace njulms {

/**
 * 「受控的页面访问」抽象。
 *
 * 为什么需要这一层：需求文档要求网站专属逻辑（NjuLmsAdapter）与浏览器会话解耦，
 * 同时要求「网站页面变化时只需要主要修改适配器和解析器」。
 *
 * 本接口把「在已登录的页面上下文里执行一次同源请求」抽象成一个异步操作：
 *   - 真实实现（WebEnginePageBridge）经 QWebEnginePage::runJavaScript 执行 fetch，
 *     天然复用浏览器 Cookie，无需在 C++ 里注入会话令牌。
 *   - 测试实现（FakePageBridge）直接回放脱敏后的真实响应样本，
 *     使 Adapter / Parser / SyncManager 可以在**没有 Qt WebEngine、没有网络**的
 *     情况下完成单元测试。
 *
 * 安全约定：实现方不得记录 Cookie、令牌或完整认证跳转 URL；
 * 日志一律经 utils/Redact 处理。
 */
class IPageBridge : public QObject
{
    Q_OBJECT

public:
    /// 一次同源请求的描述
    struct Request
    {
        enum class Method { Get, Post };

        QString path;            ///< 同源路径，例如 "/api/my-courses"（不含主机名）
        Method method = Method::Get;
        QString jsonBody;        ///< POST 时的 JSON 请求体；GET 时为空
        /// 额外的请求头。默认空；雨课堂等站点要求自定义头（如 `xtbz: ykt`）时使用。
        QList<QPair<QString, QString>> extraHeaders;

        static Request get(const QString &path) { return Request{path, Method::Get, QString()}; }
        static Request post(const QString &path, const QString &jsonBody)
        {
            return Request{path, Method::Post, jsonBody};
        }
    };

    /// 一次请求的结果
    struct Response
    {
        bool transportOk = false;  ///< JS 执行本身是否成功（网络失败时为 false）
        int httpStatus = 0;        ///< HTTP 状态码；传输失败时为 0
        QString body;              ///< 响应正文原文
        QString error;             ///< transportOk 为 false 时的错误描述（不得含敏感信息）

        /// 是否拿到了一次「看起来正常」的 HTTP 响应
        bool hasHttpResponse() const noexcept { return transportOk && httpStatus > 0; }
    };

    using Callback = std::function<void(const Response &)>;

    explicit IPageBridge(QObject *parent = nullptr) : QObject(parent) {}
    ~IPageBridge() override = default;

    /**
     * 发起一次同源请求。必须异步返回（不得阻塞调用线程 / UI）。
     * 实现方需保证同源：不得接受绝对 URL，避免适配器误请求第三方主机。
     */
    virtual void request(const Request &req, Callback callback) = 0;

    /// 当前桥接是否可用（页面已加载、未被销毁）
    virtual bool isReady() const = 0;

    /// 当前页面地址（已脱敏语义：实现方返回原始 URL 即可，日志由调用方脱敏）
    virtual QString currentUrl() const = 0;

signals:
    /// 页面加载完成
    void loadFinished(bool ok, const QString &url);

    /// 桥接被销毁或页面崩溃，后续请求都会失败
    void becameUnavailable(const QString &reason);
};

} // namespace njulms
