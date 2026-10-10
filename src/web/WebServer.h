#pragma once

#include <QObject>
#include <QString>

#include <memory>

namespace njulms {

class WebApi;

/**
 * 只读 HTTP 服务（iPad 前端用）。
 *
 * 安全边界：
 *   * 只绑定回环地址 127.0.0.1，局域网与公网都访问不到；
 *   * 对外入口交给 Tailscale（`tailscale serve` 终止 TLS 后转发到本端口），
 *     因此这里不需要也不应该做 TLS；
 *   * 只有 GET 路由，没有写操作（写操作与令牌校验在下一阶段）。
 *
 * 启动失败（端口被占用等）只记录日志，不阻塞程序启动。
 */
class WebServer : public QObject
{
    Q_OBJECT

public:
    explicit WebServer(WebApi *api, QObject *parent = nullptr);
    ~WebServer() override;

    /// 访问令牌：只用于写操作（POST）。为空表示不校验（测试用）。
    void setAuthToken(const QString &token);

    /// port 传 0 表示由系统分配空闲端口（测试用）；失败时返回 false 并写 errorOut
    bool start(quint16 port, QString *errorOut = nullptr);
    void stop();

    bool isRunning() const;
    /// 实际监听端口（port 传 0 时也有效）
    quint16 port() const;

private:
    class Impl;
    std::unique_ptr<Impl> d;
};

} // namespace njulms
