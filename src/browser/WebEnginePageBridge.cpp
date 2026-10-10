#include "WebEnginePageBridge.h"

#include "utils/Logger.h"
#include "utils/Redact.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QWebChannel>
#include <QWebEngineCookieStore>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include <QWebEngineSettings>
#include <QWebEngineView>

namespace njulms {
namespace {

/// 站点源（同源校验与首页地址都以它为准）
constexpr auto kOrigin = "https://lms.nju.edu.cn";
constexpr auto kHomeUrl = "https://lms.nju.edu.cn/user/index#/";

/**
 * 首页地址。默认是智汇南雍；设置环境变量 NJULMS_HOME_URL 可以换成别的站点。
 *
 * 用途：接入新站点前先做"真机探测"——把它当首页打开、人工登录一次，
 * 再用远程调试抓真实接口（cookie 会留在同一个持久化 Profile 里）。
 * 只影响 loadHome()，站点相对路径（loadPath）仍以南大 LMS 为基准，行为不变。
 */
QString homeUrl()
{
    const QByteArray override = qgetenv("NJULMS_HOME_URL");
    return override.isEmpty() ? QString::fromLatin1(kHomeUrl) : QString::fromLocal8Bit(override);
}

/**
 * 进程级的 Profile 缓存：同一个存储路径只允许一个 QWebEngineProfile。
 *
 * ⚠️ 这是实测踩到的硬约束，不是优化：
 * 在一个进程里对**同一个存储路径**创建第二个 QWebEngineProfile 会让
 * Chromium 直接中止进程：
 *
 *     FATAL: Check failed: !HistoryPathsTracker::GetInstance()
 *
 * 触发场景很现实：再开一个主窗口。MainWindow 的构造函数会创建 Profile，
 * 于是「打开第二个窗口」= 进程崩溃。
 * （该崩溃由 tests/MainWindowSmokeTests.cpp 的 survivesRepeatedConstruction 抓到。）
 *
 * 因此这里把 Profile 提升为进程级共享对象：
 *   - 同一个路径返回同一个对象，多个窗口/页桥共用一份会话与缓存；
 *   - 由函数内静态变量持有，生命周期贯穿整个进程（Chromium 自身也这么要求）。
 *   - 页桥用 QPointer 引用它，因此不会出现悬垂指针。
 *
 * 注意：路径**不同**的多个 Profile 仍然会触发同一个 Chromium 断言，
 * 所以本程序在一个进程内只支持一个 Profile 存储路径。
 */
QHash<QString, QWebEngineProfile *> &profileCache()
{
    static QHash<QString, QWebEngineProfile *> cache;
    return cache;
}

/**
 * 取得指定路径的共享 Profile（不存在则创建）。
 * @param created 输出参数：本次是否新建了 Profile
 */
QWebEngineProfile *acquireSharedProfile(const QString &storagePath, bool *created)
{
    auto &cache = profileCache();
    const QString key = QDir::cleanPath(storagePath);

    if (QWebEngineProfile *existing = cache.value(key, nullptr)) {
        if (created != nullptr) {
            *created = false;
        }
        return existing;
    }

    // 无父对象：生命周期由缓存（进程级）持有
    auto *profile = new QWebEngineProfile(QStringLiteral("njulms"), nullptr);
    profile->setPersistentStoragePath(storagePath);
    profile->setCachePath(QDir(storagePath).filePath(QStringLiteral("cache")));
    profile->setHttpCacheType(QWebEngineProfile::DiskHttpCache);
    profile->setPersistentCookiesPolicy(QWebEngineProfile::ForcePersistentCookies);

    cache.insert(key, profile);
    if (created != nullptr) {
        *created = true;
    }
    return profile;
}

/// 认证相关主机。用于判断是否停留在登录流程中。
/// 这三个主机来自 doc/login.md 实测的三段式认证链。
const QStringList &authHosts()
{
    static const QStringList hosts = {
        QStringLiteral("authserver.nju.edu.cn"),
        QStringLiteral("lms-identity.nju.edu.cn"),
    };
    return hosts;
}

} // namespace

// ---------------------------------------------------------------------------
// 回传通道：页面 JS 通过它把 fetch 结果送回 C++
// ---------------------------------------------------------------------------

/**
 * QWebChannel 的传输对象。
 *
 * runJavaScript 无法直接返回 Promise 的结果，因此页面脚本在 fetch 完成后
 * 主动调用本对象的槽函数把结果推回 C++。
 */
class PageBridgeTransport : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

