#include "web/WebServer.h"

#include "web/WebApi.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLoggingCategory>
#include <QString>

#include <atomic>
#include <exception>
#include <string>
#include <thread>

// 第三方单头文件库：只在本文译单元里出现，避免把它带进其它源文件
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4244 4267 4324 4456 4457 4458 4459)
#endif
#include "httplib.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

Q_LOGGING_CATEGORY(lcWeb, "njulms.web")

namespace njulms {
namespace {

/// 静态资源：路径 -> Qt 资源路径（内容在 start() 时读入内存）
struct Asset
{
    const char *urlPath;
    const char *resourcePath;
    const char *contentType;
};

const Asset kAssets[] = {
    {"/", ":/web/index.html", "text/html; charset=utf-8"},
    {"/index.html", ":/web/index.html", "text/html; charset=utf-8"},
    {"/app.js", ":/web/app.js", "application/javascript; charset=utf-8"},
    {"/style.css", ":/web/style.css", "text/css; charset=utf-8"},
    {"/apple-touch-icon.png", ":/web/apple-touch-icon.png", "image/png"},
    {"/service-worker.js", ":/web/service-worker.js", "application/javascript; charset=utf-8"},
};

void writeJson(httplib::Response &res, const QJsonObject &root, int status)
{
    const QByteArray body = QJsonDocument(root).toJson(QJsonDocument::Compact);
    res.status = status;
    res.set_content(std::string(body.constData(), static_cast<size_t>(body.size())),
                    "application/json; charset=utf-8");
    res.set_header("Cache-Control", "no-store");
}

/// 成功响应统一为 {"ok":true,"data":...}
void writeData(httplib::Response &res, const QJsonValue &data)
{
    QJsonObject root;
    root.insert(QStringLiteral("ok"), true);
    root.insert(QStringLiteral("data"), data);
    writeJson(res, root, 200);
}

/// 失败响应统一为 {"ok":false,"error":{"message":...}}
void writeError(httplib::Response &res, int status, const QString &message)
{
    QJsonObject error;
    error.insert(QStringLiteral("message"), message);

    QJsonObject root;
    root.insert(QStringLiteral("ok"), false);
    root.insert(QStringLiteral("error"), error);
    writeJson(res, root, status);
}

/// 写操作结果 → HTTP 响应
void writeActionResult(httplib::Response &res, const WebActionResult &result)
{
    if (!result.ok) {
        writeError(res, result.status > 0 ? result.status : 400, result.message);
        return;
    }
    if (result.data.isEmpty()) {
        QJsonObject data;
        data.insert(QStringLiteral("ok"), true);
        writeData(res, data);
        return;
    }
    writeData(res, result.data);
}

/// 读取请求体里的 JSON 对象；失败时直接回 400
bool readJsonBody(const httplib::Request &req, httplib::Response &res, QJsonObject *out)
{
    if (req.body.empty()) {
        writeError(res, 400, QStringLiteral("请求体为空"));
        return false;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(req.body));
    if (!doc.isObject()) {
        writeError(res, 400, QStringLiteral("请求体不是 JSON 对象"));
        return false;
    }
    *out = doc.object();
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

class WebServer::Impl
{
public:
    explicit Impl(WebApi *api) : api(api) {}

    WebApi *api = nullptr;
    QString token;   ///< 写操作令牌；为空表示不校验
    httplib::Server server;
    std::thread thread;
    std::atomic<bool> running{false};
    quint16 port = 0;
    QHash<QString, QByteArray> assets;
    QHash<QString, QString> contentTypes;
};

// ---------------------------------------------------------------------------
// 构造 / 析构
// ---------------------------------------------------------------------------

WebServer::WebServer(WebApi *api, QObject *parent)
    : QObject(parent), d(std::make_unique<Impl>(api))
{
}

WebServer::~WebServer()
{
    stop();
}

bool WebServer::isRunning() const
{
    return d->running.load();
}

quint16 WebServer::port() const
{
    return d->port;
}

void WebServer::setAuthToken(const QString &token)
{
    d->token = token;
}

// ---------------------------------------------------------------------------
// 启动 / 停止
// ---------------------------------------------------------------------------

bool WebServer::start(quint16 port, QString *errorOut)
{
    if (d->running.load()) {
        return true;
    }
    if (d->api == nullptr) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("WebApi 未初始化");
        }
        return false;
    }

    // ---- 静态资源：读一次放内存（资源缺失只告警，不影响 API）----
    for (const Asset &asset : kAssets) {
        QFile file(QString::fromLatin1(asset.resourcePath));
        if (!file.open(QIODevice::ReadOnly)) {
            qCWarning(lcWeb) << "web asset missing:" << asset.resourcePath;
            continue;
        }
        const QString key = QString::fromLatin1(asset.urlPath);
        d->assets.insert(key, file.readAll());
        d->contentTypes.insert(key, QString::fromLatin1(asset.contentType));
    }

    // 资源版本 = 静态资源内容的哈希。
    //
    // 为什么需要：iPad 上的"添加到主屏幕"网页 app 会长时间开着，页面里的 JS/CSS
    // 不会自己重新下载 —— 改了前端，用户看到的还是旧界面（真实踩过：
    // 刷新按钮的新反馈没出现）。这里把版本号拼进 index.html 里的资源 URL，
    // 前端一改 URL 就变，浏览器只能重新拉取；用户什么都不用做。
    {
        QCryptographicHash hash(QCryptographicHash::Sha256);
        const QStringList order = {QStringLiteral("/app.js"), QStringLiteral("/style.css"),
                                   QStringLiteral("/")};
        for (const QString &key : order) {
            hash.addData(d->assets.value(key));
        }
        const QString version = QString::fromLatin1(hash.result().toHex().left(8));
        if (d->assets.contains(QStringLiteral("/"))) {
            QByteArray &page = d->assets[QStringLiteral("/")];
            page.replace("__ASSET_VERSION__", version.toUtf8());
        }
        qCInfo(lcWeb) << "asset version:" << version;
    }

    // ---- 路由（全部只读）----
    d->server.Get("/api/health", [](const httplib::Request &, httplib::Response &res) {
        QJsonObject data;
        data.insert(QStringLiteral("status"), QStringLiteral("up"));
        writeData(res, data);
    });

    d->server.Get("/api/status", [this](const httplib::Request &, httplib::Response &res) {
        writeData(res, d->api->status());
    });

    d->server.Get("/api/courses", [this](const httplib::Request &, httplib::Response &res) {
        writeData(res, d->api->courses());
    });

    // 跨课程的作业集合：前端两个 tab（未提交 / 已提交）就靠这一个接口
    d->server.Get("/api/assignments", [this](const httplib::Request &req, httplib::Response &res) {
        const QString scope =
            req.has_param("scope") ? QString::fromStdString(req.get_param_value("scope"))
                                   : QStringLiteral("pending");
        if (scope != QLatin1String("pending") && scope != QLatin1String("submitted")
            && scope != QLatin1String("deleted")) {
            writeError(res, 400, QStringLiteral("scope 只能是 pending、submitted 或 deleted"));
            return;
        }
        writeData(res, d->api->assignmentsByScope(scope));
    });

    d->server.Get(R"(/api/courses/([^/]+)/assignments)",
                  [this](const httplib::Request &req, httplib::Response &res) {
                      const QString courseId = QString::fromUtf8(req.matches[1].str().c_str());
                      writeData(res, d->api->assignments(courseId));
                  });

    d->server.Get("/api/logs", [this](const httplib::Request &req, httplib::Response &res) {
        int lines = 200;
        if (req.has_param("tail")) {
            bool ok = false;
            const int requested = QString::fromStdString(req.get_param_value("tail")).toInt(&ok);
            if (ok) {
                lines = qBound(1, requested, 2000);
            }
        }
        QJsonObject data;
        data.insert(QStringLiteral("lines"), d->api->logTail(lines));
        writeData(res, data);
    });