    void setHandler(std::function<void(const QString &)> handler)
    {
        m_handler = std::move(handler);
    }

public slots:
    /// 由页面脚本调用。参数是 JSON 字符串：
    /// {"id": "...", "ok": true, "status": 200, "body": "..."}
    void deliver(const QString &payload)
    {
        if (m_handler) {
            m_handler(payload);
        }
    }

private:
    std::function<void(const QString &)> m_handler;
};

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

class WebEnginePageBridge::Impl
{
public:
    /// 进程级共享的 Profile 弱引用。
    ///
    /// **不拥有**它：对象由 acquireSharedProfile() 的进程级缓存持有。
    /// 用 QPointer 而非裸指针，以免将来有人误删缓存中的对象后这里变悬垂。
    QPointer<QWebEngineProfile> profile = nullptr;
    QWebEnginePage *page = nullptr;
    QWebEngineView *view = nullptr;
    QWebChannel *channel = nullptr;
    PageBridgeTransport *transport = nullptr;
    bool ready = false;

    /// requestId -> 回调
    QHash<QString, Callback> pending;
    int nextRequestId = 0;

    /// 缓存的注入脚本（qwebchannel.js 与 fetch 包装器）
    QString channelScript;
    QString wrapperScript;
    bool scriptsLoaded = false;
};

// ---------------------------------------------------------------------------
// 构造 / 析构
// ---------------------------------------------------------------------------

WebEnginePageBridge::WebEnginePageBridge(QObject *parent)
    : IPageBridge(parent), d(std::make_unique<Impl>())
{
    d->view = new QWebEngineView();
    d->page = new QWebEnginePage(d->view);
    d->view->setPage(d->page);

    // QWebChannel 用于页面 → C++ 的结果回传
    d->channel = new QWebChannel(this);
    d->transport = new PageBridgeTransport(this);
    d->channel->registerObject(QStringLiteral("njulmsBridge"), d->transport);
    d->page->setWebChannel(d->channel);

    d->transport->setHandler([this](const QString &payload) {
        const QJsonDocument doc = QJsonDocument::fromJson(payload.toUtf8());
        if (!doc.isObject()) {
            qCWarning(lcBrowser) << "malformed payload from page transport";
            return;
        }
        const QJsonObject obj = doc.object();
        const QString id = obj.value(QStringLiteral("id")).toString();

        const auto it = d->pending.find(id);
        if (it == d->pending.end()) {
            qCWarning(lcBrowser) << "transport delivered an unknown request id";
            return;
        }
        const Callback callback = it.value();
        d->pending.erase(it);

        Response res;
        res.transportOk = obj.value(QStringLiteral("ok")).toBool(false);
        res.httpStatus = obj.value(QStringLiteral("status")).toInt(0);
        res.body = obj.value(QStringLiteral("body")).toString();
        res.error = obj.value(QStringLiteral("error")).toString();

        // 注意：这里只记录状态与长度，**不记录响应正文**，
        // 避免把作业内容或认证页面内容写进日志。
        qCDebug(lcBrowser) << "page request finished; status:" << res.httpStatus
                           << "transportOk:" << res.transportOk << "bytes:" << res.body.size();
        callback(res);
    });

    // 地址变化：只对外发送脱敏后的地址
    connect(d->page, &QWebEnginePage::urlChanged, this, [this](const QUrl &url) {
        const QString safe = redact::url(url);
        emit safeUrlChanged(safe);

        const QString host = url.host();
        if (authHosts().contains(host)) {
            emit loginPageReached();
            qCInfo(lcBrowser) << "reached authentication host; waiting for manual login";
        }
    });

    connect(d->page, &QWebEnginePage::loadFinished, this, [this](bool ok) {
        d->ready = ok;
        emit loadFinished(ok, redact::url(d->page->url()));
        if (ok) {
            // 预注入包装脚本，使首次请求无需额外往返
            ensurePageScripts();
        }
    });

    // 页面被销毁（例如窗口关闭）时通知上层
    connect(d->page, &QObject::destroyed, this, [this]() {
        d->ready = false;
        emit becameUnavailable(QStringLiteral("page destroyed"));
    });

    // 初始页面（默认 Profile）也要注入通道脚本，
    // 否则在 initialize() 之前发起的请求同样无法回传结果。
    installTransportScript();
}

WebEnginePageBridge::~WebEnginePageBridge()
{
    // view 没有父对象，需要手工释放
    delete d->view;
    d->view = nullptr;
}

// ---------------------------------------------------------------------------
// 初始化
// ---------------------------------------------------------------------------

QString WebEnginePageBridge::defaultProfileDir()
{
    const QByteArray env = qgetenv("NJULMS_PROFILE_DIR");
    if (!env.isEmpty()) {
        return QString::fromLocal8Bit(env);
    }
    if (QCoreApplication::instance() != nullptr) {
        const QString appDir = QCoreApplication::applicationDirPath();
        if (!appDir.isEmpty()) {
            return QDir(appDir).filePath(QStringLiteral("profile"));
        }
    }
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    return QDir(base).filePath(QStringLiteral("profile"));
}

bool WebEnginePageBridge::initialize(const QString &profileDir, QString *error)
{
    const auto fail = [&](const QString &message) {
        if (error != nullptr) {
            *error = message;
        }
        qCCritical(lcBrowser) << "profile initialization failed:" << message;
        emit profileError(message);
        return false;
    };

    const QString dir = profileDir.isEmpty() ? defaultProfileDir() : profileDir;
    if (!QDir().mkpath(dir)) {
        return fail(QStringLiteral("cannot create profile directory: %1").arg(dir));
    }

    // 持久化 Profile：会话数据写入磁盘，重启后仍可复用。
    //
    // ⚠️ 必须走进程级共享缓存：同一路径创建第二个 QWebEngineProfile 会让
    // Chromium 直接中止进程（FATAL: !HistoryPathsTracker::GetInstance()）。
    // 见 acquireSharedProfile() 的说明。
    bool createdNow = false;
    d->profile = acquireSharedProfile(dir, &createdNow);
    if (d->profile == nullptr) {
        return fail(QStringLiteral("cannot create the browser profile"));
    }

    if (createdNow) {
        // ⚠️ 已知限制：统一身份认证链里的 CASTGC / KEYCLOAK_IDENTITY 属于
        // 「会话级 Cookie」。Chromium 在关闭时会丢弃它们，即使
        // ForcePersistentCookies 也无法可靠保留（既有实测记录了这一现象）。
        // 因此程序重启后可能仍需人工登录一次。这是站点策略导致的，
        // 本项目不做任何绕过处理，只在 README 与 UI 上明确说明。
        qCInfo(lcBrowser) << "persistent profile created; directory:"
                          << QFileInfo(dir).fileName();  // 只记目录名，不记完整路径

        // 只观察 Cookie 名称用于诊断，**绝不记录 Cookie 值**
        connect(d->profile->cookieStore(), &QWebEngineCookieStore::cookieAdded, this,
                [](const QNetworkCookie &cookie) {
                    // 注意：QNetworkCookie::name() 返回 QByteArray；本项目定义了
                    // QT_NO_CAST_FROM_ASCII，因此必须显式转换，不能用隐式构造。
                    const QString name = QString::fromUtf8(cookie.name());
                    if (name.compare(QLatin1String("session"), Qt::CaseInsensitive) == 0) {
                        // 站点会话 Cookie 每次访问顺延 24 小时，这里只提示它出现了
                        qCDebug(lcBrowser) << "session cookie observed";
                    }
                });
    } else {
        qCDebug(lcBrowser) << "reusing the existing browser profile for this storage path";
    }

    // 站点是 SPA，需要 JS 与本地存储
    QWebEngineSettings *settings = d->profile->settings();
    settings->setAttribute(QWebEngineSettings::JavascriptEnabled, true);
    settings->setAttribute(QWebEngineSettings::LocalStorageEnabled, true);
    settings->setAttribute(QWebEngineSettings::JavascriptCanOpenWindows, true);
    // 不做任何反检测处理：本项目不隐藏自动化痕迹、不篡改 UA。

    // ---- 注入 QWebChannel 客户端库 ----
    //
    // 必须在页面创建时就注入 qwebchannel.js，否则页面里没有 QWebChannel
    // 全局对象，也就无法建立 qt.webChannelTransport，fetch 的结果永远回不到 C++。
    // 实测症状：请求发出后 runJavaScript 返回 'no-webchannel'，调用方一直等超时。
    installTransportScript();

    // 用持久化 Profile 重建页面
    //
    // ⚠️ 必须先断开旧页面的信号再删除它。
    // 旧页面上连着 "page destroyed" → becameUnavailable 的处理，
    // 若不先断开，这里的 delete 会立刻发出
    // 「page bridge became unavailable: page destroyed」，
    // 而实际上页桥马上就会被新页面替换掉 —— 这是个纯粹的误报，
    // 却会让适配器/上层以为浏览器不可用（实测在 GUI 冒烟测试中观察到）。
    if (d->page != nullptr) {
        disconnect(d->page, nullptr, this, nullptr);
    }
    delete d->page;
    d->page = new QWebEnginePage(d->profile, d->view);
    d->view->setPage(d->page);
    d->page->setWebChannel(d->channel);
    d->ready = false;   // 新页面尚未加载完成

    connect(d->page, &QWebEnginePage::loadFinished, this, [this](bool ok) {
        d->ready = ok;
        emit loadFinished(ok, redact::url(d->page->url()));
        if (ok) {
            ensurePageScripts();
        }
    });
    connect(d->page, &QWebEnginePage::urlChanged, this, [this](const QUrl &url) {
        emit safeUrlChanged(redact::url(url));
        const QString host = url.host();
        if (authHosts().contains(host)) {
            emit loginPageReached();
        }
    });

    return true;
}