    // 静态页面：路径 -> 内容
    for (auto it = d->assets.constBegin(); it != d->assets.constEnd(); ++it) {
        const QString path = it.key();
        const QByteArray body = it.value();
        const QString type = d->contentTypes.value(path);
        // Service Worker 脚本不能用 no-store：浏览器要求它能被缓存/复验，
        // 否则注册会被拒绝（这里用 no-cache = 可缓存但每次都必须回源校验）。
        const bool isServiceWorker = path == QLatin1String("/service-worker.js");
        d->server.Get(path.toStdString(),
                      [body, type, isServiceWorker](const httplib::Request &,
                                                    httplib::Response &res) {
                          res.set_content(
                              std::string(body.constData(), static_cast<size_t>(body.size())),
                              type.toStdString());
                          res.set_header("Cache-Control",
                                         isServiceWorker ? "no-cache" : "no-store");
                      });
    }

    // ---- 写操作（一律要求访问令牌）----
    //
    // 只读接口靠「只监听回环 + 仅 tailnet」两层边界就够了；但写操作能改采集设置、
    // 触发同步，所以再加一道令牌：token 在 start() 之前设置一次，之后只读，
    // 因此多线程读它没有数据竞争。
    auto authorized = [this](const httplib::Request &req, httplib::Response &res) -> bool {
        if (d->token.isEmpty()) {
            return true;   // 未配置令牌（测试）：不校验
        }
        const QString provided =
            QString::fromUtf8(req.get_header_value("X-Auth-Token").c_str()).trimmed();
        if (provided == d->token) {
            return true;
        }
        writeError(res, 401,
                   QStringLiteral("缺少或错误的访问令牌：在 PC 端托盘菜单里复制「复制访问令牌」，"
                                  "填到本页「设置 → 访问令牌」。"));
        return false;
    };

    d->server.Post("/api/settings",
                   [this, authorized](const httplib::Request &req, httplib::Response &res) {
                       if (!authorized(req, res)) {
                           return;
                       }
                       QJsonObject patch;
                       if (!readJsonBody(req, res, &patch)) {
                           return;
                       }
                       writeActionResult(res, d->api->patchSettings(patch));
                   });

    d->server.Post("/api/sync",
                   [this, authorized](const httplib::Request &req, httplib::Response &res) {
                       if (!authorized(req, res)) {
                           return;
                       }
                       QString mode = QStringLiteral("incremental");
                       if (!req.body.empty()) {
                           QJsonObject body;
                           if (!readJsonBody(req, res, &body)) {
                               return;
                           }
                           if (body.contains(QStringLiteral("mode"))) {
                               mode = body.value(QStringLiteral("mode")).toString(mode);
                           }
                       }
                       writeActionResult(res, d->api->startSync(mode));
                   });

    d->server.Post("/api/sync/pause",
                   [this, authorized](const httplib::Request &req, httplib::Response &res) {
                       if (!authorized(req, res)) {
                           return;
                       }
                       writeActionResult(res, d->api->pauseSync());
                   });

    d->server.Post("/api/sync/resume",
                   [this, authorized](const httplib::Request &req, httplib::Response &res) {
                       if (!authorized(req, res)) {
                           return;
                       }
                       writeActionResult(res, d->api->resumeSync());
                   });

    d->server.Post("/api/sync/cancel",
                   [this, authorized](const httplib::Request &req, httplib::Response &res) {
                       if (!authorized(req, res)) {
                           return;
                       }
                       writeActionResult(res, d->api->cancelSync());
                   });

    // 危险操作：清空采集数据（保留登录状态）。前端弹警告窗并勾选确认后才发这个请求，
    // 接口这里再要求一个显式的确认串，避免"手滑点到"或将来别的调用方误用。
    d->server.Post("/api/data/purge",
                   [this, authorized](const httplib::Request &req, httplib::Response &res) {
                       if (!authorized(req, res)) {
                           return;
                       }
                       QJsonObject body;
                       if (!readJsonBody(req, res, &body)) {
                           return;
                       }
                       writeActionResult(
                           res, d->api->purgeData(body.value(QStringLiteral("confirm")).toString()));
                   });

    // 单条作业的软删除 / 恢复（iPad「未提交 → 删除」「已删除 → 恢复」）。
    // 与其它写操作一样要求令牌；只改 user_deleted 标记，数据保留在库里。
    const auto assignmentKeyFromBody =
        [](const QJsonObject &body, QString *courseId, QString *sourceId) -> bool {
        *courseId = body.value(QStringLiteral("courseId")).toString().trimmed();
        *sourceId = body.value(QStringLiteral("sourceId")).toString().trimmed();
        return !courseId->isEmpty() && !sourceId->isEmpty();
    };

    d->server.Post("/api/assignments/delete",
                   [this, authorized, assignmentKeyFromBody](const httplib::Request &req,
                                                             httplib::Response &res) {
                       if (!authorized(req, res)) {
                           return;
                       }
                       QJsonObject body;
                       if (!readJsonBody(req, res, &body)) {
                           return;
                       }
                       QString courseId;
                       QString sourceId;
                       if (!assignmentKeyFromBody(body, &courseId, &sourceId)) {
                           writeError(res, 400,
                                      QStringLiteral("需要 courseId 与 sourceId 两个字段"));
                           return;
                       }
                       writeActionResult(res, d->api->deleteAssignment(courseId, sourceId));
                   });

    d->server.Post("/api/assignments/restore",
                   [this, authorized, assignmentKeyFromBody](const httplib::Request &req,
                                                             httplib::Response &res) {
                       if (!authorized(req, res)) {
                           return;
                       }
                       QJsonObject body;
                       if (!readJsonBody(req, res, &body)) {
                           return;
                       }
                       QString courseId;
                       QString sourceId;
                       if (!assignmentKeyFromBody(body, &courseId, &sourceId)) {
                           writeError(res, 400,
                                      QStringLiteral("需要 courseId 与 sourceId 两个字段"));
                           return;
                       }
                       writeActionResult(res, d->api->restoreAssignment(courseId, sourceId));
                   });

    d->server.set_exception_handler([](const httplib::Request &, httplib::Response &res,
                                       std::exception_ptr ep) {
        QString message = QStringLiteral("服务内部错误");
        try {
            if (ep != nullptr) {
                std::rethrow_exception(ep);
            }
        } catch (const std::exception &e) {
            message = QString::fromUtf8(e.what());
        } catch (...) {
        }
        qCWarning(lcWeb) << "handler threw:" << message;
        writeError(res, 500, message);
    });

    // 请求日志走项目统一的日志系统（默认 Info 级别下不输出 debug）
    d->server.set_logger([](const httplib::Request &req, const httplib::Response &res) {
        qCDebug(lcWeb) << req.method.c_str() << req.path.c_str() << res.status;
    });

    d->server.set_read_timeout(10, 0);
    d->server.set_write_timeout(10, 0);
    d->server.set_payload_max_length(64 * 1024);

    // ---- 只绑定回环地址 ----
    const char *kLoopback = "127.0.0.1";
    if (port == 0) {
        const int bound = d->server.bind_to_any_port(kLoopback);
        if (bound <= 0) {
            if (errorOut != nullptr) {
                *errorOut = QStringLiteral("无法绑定回环地址");
            }
            return false;
        }
        d->port = static_cast<quint16>(bound);
    } else {
        if (!d->server.bind_to_port(kLoopback, port)) {
            if (errorOut != nullptr) {
                *errorOut = QStringLiteral("端口 %1 已被占用（或无法绑定回环地址）").arg(port);
            }
            return false;
        }
        d->port = port;
    }

    d->running.store(true);
    d->thread = std::thread([this]() {
        qCInfo(lcWeb) << "listening on 127.0.0.1:" << d->port;
        d->server.listen_after_bind();
        d->running.store(false);
        qCInfo(lcWeb) << "stopped";
    });
    return true;
}

void WebServer::stop()
{
    if (!d->running.load() && !d->thread.joinable()) {
        return;
    }
    d->server.stop();
    if (d->thread.joinable()) {
        d->thread.join();
    }
    d->running.store(false);
}

} // namespace njulms