QWebEngineView *WebEnginePageBridge::view() const
{
    return d->view;
}

QWebEngineProfile *WebEnginePageBridge::profile() const
{
    return d->profile.data();
}

void WebEnginePageBridge::loadHome()
{
    loadPath(QString());
}

void WebEnginePageBridge::loadPath(const QString &path)
{
    if (d->page == nullptr) {
        return;
    }
    const QUrl url = path.isEmpty() ? QUrl(homeUrl())
                                    : QUrl(QString::fromLatin1(kOrigin) + path);
    qCInfo(lcBrowser) << "navigating to" << redact::url(url);
    d->page->setUrl(url);
}

bool WebEnginePageBridge::looksAuthenticated() const
{
    if (d->page == nullptr) {
        return false;
    }
    const QUrl url = d->page->url();
    return url.host() == QLatin1String("lms.nju.edu.cn") && !isOnAuthHost();
}

bool WebEnginePageBridge::isOnAuthHost() const
{
    if (d->page == nullptr) {
        return false;
    }
    return authHosts().contains(d->page->url().host());
}

QString WebEnginePageBridge::currentUrl() const
{
    return d->page != nullptr ? d->page->url().toString() : QString();
}

bool WebEnginePageBridge::isReady() const
{
    return d->ready && d->page != nullptr;
}

void WebEnginePageBridge::installTransportScript()
{
    // 把 qwebchannel.js 与 fetch 包装器读进内存缓存，稍后用 runJavaScript 注入。
    //
    // 为什么不用 QWebEngineScript(WorldId = MainWorld)？
    // 实测：即使按 DocumentCreation + MainWorld 插入，页面里仍然没有
    // QWebChannel 全局对象，且 qt.webChannelTransport.onmessage 永远收不到消息
    // （页面自检输出 {"hasQWebChannel":false,...,"rawMessages":0}）。
    // 插入失败的原因不明，因此改用**可验证**的路径：在 loadFinished 之后用
    // runJavaScript 直接执行源码，注入成功与否在页面里一目了然。
    QFile js(QStringLiteral(":/qtwebchannel/qwebchannel.js"));
    if (!js.open(QIODevice::ReadOnly)) {
        const QString message =
            QStringLiteral("cannot open :/qtwebchannel/qwebchannel.js - the QWebChannel client "
                           "library is missing, so page results cannot be delivered to C++");
        qCCritical(lcBrowser) << message;
        emit becameUnavailable(message);
        return;
    }
    d->channelScript = QString::fromUtf8(js.readAll());
    d->wrapperScript = requestScript();
    d->scriptsLoaded = true;
    qCDebug(lcBrowser) << "channel client cached:" << d->channelScript.size() << "bytes";
}

void WebEnginePageBridge::ensurePageScripts()
{
    if (!d->scriptsLoaded || d->page == nullptr) {
        return;
    }
    // 两段脚本都是幂等的：
    //   qwebchannel.js 定义了 QWebChannel 就不会重复定义；
    //   包装器内部有 __njulmsFetchInstalled 守卫。
    // 因此可以在每次导航后安全地重复调用。
    d->page->runJavaScript(d->channelScript, [](const QVariant &) {});
    d->page->runJavaScript(d->wrapperScript, [](const QVariant &) {});
    qCDebug(lcBrowser) << "page scripts injected via runJavaScript";
}

// ---------------------------------------------------------------------------
// 请求：把 fetch 注入页面执行，经 QWebChannel 回传结果
// ---------------------------------------------------------------------------

QString WebEnginePageBridge::requestScript()
{
    // 该脚本在页面上下文里执行，因此自带浏览器的真实会话 Cookie。
    //
    // ── 关于结果回传通道 ────────────────────────────────────────────────────
    // 实测结论（一步一步验证出来的）：
    //   1. 用 QWebEngineScript(DocumentCreation + MainWorld) 注入
    //      :/qtwebchannel/qwebchannel.js **不会生效** —— 页面自检显示
    //      hasQWebChannel = false，注入的代码根本没有执行。
    //   2. 改用 runJavaScript 执行同一份源码后就正常了（hasQWebChannel = true）。
    //      因此 qwebchannel.js 由 ensurePageScripts() 用 runJavaScript 注入。
    //   3. 既然官方客户端可用，就**不要**自己实现 WebChannel 线协议：
    //      手写的 invokeMethod 版本实测收不到任何回执（rawMessages 恒为 0），
    //      因为通道只有在客户端完整构造后才会开始投递消息。
    //      这里改为等待 window.__njulmsChannel 就绪后再调用其 deliver。
    //
    // ── 安全约束 ────────────────────────────────────────────────────────────
    // 只接受同源路径，拒绝绝对 URL；只回传状态码与正文，绝不回传 Cookie / 令牌。
    //
    // ⚠️ 实现约束：用「多行普通字面量拼接」而不是原始字符串 R"(...)"。
    // 本文件含 Q_OBJECT，会被 moc 预处理；moc 会把字面量里的换行替换为空格
    // （它需要在字面量内识别 #define），导致字面量之后的代码错位，
    // 再叠加 moc 的单 token 上限，最终 moc 报 "No relevant classes found"
    // 并生成 0 字节 .moc，只在链接期才暴露为未解析符号。
    // 详见 docs/01-实现决策.md。
    return QStringLiteral(
        "(function () {\n"
        "  if (window.__njulmsFetchInstalled) { return 'already-installed'; }\n"
        "  window.__njulmsFetchInstalled = true;\n"
        "\n"
        "  window.__njulmsChannelReady = false;\n"
        "\n"
        "  window.__njulmsEnsureChannel = function () {\n"
        "    if (window.__njulmsChannelReady) { return true; }\n"
        "    if (typeof QWebChannel === 'undefined') { return false; }\n"
        "    if (!window.qt || !window.qt.webChannelTransport) { return false; }\n"
        "    new QWebChannel(window.qt.webChannelTransport, function (channel) {\n"
        "      window.__njulmsChannel = channel;\n"
        "      window.__njulmsChannelReady = true;\n"
        "    });\n"
        "    return false;\n"
        "  };\n"
        "\n"
        "  // 通道构造是异步的：等它就绪后再发请求。\n"
        "  window.__njulmsAwaitChannel = function () {\n"
        "    return new Promise(function (resolve) {\n"
        "      if (window.__njulmsEnsureChannel()) { resolve(true); return; }\n"
        "      var tries = 0;\n"
        "      var timer = setInterval(function () {\n"
        "        tries += 1;\n"
        "        if (window.__njulmsEnsureChannel()) {\n"
        "          clearInterval(timer); resolve(true);\n"
        "        } else if (tries > 100) {\n"
        "          clearInterval(timer); resolve(false);\n"
        "        }\n"
        "      }, 50);\n"
        "    });\n"
        "  };\n"
        "\n"
        "  window.__njulmsRequest = function (id, path, method, body) {\n"
        "    window.__njulmsAwaitChannel().then(function (ready) {\n"
        "      var send = function (payload) {\n"
        "        if (!ready) { return; }\n"
        "        var text = JSON.stringify(payload);\n"
        "        var bridge = window.__njulmsChannel.objects.njulmsBridge;\n"
        "        if (bridge && bridge.deliver) { bridge.deliver(text); }\n"
        "      };\n"
        "\n"
        "      if (!ready) {\n"
        "        send({ id: id, ok: false, status: 0, body: '',\n"
        "               error: 'result channel did not become ready' });\n"
        "        return;\n"
        "      }\n"
        "\n"
        "      var options = {\n"
        "        method: method || 'GET',\n"
        "        credentials: 'same-origin',\n"
        "        headers: { 'Accept': 'application/json, text/plain, */*' }\n"
        "      };\n"
        "      if (body !== null && body !== undefined && body !== '') {\n"
        "        options.headers['Content-Type'] = 'application/json;charset=UTF-8';\n"
        "        options.body = body;\n"
        "      }\n"
        "\n"
        "      fetch(path, options).then(function (response) {\n"
        "        return response.text().then(function (text) {\n"
        "          send({ id: id, ok: true, status: response.status, body: text, error: '' });\n"
        "        });\n"
        "      }).catch(function (err) {\n"
        "        send({ id: id, ok: false, status: 0, body: '',\n"
        "               error: 'fetch failed: ' + (err && err.message ? err.message : String(err)) });\n"
        "      });\n"
        "    });\n"
        "  };\n"
        "\n"
        "  return 'installed';\n"
        "})();\n");
}

void WebEnginePageBridge::request(const Request &req, Callback callback)
{
    if (d->page == nullptr || !d->ready) {
        Response res;
        res.transportOk = false;
        res.error = QStringLiteral("page is not ready");
        callback(res);
        return;
    }

    // 同源约束：只接受以 '/' 开头的路径
    if (req.path.isEmpty() || !req.path.startsWith(QLatin1Char('/'))) {
        Response res;
        res.transportOk = false;
        res.error = QStringLiteral("only same-origin paths starting with '/' are allowed");
        qCWarning(lcBrowser) << "rejected a non-same-origin request path";
        callback(res);
        return;
    }

    const QString id = QStringLiteral("r%1").arg(++d->nextRequestId);
    d->pending.insert(id, std::move(callback));

    // 由 requestScript() 安装的包装器自行建立通道并执行 fetch，
    // 因此这里只负责把参数传进去。
    // 同样使用拼接字面量而非原始字符串（原因见 requestScript 的注释）。
    const QString bootstrap =
        QStringLiteral(
            "(function () {\n"
            "  if (typeof window.__njulmsRequest !== 'function') {\n"
            "    return 'wrapper-missing';\n"
            "  }\n"
            "  if (!window.qt || !window.qt.webChannelTransport) {\n"
            "    return 'transport-missing';\n"
            "  }\n"
            "  window.__njulmsRequest(%1, %2, %3, %4);\n"
            "  return 'started';\n"
            "})();\n")
            .arg(QStringLiteral("'%1'").arg(id),
                 QStringLiteral("'%1'").arg(req.path),
                 QStringLiteral("'%1'").arg(req.method == Request::Method::Post
                                               ? QStringLiteral("POST")
                                               : QStringLiteral("GET")),
                 req.jsonBody.isEmpty()
                     ? QStringLiteral("null")
                     : QStringLiteral("'%1'").arg(
                           QString(req.jsonBody)
                               .replace(QLatin1Char('\\'), QLatin1String("\\\\"))
                               .replace(QLatin1Char('\''), QLatin1String("\\'"))));

    d->page->runJavaScript(bootstrap, [this, id](const QVariant &result) {
        const QString status = result.toString();
        if (status == QLatin1String("started")) {
            return;  // 已发出，结果稍后经通道回传
        }
        // 包装器或通道不可用：立即失败，而不是让调用方一直等到超时
        const auto it = d->pending.find(id);
        if (it != d->pending.end()) {
            const Callback callback = it.value();
            d->pending.erase(it);
            Response res;
            res.transportOk = false;
            res.error = status == QLatin1String("transport-missing")
                            ? QStringLiteral("qt.webChannelTransport is not available in the page")
                            : QStringLiteral("the page request wrapper was not installed");
            qCWarning(lcBrowser) << "request bootstrap failed:" << res.error;
            callback(res);
        }
    });

    // 兜底超时：页面崩溃或通道断开时不能让上层永久等待
    QTimer::singleShot(60000, this, [this, id]() {
        const auto it = d->pending.find(id);
        if (it == d->pending.end()) {
            return;
        }
        const Callback callback = it.value();
        d->pending.erase(it);
        Response res;
        res.transportOk = false;
        res.error = QStringLiteral("request timed out after 60s");
        qCWarning(lcBrowser) << "page request timed out";
        callback(res);
    });
}

} // namespace njulms

#include "WebEnginePageBridge.moc"
