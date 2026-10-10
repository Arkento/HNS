#include "MainWindow.h"

#include "auth/AuthManager.h"
#include "adapters/ChaoxingAdapter.h"
#include "parser/AssignmentParser.h"
#include "sync/ChaoxingSyncRunner.h"
#include "ui/UiTheme.h"
#include "utils/Logger.h"
#include "utils/Redact.h"
#include "utils/TimeUtils.h"
#include "web/WebApi.h"
#include "web/WebServer.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QSpinBox>
#include <QStyle>
#include <QStatusBar>
#include <QStringList>
#include <QSystemTrayIcon>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextDocument>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWebEngineView>
#include <QRandomGenerator>
#include <QRegularExpression>

namespace njulms {
namespace {

// ---------------------------------------------------------------------------
// 用户偏好（v2：启动全量扫描开关）
// ---------------------------------------------------------------------------

/// 设置组织/应用名（与 main.cpp 里设置的一致；显式给出，测试环境也能工作）
constexpr auto kSettingsOrganization = "NjuLmsCrawler";
constexpr auto kSettingsApplication = "NjuLmsCrawler";
/// 设置键：启动时是否自动做一次完整全量扫描
constexpr auto kStartupFullScanKey = "sync/startupFullScan";
/// 设置键：定时同步开关 / 周期（分钟）/ 快速增量
constexpr auto kScheduleEnabledKey = "sync/scheduleEnabled";
constexpr auto kScheduleIntervalKey = "sync/scheduleIntervalMinutes";
constexpr auto kQuickIncrementalKey = "sync/quickIncremental";
/// 启动扫描的性能保护：距上次**成功全量扫描**不足这个分钟数就跳过，
/// 避免"刚扫完就重启"造成重复的一整轮全量请求。
constexpr int kStartupFullScanMinGapMinutes = 5;

/// iPad 前端（只读 HTTP 服务）默认端口；0 或负数表示关闭
constexpr int kDefaultWebPort = 8790;
/// 设置键：前端端口（0 = 关闭）
constexpr auto kWebPortKey = "web/port";
/// 设置键：前端写操作访问令牌（首次启动时生成）
constexpr auto kWebTokenKey = "web/token";
/// 设置键：是否用活动接口补全作业说明（每门课多 1 个请求，只补正文不改记录）
constexpr auto kEnrichDescriptionsKey = "sync/enrichDescriptions";

QSettings makeAppSettings()
{
    return QSettings(QString::fromLatin1(kSettingsOrganization),
                     QString::fromLatin1(kSettingsApplication));
}

// ---------------------------------------------------------------------------
// 样式属性助手（v2）
//
// 界面只声明"这个控件扮演什么角色"（语义属性），具体颜色由 UiTheme 的令牌决定。
// 这样深浅色主题可以整体切换，也不会再出现硬编码颜色在深色下看不清的问题。
// ---------------------------------------------------------------------------

void setStyleProperty(QWidget *widget, const char *name, const char *value)
{
    if (widget == nullptr) {
        return;
    }
    widget->setProperty(name, QString::fromLatin1(value));
    // 动态属性变化后必须重新求值样式，否则不会重绘
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}

void setLabelRole(QLabel *label, const char *role)
{
    setStyleProperty(label, "role", role);
}

void setLabelState(QLabel *label, const char *state)
{
    setStyleProperty(label, "state", state);
}

void setButtonVariant(QPushButton *button, const char *variant)
{
    setStyleProperty(button, "variant", variant);
}

/// 等宽字体（来源 ID、时间列、日志用），比正文略小半号
QFont monospaceFont()
{
    QFont font(uitheme::monospaceFamily());
    const qreal size = QApplication::font().pointSizeF();
    if (size > 0) {
        font.setPointSizeF(size - 0.5);
    }
    return font;
}

// ---------------------------------------------------------------------------
// iPad 前端（只读 HTTP 服务）与托盘常驻：辅助函数
// ---------------------------------------------------------------------------

/// 运行记录状态的中文文案（桌面端与前端共用一处定义）
QString runStatusDisplay(CrawlRun::Status status)
{
    switch (status) {
    case CrawlRun::Status::Running:   return QStringLiteral("进行中");
    case CrawlRun::Status::Success:   return QStringLiteral("成功");
    case CrawlRun::Status::Partial:   return QStringLiteral("部分完成");
    case CrawlRun::Status::Failed:    return QStringLiteral("失败");
    case CrawlRun::Status::NeedLogin: return QStringLiteral("等待重新登录");
    case CrawlRun::Status::Cancelled: return QStringLiteral("已取消");
    }
    return QString();
}

/// 托盘图标：用当前主题的强调色画一个圆角方块（不引入图片资源）
QIcon makeAppIcon()
{
    constexpr int kSize = 64;
    QPixmap pixmap(kSize, kSize);
    pixmap.fill(Qt::transparent);

    const uitheme::Tokens &t = uitheme::tokens();
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(t.accent);
    painter.drawRoundedRect(QRectF(4, 4, kSize - 8, kSize - 8), 16, 16);
    painter.setPen(t.accentText);
    QFont font = QApplication::font();
    font.setPixelSize(34);
    font.setBold(true);
    painter.setFont(font);
    painter.drawText(QRectF(0, 0, kSize, kSize), Qt::AlignCenter, QStringLiteral("南"));
    painter.end();
    return QIcon(pixmap);
}

/**
 * 探测 iPad 的访问地址。
 *
 * 有 Tailscale 时用 MagicDNS 名字（https://<机器>.<tailnet>.ts.net/），
 * 也就是 `tailscale serve` 对外提供的那个入口；取不到就退回回环地址，
 * 说明还没配 serve（那时只有 PC 自己能打开这个页面）。
 */
QString detectPublicUrl(int port)
{
    const QStringList candidates = {
        QStringLiteral("tailscale"),
        QStringLiteral("C:/Program Files/Tailscale/tailscale.exe"),
        QStringLiteral("D:/app/Tailscale/tailscale.exe"),
    };
    for (const QString &program : candidates) {
        QProcess process;
        process.start(program, {QStringLiteral("status"), QStringLiteral("--json")});
        if (!process.waitForStarted(2000) || !process.waitForFinished(4000)) {
            process.kill();
            process.waitForFinished(500);
            continue;
        }
        const QJsonObject self = QJsonDocument::fromJson(process.readAllStandardOutput())
                                     .object()
                                     .value(QStringLiteral("Self"))
                                     .toObject();
        QString dnsName = self.value(QStringLiteral("DNSName")).toString();
        if (dnsName.endsWith(QLatin1Char('.'))) {
            dnsName.chop(1);
        }
        if (!dnsName.isEmpty()) {
            return QStringLiteral("https://%1/").arg(dnsName);
        }
    }
    return QStringLiteral("http://127.0.0.1:%1/").arg(port);
}

// ---------------------------------------------------------------------------
// 登录窗口（v2）
// ---------------------------------------------------------------------------

/**
 * 承载浏览器页面的独立顶层窗口。
 *
 * 为什么独立成窗口：日常只需要"程序在后台拿着会话取数"，把浏览器嵌在主界面里
 * 既占地方又没必要。默认隐藏；只有会话失效、需要人工完成统一身份认证时才弹出，
 * 登录成功后自动隐藏（见 MainWindow::showLoginWindow / hideLoginWindow）。
 *
 * 关闭按钮只隐藏、不销毁：页面与会话要继续为后台取数服务。
 */
class LoginWindow : public QWidget
{
public:
    explicit LoginWindow(QWidget *parent = nullptr) : QWidget(parent, Qt::Window)
    {
        setWindowTitle(QStringLiteral("登录 · 智汇南雍（完成后窗口会自动隐藏）"));
        resize(1024, 720);
    }

protected:
    void closeEvent(QCloseEvent *event) override
    {
        event->ignore();   // 只隐藏，不销毁
        hide();
    }
};

/// 作业表格列
enum Column {
    ColStatus = 0,
    ColKind,
    ColTitle,
    ColDue,
    ColDueRelative,
    ColPublished,
    ColScore,
    ColSourceId,
    ColSource,
    ColumnCount,
};

/// 来源键 → 界面标签（多站点混排时显示在课程表 / 作业表 / iPad 卡片上）
QString sourceDisplay(const QString &source)
{
    if (source == QLatin1String("nju-lms")) {
        return QStringLiteral("南大");
    }
    if (source == QLatin1String("chaoxing")) {
        return QStringLiteral("学习通");
    }
    return source.isEmpty() ? QStringLiteral("未知") : source;
}

QString formatTime(const Observed<QDateTime> &value)
{
    if (value.hasValue()) {
        return timeutil::toBeijingDisplay(value.require());
    }
    switch (value.state()) {
    case FieldState::Absent:  return QStringLiteral("（接口为空）");
    case FieldState::Missing: return QStringLiteral("（未提供）");
    case FieldState::Invalid: return QStringLiteral("（解析失败）");
    case FieldState::Present: break;
    }
    return QString();
}

QString formatScore(const Observed<double> &value)
{
    if (value.hasValue()) {
        return QString::number(value.require(), 'g', 6);
    }
    return value.state() == FieldState::Absent ? QStringLiteral("（未公布）")
                                               : QStringLiteral("（未知）");
}

/// 说明文本上限：个别作业正文很长，截断以免首屏被拖慢
constexpr int kMaxDescriptionChars = 4000;

/**
 * HTML 片段 → 纯文本。
 *
 * 为什么要转：作业说明来自站点（含 span/style 的 HTML 片段）。原样注进前端页面
 * 等于把第三方内容当代码渲染；转成纯文本后只保留换行与列表结构，既不丢信息也安全。
 */
QString htmlToPlainText(const QString &html)
{
    if (html.isEmpty()) {
        return QString();
    }
    QTextDocument document;
    document.setHtml(html);
    QString text = document.toPlainText();
    text.replace(QChar(0x00a0), QLatin1Char(' '));
    text.replace(QRegularExpression(QStringLiteral("[ \\t]+\\n")), QStringLiteral("\n"));
    text.replace(QRegularExpression(QStringLiteral("\\n{3,}")), QStringLiteral("\n\n"));
    return text.trimmed();
}

/// 一条作业 → 前端 JSON（两个 tab 共用同一套字段与文案）
QJsonObject assignmentRow(const Course &course, const Assignment &item, bool withCourseName,
                          bool withDescription)
{
    QString statusText = QStringLiteral("未知");
    if (item.status.hasValue()) {
        statusText = assignmentStatusDisplay(item.status.require());
    }

    QString relative;
    if (item.dueAt.hasValue()) {
        relative = timeutil::describeRelativeToNow(item.dueAt.require());
    }

    QJsonObject row;
    if (withCourseName) {
        row.insert(QStringLiteral("courseId"), course.courseId);
        row.insert(QStringLiteral("courseName"), course.courseName);
    }
    row.insert(QStringLiteral("title"),
               item.title.hasValue() ? item.title.require() : QStringLiteral("（标题未获取）"));
    row.insert(QStringLiteral("status"), statusText);
    row.insert(QStringLiteral("dueAt"), formatTime(item.dueAt));
    row.insert(QStringLiteral("dueRelative"), relative);
    row.insert(QStringLiteral("publishedAt"), formatTime(item.publishedAt));
    row.insert(QStringLiteral("score"), formatScore(item.score));
    row.insert(QStringLiteral("sourceId"), item.sourceId);
    // 多站点：来源键（机器读） + 来源标签（直接显示）
    row.insert(QStringLiteral("source"), course.source);
    row.insert(QStringLiteral("sourceLabel"), sourceDisplay(course.source));
    // 条目类型：作业 / 考试（考试接入）
    row.insert(QStringLiteral("kind"), item.kind);
    row.insert(QStringLiteral("kindLabel"), assignmentKindDisplay(item.kind));

    // 原站详情链接（前端那个「↗」按钮用）。
    // 只放行 https + 已知站点域名（南大 / 学习通）：详情链接是按站点路由
    // 推导或从页面提取的，万一哪天解析出别的域名，也不该变成前端一个
    // "点一下就跳走"的任意链接。
    if (item.detailUrl.hasValue()) {
        const QUrl url(item.detailUrl.require());
        const QString host = url.host();
        const bool allowedHost =
            host == QLatin1String("nju.edu.cn") || host.endsWith(QLatin1String(".nju.edu.cn"))
            || host == QLatin1String("chaoxing.com")
            || host.endsWith(QLatin1String(".chaoxing.com"));
        if (url.isValid() && url.scheme() == QLatin1String("https") && allowedHost) {
            row.insert(QStringLiteral("detailUrl"), url.toString());
        }
    }

    if (withDescription) {
        const QString description =
            item.description.hasValue() ? htmlToPlainText(item.description.require()) : QString();
        row.insert(QStringLiteral("descriptionTruncated"),
                   description.size() > kMaxDescriptionChars);
        row.insert(QStringLiteral("descriptionText"),
                   description.size() > kMaxDescriptionChars
                       ? description.left(kMaxDescriptionChars)
                       : description);
    }
    return row;
}

} // namespace

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

class MainWindow::Impl
{
public:
    // 核心模块
    std::unique_ptr<SqliteRepository> repository;
    std::unique_ptr<WebEnginePageBridge> bridge;
    std::unique_ptr<NjuLmsAdapter> adapter;
    std::unique_ptr<SyncManager> sync;
    std::unique_ptr<Scheduler> scheduler;
    std::unique_ptr<AuthManager> auth;
    /// 学习通（超星）：页面驱动采集，与主页面共享同一个持久化 Profile
    std::unique_ptr<ChaoxingAdapter> chaoxing;
    std::unique_ptr<ChaoxingSyncRunner> chaoxingSync;

    // 界面
    QTabWidget *tabs = nullptr;

    // 采集标签页
    QLabel *authStatus = nullptr;
    QPushButton *openSiteButton = nullptr;
    QPushButton *checkSessionButton = nullptr;
    /// 学习通登录状态（与南大同一格式：站点名 + 状态 + 两个按钮）
    QLabel *chaoxingAuthStatus = nullptr;
    QPushButton *openChaoxingButton = nullptr;
    QPushButton *checkChaoxingButton = nullptr;
    QPushButton *fullScanButton = nullptr;
    QPushButton *incrementalButton = nullptr;
    QPushButton *pauseButton = nullptr;
    QPushButton *resumeButton = nullptr;
    QPushButton *cancelButton = nullptr;
    QProgressBar *progress = nullptr;
    QLabel *progressLabel = nullptr;
    QLabel *lastRunLabel = nullptr;

    // 定时同步
    QCheckBox *scheduleEnabled = nullptr;
    QSpinBox *scheduleMinutes = nullptr;
    QLabel *nextRunLabel = nullptr;
    /// v2：快速增量开关（用待办接口做变更信号，只深扫变化的课程）
    QCheckBox *quickIncremental = nullptr;
    /// v2：启动时自动全量扫描开关
    QCheckBox *startupFullScan = nullptr;

    // 数据标签页
    QTableWidget *courseTable = nullptr;
    QComboBox *courseSelector = nullptr;
    QTableWidget *assignmentTable = nullptr;
    QLabel *assignmentSummary = nullptr;
    /// v2：空状态提示（无数据时显示，替代空白表格）
    QLabel *courseEmptyHint = nullptr;
    QLabel *assignmentEmptyHint = nullptr;
    /// v2：清空采集数据（保留登录状态）
    QPushButton *clearDataButton = nullptr;

    // v2：登录窗口（浏览器页面独立成窗口，默认隐藏）
    LoginWindow *loginWindow = nullptr;
    /// 学习通登录窗口（扫码后才弹出；登录成功后自动隐藏）
    LoginWindow *chaoxingLoginWindow = nullptr;

    // 日志标签页
    QPlainTextEdit *logView = nullptr;
    QCheckBox *autoScroll = nullptr;

    // v2：iPad 前端（只读 HTTP 服务）与托盘常驻
    std::unique_ptr<WebApi> webApi;
    std::unique_ptr<WebServer> webServer;
    QSystemTrayIcon *tray = nullptr;
    QString webUrl;          ///< iPad 访问地址（托盘菜单与状态栏显示）
    bool quitting = false;   ///< 托盘菜单点了「退出程序」：关窗要真的退出
    bool trayHintShown = false;

    // v2：启动后的第一次自动动作（全量扫描 / 会话检查）是否已触发过
    bool startupActionDone = false;

    /// 最近一次南大同步的模式：决定学习通跟随跑 full 还是 incremental 记录
    SyncManager::Mode lastSyncMode = SyncManager::Mode::Incremental;
};

// ---------------------------------------------------------------------------
// 构造
// ---------------------------------------------------------------------------

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent), d(std::make_unique<Impl>())
{
    setWindowTitle(QStringLiteral("智汇南雍 作业采集与管理"));
    // 两栏布局下 1080x720 就能放下全部内容（过大的默认窗口会在下方留出空白）
    resize(1080, 720);

    d->repository = std::make_unique<SqliteRepository>();
    d->bridge = std::make_unique<WebEnginePageBridge>();
    d->adapter = std::make_unique<NjuLmsAdapter>(d->bridge.get());
    d->sync = std::make_unique<SyncManager>(d->repository.get(), d->adapter.get());
    d->scheduler = std::make_unique<Scheduler>(d->repository.get(), d->sync.get());
    d->auth = std::make_unique<AuthManager>(d->adapter.get());

    buildUi();
    wireSignals();

    // ---- v2：恢复用户偏好 ----
    // buildUi() 只是把界面状态回读出来（此时信号还没接，不会触发动作），
    // 这里再把偏好同步到运行时：
    //   * 快速增量开关 → SyncManager::Config；
    //   * 定时同步周期 → Scheduler；
    //   * 若上次是「启用定时同步」，直接启动定时器（相当于替用户点一次开关）。
    {
        SyncManager::Config config = d->sync->config();
        config.useTodoFastPath = d->quickIncremental->isChecked();
        // 作业说明补全（默认开）：只有需要时才会多发请求，见 SyncManager::Config
        config.enrichDescriptions =
            makeAppSettings().value(QString::fromLatin1(kEnrichDescriptionsKey), true).toBool();
        d->sync->setConfig(config);

        d->scheduler->setIntervalMinutes(configuredIntervalMinutes());
        if (makeAppSettings().value(QString::fromLatin1(kScheduleEnabledKey), false).toBool()) {
            d->scheduleEnabled->setChecked(true);   // 触发 toggled → 启动定时器并写日志
        }
    }

    // ---- 打开数据库 ----
    QString error;
    const QString dbPath = SqliteRepository::defaultDatabasePath();
    if (!d->repository->open(dbPath, &error)) {
        QMessageBox::critical(this, QStringLiteral("数据库错误"),
                              QStringLiteral("无法打开数据库：\n%1\n\n路径：%2")
                                  .arg(error, dbPath));
    } else {
        qCInfo(lcApp) << "database ready";
    }

    // ---- 浏览器 Profile ----
    if (!d->bridge->initialize(QString(), &error)) {
        appendLog(QStringLiteral("浏览器 Profile 初始化失败：%1").arg(error), true);
    } else if (qEnvironmentVariableIsSet("NJULMS_DISABLE_AUTO_OPEN")) {
        // 测试环境：跳过启动自动打开，保持离线与确定性
        appendLog(QStringLiteral("已跳过启动自动打开站点（NJULMS_DISABLE_AUTO_OPEN）"));
    } else {
        // v2：启动即自动打开智汇南雍。
        //
        // 为什么必须自动打开：所有取数都发生在「已登录页面上下文」里
        // （页桥在 page 未就绪时会直接以 "page is not ready" 失败），
        // 因此页面必须被加载。会话 Cookie 是持久化的，加载成功即代表免密可用，
        // 用户不需要再点一次「打开智汇南雍并登录」。
        appendLog(QStringLiteral("正在自动打开智汇南雍…"));
        d->bridge->loadHome();
    }

    // ---- 学习通（超星）：适配器 + 同步运行器（与主页面共享持久化 Profile）----
    setupChaoxing();

    // ---- 启动状态检查：识别上次未完成的任务与是否需要补一次同步 ----
    QString startupSummary;
    const bool needsCatchUp = d->scheduler->checkStartupState(&startupSummary);
    if (!startupSummary.isEmpty()) {
        appendLog(QStringLiteral("启动检查：%1").arg(startupSummary));
    }
    if (needsCatchUp) {
        statusBar()->showMessage(QStringLiteral("建议执行一次同步以更新数据"), 15000);
    }

    loadPersistedData();
    updateActionStates();

    // ---- v2：iPad 前端（只读）与托盘常驻 ----
    // 顺序：先起服务再建托盘。两者失败都只记录，不影响采集功能。
    startWebServer();
    buildTray();
}

MainWindow::~MainWindow() = default;

// ---------------------------------------------------------------------------
// iPad 前端（只读 HTTP 服务）
// ---------------------------------------------------------------------------

/**
 * 启动只读前端。
 *
 * 关键点：
 *   * 只监听 127.0.0.1，外部入口交给 Tailscale（`tailscale serve` 提供 HTTPS）；
 *   * 所有数据都通过 WebApi 投递到主线程再取，与同步任务天然串行，
 *     不会出现「同步写库时前端读库」；
 *   * 端口占用等失败只写日志，不影响采集。
 */
void MainWindow::startWebServer()
{
    int port = makeAppSettings().value(QString::fromLatin1(kWebPortKey), kDefaultWebPort).toInt();
    const QString envPort = qEnvironmentVariable("NJULMS_WEB_PORT");
    if (!envPort.isEmpty()) {
        bool ok = false;
        const int parsed = envPort.toInt(&ok);
        if (ok) {
            port = parsed;
        }
    }
    if (port <= 0 || port > 65535) {
        appendLog(QStringLiteral("iPad 前端已关闭（设置 web/port=%1；0 表示关闭）。").arg(port));
        return;
    }

    WebSources sources;
    sources.status = [this]() { return buildStatusJson(); };
    sources.courses = [this]() { return buildCoursesJson(); };
    sources.assignments = [this](const QString &courseId) {
        return buildAssignmentsJson(courseId);
    };
    sources.assignmentsByScope = [this](const QString &scope) {
        return buildAssignmentsByScope(scope);
    };
    sources.logTail = [](int lines) { return readLogTail(lines); };
    sources.patchSettings = [this](const QJsonObject &patch) { return applyWebSettings(patch); };
    sources.startSync = [this](const QString &mode) { return startWebSync(mode); };
    sources.pauseSync = [this]() { return pauseWebSync(); };
    sources.resumeSync = [this]() { return resumeWebSync(); };
    sources.cancelSync = [this]() { return cancelWebSync(); };
    sources.purgeData = [this](const QString &confirm) { return purgeWebData(confirm); };

    d->webApi = std::make_unique<WebApi>(sources);
    d->webServer = std::make_unique<WebServer>(d->webApi.get());
    d->webServer->setAuthToken(webAccessToken());
    d->webUrl = detectPublicUrl(port);

    QString error;
    if (!d->webServer->start(static_cast<quint16>(port), &error)) {
        appendLog(QStringLiteral("iPad 前端启动失败：%1").arg(error), true);
        d->webServer.reset();
        d->webApi.reset();
        d->webUrl.clear();
        return;
    }

    appendLog(QStringLiteral("iPad 前端已启动：%1（只监听 127.0.0.1:%2；"
                             "写操作需要访问令牌，托盘菜单可复制）")
                  .arg(d->webUrl)
                  .arg(port));
    statusBar()->showMessage(QStringLiteral("iPad 前端：%1").arg(d->webUrl), 20000);
}

QJsonObject MainWindow::buildStatusJson() const
{
    QJsonObject root;

    QJsonObject app;
    app.insert(QStringLiteral("name"), QCoreApplication::applicationName());
    app.insert(QStringLiteral("version"), QCoreApplication::applicationVersion());
    app.insert(QStringLiteral("port"), d->webServer != nullptr ? d->webServer->port() : 0);
    app.insert(QStringLiteral("publicUrl"), d->webUrl);
    root.insert(QStringLiteral("app"), app);

    // 会话状态：直接复用界面上的展示文案与状态属性，
    // 这样前端不会出现「和 PC 上显示的不一样」的第二份真相。
    QJsonObject auth;
    auth.insert(QStringLiteral("state"),
                QString::fromLatin1(authStateKey(d->auth->state())));
    auth.insert(QStringLiteral("display"), d->authStatus->text());
    auth.insert(QStringLiteral("tone"), d->authStatus->property("state").toString());
    auth.insert(QStringLiteral("courseCount"), d->auth->lastKnownCourseCount());
    root.insert(QStringLiteral("auth"), auth);

    QJsonObject sync;
    sync.insert(QStringLiteral("running"), d->sync->isRunning());
    sync.insert(QStringLiteral("paused"), d->sync->isPaused());
    // 学习通阶段的运行状态单独给出：iPad 上"正在采集学习通"也要看得到
    sync.insert(QStringLiteral("chaoxingRunning"), chaoxingBusy());
    sync.insert(QStringLiteral("percent"), d->progress->value());
    sync.insert(QStringLiteral("currentIndex"), d->sync->currentCourseIndex());
    sync.insert(QStringLiteral("total"), d->sync->totalCourses());
    sync.insert(QStringLiteral("currentCourse"), d->sync->currentCourseName());
    sync.insert(QStringLiteral("statusMessage"), d->progressLabel->text());
    root.insert(QStringLiteral("sync"), sync);

    QJsonObject schedule;
    schedule.insert(QStringLiteral("enabled"), d->scheduler->isEnabled());
    schedule.insert(QStringLiteral("intervalMinutes"), d->scheduler->intervalMinutes());
    const QDateTime next = d->scheduler->nextRunAt();
    schedule.insert(QStringLiteral("nextRunAt"),
                    next.isValid() ? timeutil::toBeijingDisplay(next) : QString());
    root.insert(QStringLiteral("schedule"), schedule);

    QJsonObject settings;
    settings.insert(QStringLiteral("scheduleEnabled"), d->scheduleEnabled->isChecked());
    settings.insert(QStringLiteral("scheduleIntervalMinutes"), d->scheduleMinutes->value());
    settings.insert(QStringLiteral("quickIncremental"), d->quickIncremental->isChecked());
    settings.insert(QStringLiteral("startupFullScan"), d->startupFullScan->isChecked());
    settings.insert(QStringLiteral("enrichDescriptions"),
                    d->sync->config().enrichDescriptions);
    root.insert(QStringLiteral("settings"), settings);

    // 前端的写操作能不能做：完全由桌面端此刻的状态决定（同一份真相）
    const bool running = d->sync->isRunning() || chaoxingBusy();
    const bool paused = d->sync->isPaused();
    QJsonObject capabilities;
    capabilities.insert(QStringLiteral("canStartSync"), !running);
    capabilities.insert(QStringLiteral("canPause"), running && !paused);
    capabilities.insert(QStringLiteral("canResume"), running && paused);
    capabilities.insert(QStringLiteral("canCancel"), running);
    capabilities.insert(QStringLiteral("canChangeSchedule"), !running);
    capabilities.insert(QStringLiteral("canPurge"), !running);
    capabilities.insert(QStringLiteral("scheduleIntervalMin"), Scheduler::kMinIntervalMinutes);
    capabilities.insert(QStringLiteral("scheduleIntervalMax"), Scheduler::kMaxIntervalMinutes);
    capabilities.insert(QStringLiteral("blockedReason"),
                        running ? QStringLiteral("正在同步，设置改不了；可以暂停或取消")
                                : QString());
    root.insert(QStringLiteral("capabilities"), capabilities);

    const QList<CrawlRun> runs = d->repository->listRecentRuns(1);
    if (runs.isEmpty()) {
        root.insert(QStringLiteral("lastRun"), QJsonValue(QJsonValue::Null));
    } else {
        const CrawlRun &run = runs.first();
        QJsonObject last;
        last.insert(QStringLiteral("runId"), static_cast<double>(run.runId));
        last.insert(QStringLiteral("status"), QString::fromLatin1(CrawlRun::statusKey(run.status)));
        last.insert(QStringLiteral("statusDisplay"), runStatusDisplay(run.status));
        last.insert(QStringLiteral("startedAt"), timeutil::toBeijingDisplay(run.startedAt));
        last.insert(QStringLiteral("finishedAt"),
                    run.finishedAt.has_value() ? timeutil::toBeijingDisplay(*run.finishedAt)
                                               : QString());
        last.insert(QStringLiteral("message"), run.errorMessage);
        root.insert(QStringLiteral("lastRun"), last);
    }

    return root;
}

QJsonArray MainWindow::buildCoursesJson() const
{
    QJsonArray rows;
    const QList<Course> courses = d->repository->listCourses();
    for (const Course &course : courses) {
        QJsonObject row;
        row.insert(QStringLiteral("id"), course.courseId);
        row.insert(QStringLiteral("name"), course.courseName);
        row.insert(QStringLiteral("code"), course.courseCode);
        row.insert(QStringLiteral("instructorsText"),
                   course.instructors.join(QStringLiteral("、")));
        row.insert(QStringLiteral("lastSeenAt"),
                   course.lastSeenAt.isValid() ? timeutil::toBeijingDisplay(course.lastSeenAt)
                                               : QString());
        row.insert(QStringLiteral("assignmentCount"),
                   d->repository->countAssignments(course.courseId));
        rows.append(row);
    }
    return rows;
}

QJsonArray MainWindow::buildAssignmentsJson(const QString &courseId) const
{
    QJsonArray rows;
    if (courseId.isEmpty()) {
        return rows;
    }

    Course course;
    course.courseId = courseId;
    for (const Course &candidate : d->repository->listCourses()) {
        if (candidate.courseId == courseId) {
            course = candidate;
            break;
        }
    }

    const QList<Assignment> assignments = d->repository->listAssignments(courseId);
    for (const Assignment &item : assignments) {
        rows.append(assignmentRow(course, item, /*withCourseName=*/false,
                                  /*withDescription=*/true));
    }
    return rows;
}

/**
 * 跨课程的作业集合：iPad 上两个 tab 的数据源。
 *
 * 排序口径（在界面上会写明，避免误导）：
 *   * pending   = 未提交 + 已逾期，按**截止时间升序**（最急的在前）；
 *   * submitted = 已提交，按**发布时间降序**（用户指定）；发布时间缺失的那几条
 *     退化为按截止时间降序排在其后，不能让"没有发布时间的记录"乱插在中间。
 * 说明文本只在 pending 里带：已提交那一栏只列标题与时间，不带正文。
 */
QJsonArray MainWindow::buildAssignmentsByScope(const QString &scope) const
{
    const bool wantPending = scope == QLatin1String("pending");
    const QList<Course> courses = d->repository->listCourses();

    QVector<QPair<QDateTime, QJsonObject>> sorted;
    for (const Course &course : courses) {
        const QList<Assignment> assignments = d->repository->listAssignments(course.courseId);
        for (const Assignment &item : assignments) {
            if (!item.status.hasValue()) {
                continue;
            }
            const AssignmentStatus status = item.status.require();
            const bool isPending = status == AssignmentStatus::Pending
                                   || status == AssignmentStatus::Overdue;
            const bool isSubmitted = status == AssignmentStatus::Submitted;
            if (wantPending) {
                if (!isPending) {
                    continue;
                }
            } else if (!isSubmitted) {
                continue;
            }

            QDateTime key;
            if (wantPending) {
                key = item.dueAt.hasValue() ? item.dueAt.require() : QDateTime();
            } else if (item.publishedAt.hasValue()) {
                key = item.publishedAt.require();
            } else if (item.dueAt.hasValue()) {
                key = item.dueAt.require();
            }
            sorted.append(QPair<QDateTime, QJsonObject>(
                key, assignmentRow(course, item, /*withCourseName=*/true,
                                   /*withDescription=*/wantPending)));
        }
    }

    std::stable_sort(sorted.begin(), sorted.end(),
                     [wantPending](const QPair<QDateTime, QJsonObject> &lhs,
                                   const QPair<QDateTime, QJsonObject> &rhs) {
                         const bool lhsValid = lhs.first.isValid();
                         const bool rhsValid = rhs.first.isValid();
                         if (lhsValid != rhsValid) {
                             return lhsValid;
                         }
                         if (!lhsValid) {
                             return false;
                         }
                         return wantPending ? lhs.first < rhs.first : lhs.first > rhs.first;
                     });

    QJsonArray rows;
    for (const auto &entry : sorted) {
        rows.append(entry.second);
    }
    return rows;
}

QString MainWindow::webAccessToken()
{
    QSettings settings = makeAppSettings();
    QString token = settings.value(QString::fromLatin1(kWebTokenKey)).toString().trimmed();
    if (token.isEmpty()) {
        // 32 位十六进制随机串：够用且方便手抄/粘贴
        token = QString::number(QRandomGenerator::global()->generate64(), 16).rightJustified(16, QLatin1Char('0'))
                + QString::number(QRandomGenerator::global()->generate64(), 16).rightJustified(16, QLatin1Char('0'));
        settings.setValue(QString::fromLatin1(kWebTokenKey), token);
    }
    return token;
}

/**
 * 前端的写入口：设置。
 *
 * 实现方式是**驱动桌面端同一批控件**（QSpinBox / QCheckBox），它们的信号槽里
 * 已经有"写 QSettings + 应用到 Scheduler/SyncManager + 记日志"的完整逻辑，
 * 所以不存在第二套行为，也不会出现前端改了设置、后端还在按旧设置跑。
 *
 * 与桌面端一致的护栏：同步进行中不许改周期与定时开关（桌面端此时也是灰的），
 * 返回 409 让前端把按钮置灰并说明原因。
 */
WebActionResult MainWindow::applyWebSettings(const QJsonObject &patch)
{
    const bool running = d->sync->isRunning() || chaoxingBusy();
    const bool touchesSchedule = patch.contains(QStringLiteral("scheduleEnabled"))
                                 || patch.contains(QStringLiteral("scheduleIntervalMinutes"));
    if (running && touchesSchedule) {
        return WebActionResult{false, 409,
                               QStringLiteral("正在同步，定时周期与开关请在同步结束后再修改"),
                               {}};
    }

    if (patch.contains(QStringLiteral("scheduleIntervalMinutes"))) {
        const QJsonValue value = patch.value(QStringLiteral("scheduleIntervalMinutes"));
        if (!value.isDouble()) {
            return WebActionResult{false, 400, QStringLiteral("scheduleIntervalMinutes 必须是数字"), {}};
        }
        const int requested = static_cast<int>(value.toDouble());
        const int clamped = qBound(Scheduler::kMinIntervalMinutes, requested,
                                   Scheduler::kMaxIntervalMinutes);
        if (clamped != requested) {
            return WebActionResult{false, 400,
                                   QStringLiteral("周期必须在 %1 ~ %2 分钟之间")
                                       .arg(Scheduler::kMinIntervalMinutes)
                                       .arg(Scheduler::kMaxIntervalMinutes),
                                   {}};
        }
        // 走控件 → 触发 valueChanged → 持久化 + 应用到 Scheduler（若已启用）
        d->scheduleMinutes->setValue(clamped);
    }

    if (patch.contains(QStringLiteral("scheduleEnabled"))) {
        d->scheduleEnabled->setChecked(patch.value(QStringLiteral("scheduleEnabled")).toBool());
    }
    if (patch.contains(QStringLiteral("quickIncremental"))) {
        d->quickIncremental->setChecked(patch.value(QStringLiteral("quickIncremental")).toBool());
    }
    if (patch.contains(QStringLiteral("startupFullScan"))) {
        d->startupFullScan->setChecked(patch.value(QStringLiteral("startupFullScan")).toBool());
    }
    if (patch.contains(QStringLiteral("enrichDescriptions"))) {
        const bool on = patch.value(QStringLiteral("enrichDescriptions")).toBool();
        makeAppSettings().setValue(QString::fromLatin1(kEnrichDescriptionsKey), on);
        SyncManager::Config config = d->sync->config();
        config.enrichDescriptions = on;
        d->sync->setConfig(config);
        appendLog(on ? QStringLiteral("已开启「用活动接口补全作业说明」。")
                     : QStringLiteral("已关闭「用活动接口补全作业说明」。"));
    }

    return WebActionResult{true, 200, {}, buildStatusJson()};
}

/**
 * 前端触发同步。
 *
 * 刻意**不走** beginSync()：那个入口在失败时会弹桌面端对话框，而"从 iPad 点一下"
 * 不该在 PC 上蹦出一个要人点的窗口。这里做同样的前置判断，把原因用 409 返回给前端。
 */
WebActionResult MainWindow::startWebSync(const QString &mode)
{
    if (d->sync->isRunning() || chaoxingBusy()) {
        return WebActionResult{false, 409, QStringLiteral("已有同步任务在运行"), {}};
    }

    SyncManager::Mode syncMode = SyncManager::Mode::Incremental;
    if (mode == QLatin1String("full")) {
        syncMode = SyncManager::Mode::Full;
    } else if (mode != QLatin1String("incremental")) {
        return WebActionResult{false, 400, QStringLiteral("mode 只能是 full 或 incremental"), {}};
    }

    if (!d->bridge->isReady()) {
        appendLog(QStringLiteral("iPad 端触发同步：页面尚未就绪，正在打开站点…"));
        d->bridge->loadHome();
        return WebActionResult{false, 409,
                               QStringLiteral("页面尚未就绪，正在打开站点，请几秒后重试"), {}};
    }

    QString error;
    if (!d->sync->start(syncMode, &error)) {
        return WebActionResult{false, 409, QStringLiteral("无法开始同步：%1").arg(error), {}};
    }

    appendLog(QStringLiteral("iPad 端触发了 %1。").arg(syncModeDisplay(syncMode)));
    d->tabs->setCurrentIndex(0);   // 与桌面端一致：切到采集页看进度
    updateActionStates();
    return WebActionResult{true, 200, {}, buildStatusJson()};
}

WebActionResult MainWindow::pauseWebSync()
{
    if (!d->sync->isRunning() || d->sync->isPaused()) {
        return WebActionResult{false, 409, QStringLiteral("当前没有正在进行的同步"), {}};
    }
    d->sync->pause();
    appendLog(QStringLiteral("iPad 端暂停了同步。"));
    updateActionStates();
    return WebActionResult{true, 200, {}, buildStatusJson()};
}

WebActionResult MainWindow::resumeWebSync()
{
    if (!d->sync->isRunning() || !d->sync->isPaused()) {
        return WebActionResult{false, 409, QStringLiteral("当前没有已暂停的同步"), {}};
    }
    d->sync->resume();
    appendLog(QStringLiteral("iPad 端继续了同步。"));
    updateActionStates();
    return WebActionResult{true, 200, {}, buildStatusJson()};
}

WebActionResult MainWindow::cancelWebSync()
{
    if (!d->sync->isRunning() && !chaoxingBusy()) {
        return WebActionResult{false, 409, QStringLiteral("当前没有正在进行的同步"), {}};
    }
    if (d->sync->isRunning()) {
        d->sync->cancel();
    }
    if (chaoxingBusy()) {
        d->chaoxingSync->cancel();
    }
    appendLog(QStringLiteral("iPad 端取消了同步。"));
    updateActionStates();
    return WebActionResult{true, 200, {}, buildStatusJson()};
}

/**
 * 清空采集数据（保留登录状态）。
 *
 * 与 PC 上的「删除全部数据」是同一件事、同一个函数（`clearAllData`），
 * 只是入口从按钮换成了 HTTP。两道确认：前端弹警告窗并勾选，
 * 接口这里再要求 `confirm == "DELETE"`。
 */
WebActionResult MainWindow::purgeWebData(const QString &confirm)
{
    if (d->sync->isRunning()) {
        return WebActionResult{false, 409,
                               QStringLiteral("正在同步，请等它结束或先点「取消」，再删除数据"),
                               {}};
    }
    if (confirm != QLatin1String("DELETE")) {
        return WebActionResult{false, 400,
                               QStringLiteral("缺少确认标记（confirm 必须为 DELETE）"), {}};
    }

    QString error;
    const int removed = d->repository->clearAllData(&error);
    if (removed < 0) {
        appendLog(QStringLiteral("iPad 端删除全部数据失败：%1").arg(error), true);
        return WebActionResult{false, 500, QStringLiteral("删除失败：%1").arg(error), {}};
    }

    appendLog(QStringLiteral("iPad 端删除了全部采集数据（共 %1 行），登录状态保留。").arg(removed));
    refreshCourseList();
    loadPersistedData();
    refreshLastRun();
    updateActionStates();

    // 注意变量名：叫 data 会遮蔽 QWidget 基类的同名成员（MSVC C4458）
    QJsonObject payload;
    payload.insert(QStringLiteral("removed"), removed);
    return WebActionResult{true, 200, {}, payload};
}

QString MainWindow::readLogTail(int lines)
{
    const QString path =
        Logger::instance() != nullptr
            ? Logger::instance()->logFilePath()
            : QDir(Logger::defaultLogDir()).filePath(QStringLiteral("njulms.log"));

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QString();
    }

    // 只读尾部：日志可能长到几 MB，没必要整份读进来
    constexpr qint64 kMaxBytes = 256 * 1024;
    const qint64 offset = qMax<qint64>(0, file.size() - kMaxBytes);
    if (!file.seek(offset)) {
        return QString();
    }

    QStringList list = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    if (list.size() > lines) {
        list = list.mid(list.size() - lines);
    }
    return list.join(QLatin1Char('\n'));
}

// ---------------------------------------------------------------------------
// 托盘常驻
// ---------------------------------------------------------------------------

/**
 * 托盘图标：关窗后程序继续运行。
 *
 * 为什么需要：iPad 前端与定时同步都依赖这个进程活着；如果关窗即退出，
 * 手机/平板上就再也连不上（而且用户关窗的意图通常只是"把界面收起来"）。
 * 托盘不可用（某些精简系统）时退回旧行为：关窗即退出。
 */
void MainWindow::buildTray()
{
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        appendLog(QStringLiteral("系统托盘不可用：关闭窗口将直接退出程序。"));
        return;
    }

    QApplication::setQuitOnLastWindowClosed(false);

    d->tray = new QSystemTrayIcon(makeAppIcon(), this);
    d->tray->setToolTip(QStringLiteral("智汇南雍 作业采集与管理"));

    auto *menu = new QMenu(this);
    menu->addAction(QStringLiteral("显示主窗口"), this, [this]() {
        show();
        setWindowState((windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
        raise();
        activateWindow();
    });
    menu->addAction(QStringLiteral("立即增量同步"), this,
                    [this]() { beginSync(SyncManager::Mode::Incremental); });

    menu->addSeparator();
    if (!d->webUrl.isEmpty()) {
        menu->addAction(QStringLiteral("复制 iPad 访问地址"), this, [this]() {
            QGuiApplication::clipboard()->setText(d->webUrl);
            statusBar()->showMessage(QStringLiteral("已复制：%1").arg(d->webUrl), 8000);
        });
        menu->addAction(QStringLiteral("复制访问令牌"), this, [this]() {
            QGuiApplication::clipboard()->setText(webAccessToken());
            statusBar()->showMessage(
                QStringLiteral("访问令牌已复制：在 iPad 的「设置 → 访问令牌」里粘贴一次"),
                15000);
        });
        if (d->webServer != nullptr && d->webServer->isRunning()) {
            const int port = d->webServer->port();
            menu->addAction(QStringLiteral("在 PC 浏览器里预览前端"), this, [port]() {
                QDesktopServices::openUrl(QUrl(QStringLiteral("http://127.0.0.1:%1/").arg(port)));
            });
        }
    }

    menu->addSeparator();
    menu->addAction(QStringLiteral("退出程序"), this, [this]() {
        d->quitting = true;
        close();
        // 关窗默认只是隐藏（quitOnLastWindowClosed 已关掉），托盘退出要显式收尾
        QCoreApplication::quit();
    });
    d->tray->setContextMenu(menu);

    connect(d->tray, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason != QSystemTrayIcon::Trigger
                    && reason != QSystemTrayIcon::DoubleClick) {
                    return;
                }
                show();
                setWindowState((windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
                raise();
                activateWindow();
            });

    d->tray->show();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (d->tray != nullptr && !d->quitting) {
        event->ignore();
        hide();
        if (!d->trayHintShown) {
            d->trayHintShown = true;
            d->tray->showMessage(
                QStringLiteral("仍在后台运行"),
                QStringLiteral("定时同步与 iPad 前端继续工作。右键托盘图标可以退出程序。"),
                QSystemTrayIcon::Information, 6000);
        }
        return;
    }
    QMainWindow::closeEvent(event);
}

// ---------------------------------------------------------------------------
// 界面搭建
// ---------------------------------------------------------------------------

void MainWindow::buildUi()
{
    d->tabs = new QTabWidget(this);
    setCentralWidget(d->tabs);

    // ======================= 采集 =======================
    auto *collectPage = new QWidget();
    // v2：两栏布局（左：登录 / 操作 / 进度；右：定时同步 + 说明）。
    // 单列堆叠在宽窗口里会在下方留下大片空白。
    auto *collectLayout = new QHBoxLayout(collectPage);
    auto *leftColumn = new QVBoxLayout();
    auto *rightColumn = new QVBoxLayout();
    collectLayout->setSpacing(16);
    collectLayout->addLayout(leftColumn, 3);
    collectLayout->addLayout(rightColumn, 4);

    // --- 登录区 ---
    auto *authGroup = new QGroupBox(QStringLiteral("登录状态"), collectPage);
    auto *authLayout = new QVBoxLayout(authGroup);

    // 第一行：智汇南雍
    auto *njuAuthRow = new QHBoxLayout();
    d->authStatus = new QLabel(QStringLiteral("未知"), authGroup);
    setLabelState(d->authStatus, "unknown");
    njuAuthRow->addWidget(new QLabel(QStringLiteral("智汇南雍："), authGroup));
    njuAuthRow->addWidget(d->authStatus);
    njuAuthRow->addStretch();
    d->openSiteButton = new QPushButton(QStringLiteral("打开智汇南雍并登录"), authGroup);
    d->checkSessionButton = new QPushButton(QStringLiteral("检查登录状态"), authGroup);
    njuAuthRow->addWidget(d->openSiteButton);
    njuAuthRow->addWidget(d->checkSessionButton);
    authLayout->addLayout(njuAuthRow);

    // 第二行：学习通（与上一行同样的格式：站点名 + 状态 + 两个按钮）
    auto *chaoxingAuthRow = new QHBoxLayout();
    d->chaoxingAuthStatus = new QLabel(QStringLiteral("未检查（同步时自动采集）"), authGroup);
    setLabelState(d->chaoxingAuthStatus, "unknown");
    chaoxingAuthRow->addWidget(new QLabel(QStringLiteral("学习通："), authGroup));
    chaoxingAuthRow->addWidget(d->chaoxingAuthStatus);
    chaoxingAuthRow->addStretch();
    d->openChaoxingButton = new QPushButton(QStringLiteral("打开学习通并登录"), authGroup);
    d->checkChaoxingButton = new QPushButton(QStringLiteral("检查学习通登录"), authGroup);
    chaoxingAuthRow->addWidget(d->openChaoxingButton);
    chaoxingAuthRow->addWidget(d->checkChaoxingButton);
    authLayout->addLayout(chaoxingAuthRow);

    leftColumn->addWidget(authGroup);

    // 需求场景 C：登录失效时必须明确提示，而不是静默失败
    auto *hint = new QLabel(
        QStringLiteral("提示：程序启动时会自动打开智汇南雍并检查登录状态，会话有效时无需任何操作。"
                       "需要（重新）登录时程序会弹出浏览器窗口，请在其中手动完成统一身份认证"
                       "（含滑块或扫码），登录成功后窗口会自动隐藏。"
                       "本程序不会、也无法绕过验证码，不会保存密码。"),
        collectPage);
    hint->setWordWrap(true);
    setLabelRole(hint, "muted");
    leftColumn->addWidget(hint);

    // --- 操作区 ---
    auto *actionGroup = new QGroupBox(QStringLiteral("采集操作"), collectPage);
    auto *actionLayout = new QHBoxLayout(actionGroup);

    d->fullScanButton = new QPushButton(QStringLiteral("开始全量采集"), actionGroup);
    d->incrementalButton = new QPushButton(QStringLiteral("立即增量同步"), actionGroup);
    d->pauseButton = new QPushButton(QStringLiteral("暂停"), actionGroup);
    d->resumeButton = new QPushButton(QStringLiteral("继续"), actionGroup);
    d->cancelButton = new QPushButton(QStringLiteral("取消"), actionGroup);

    // 操作层级：全量采集是主操作；暂停/继续是次要动作；取消带危险语义
    setButtonVariant(d->fullScanButton, "primary");
    setButtonVariant(d->pauseButton, "ghost");
    setButtonVariant(d->resumeButton, "ghost");
    setButtonVariant(d->cancelButton, "danger");

    actionLayout->addWidget(d->fullScanButton);
    actionLayout->addWidget(d->incrementalButton);
    actionLayout->addSpacing(20);
    actionLayout->addWidget(d->pauseButton);
    actionLayout->addWidget(d->resumeButton);
    actionLayout->addWidget(d->cancelButton);
    actionLayout->addStretch();
    leftColumn->addWidget(actionGroup);

    // --- 进度 ---
    auto *progressGroup = new QGroupBox(QStringLiteral("当前任务进度"), collectPage);
    auto *progressLayout = new QVBoxLayout(progressGroup);

    d->progress = new QProgressBar(progressGroup);
    d->progress->setRange(0, 100);
    d->progress->setValue(0);
    progressLayout->addWidget(d->progress);

    d->progressLabel = new QLabel(QStringLiteral("空闲"), progressGroup);
    d->progressLabel->setWordWrap(true);
    progressLayout->addWidget(d->progressLabel);

    d->lastRunLabel = new QLabel(QStringLiteral("最近一次同步：尚未执行"), progressGroup);
    d->lastRunLabel->setWordWrap(true);
    setLabelRole(d->lastRunLabel, "muted");
    progressLayout->addWidget(d->lastRunLabel);
    leftColumn->addWidget(progressGroup);

    // --- 定时同步 ---
    auto *scheduleGroup = new QGroupBox(QStringLiteral("定时同步"), collectPage);
    auto *scheduleLayout = new QVBoxLayout(scheduleGroup);

    auto *scheduleRow = new QHBoxLayout();
    d->scheduleEnabled = new QCheckBox(QStringLiteral("启用定时同步"), scheduleGroup);
    d->scheduleMinutes = new QSpinBox(scheduleGroup);
    d->scheduleMinutes->setRange(Scheduler::kMinIntervalMinutes, Scheduler::kMaxIntervalMinutes);
    // v2：周期从上次设置恢复（首次使用为默认值 60 分钟）
    d->scheduleMinutes->setValue(
        qBound(Scheduler::kMinIntervalMinutes,
               makeAppSettings()
                   .value(QString::fromLatin1(kScheduleIntervalKey),
                          Scheduler::kDefaultIntervalMinutes)
                   .toInt(),
               Scheduler::kMaxIntervalMinutes));
    d->scheduleMinutes->setSuffix(QStringLiteral(" 分钟"));

    scheduleRow->addWidget(d->scheduleEnabled);
    scheduleRow->addWidget(new QLabel(QStringLiteral("周期："), scheduleGroup));
    scheduleRow->addWidget(d->scheduleMinutes);
    scheduleRow->addStretch();
    scheduleLayout->addLayout(scheduleRow);

    d->nextRunLabel = new QLabel(QStringLiteral("下一次同步：未启用"), scheduleGroup);
    setLabelRole(d->nextRunLabel, "muted");
    scheduleLayout->addWidget(d->nextRunLabel);

    // v2：快速增量（默认开启）。用 /api/todos 当变更信号：
    // 常态每轮只发 2 个请求，只有"待办有变化"的课程才做完整扫描。
    d->quickIncremental = new QCheckBox(
        QStringLiteral("快速增量（只深扫有变化的课程，少发请求）"), scheduleGroup);
    // v2：从上次设置恢复（首次使用默认开启）
    d->quickIncremental->setChecked(
        makeAppSettings().value(QString::fromLatin1(kQuickIncrementalKey), true).toBool());
    scheduleLayout->addWidget(d->quickIncremental);

    // v2：启动时自动全量扫描（默认开启，偏好持久化）
    d->startupFullScan =
        new QCheckBox(QStringLiteral("启动时自动全量扫描（保证数据最新）"), scheduleGroup);
    d->startupFullScan->setChecked(
        makeAppSettings().value(QString::fromLatin1(kStartupFullScanKey), true).toBool());
    d->startupFullScan->setToolTip(
        QStringLiteral("每次启动后自动把全部课程完整扫描一遍（N+1 个请求，十几门课约 10 秒）。\n"
                       "性能保护：若距上次成功全量扫描不足 %1 分钟会自动跳过。")
            .arg(kStartupFullScanMinGapMinutes));
    scheduleLayout->addWidget(d->startupFullScan);

    // 需求第十条：访问频率未与站点确认前必须如实提示，而不是默默高频采集
    auto *freqWarning = new QLabel(
        QStringLiteral("默认周期为 %1 分钟。经查证，站点文档中仅确认了"
                       "「密码登录强制滑块验证」这一策略，未说明自动化访问的频率限制。"
                       "请自行确认该频率是否合适，必要时调大周期。")
            .arg(Scheduler::kDefaultIntervalMinutes),
        scheduleGroup);
    freqWarning->setWordWrap(true);
    setLabelRole(freqWarning, "warning");
    scheduleLayout->addWidget(freqWarning);

    auto *schedNote = new QLabel(
        QStringLiteral("说明：程序未运行时定时器不会触发，因此「每 N 分钟」仅在程序运行期间有效；"
                       "重新启动时会检查上次状态并提示是否补一次同步。"),
        scheduleGroup);
    schedNote->setWordWrap(true);
    setLabelRole(schedNote, "muted");
    scheduleLayout->addWidget(schedNote);

    rightColumn->addWidget(scheduleGroup);
    leftColumn->addStretch();
    rightColumn->addStretch();

    // --- v2：浏览器页面独立成窗口，默认隐藏 ---
    // 日常不需要盯着页面看；只有会话失效、需要人工登录时才弹出，
    // 登录成功后自动隐藏（见 showLoginWindow / hideLoginWindow）。
    d->loginWindow = new LoginWindow(this);
    {
        auto *loginLayout = new QVBoxLayout(d->loginWindow);
        loginLayout->setContentsMargins(0, 0, 0, 0);
        loginLayout->addWidget(d->bridge->view());
    }

    auto *collectScroll = new QWidget();
    auto *collectScrollLayout = new QVBoxLayout(collectScroll);
    collectScrollLayout->addWidget(collectPage);
    collectScrollLayout->addStretch();

    d->tabs->addTab(collectScroll, QStringLiteral("采集"));

    // ======================= 数据 =======================
    auto *dataPage = new QWidget();
    auto *dataLayout = new QVBoxLayout(dataPage);

    auto *courseGroup = new QGroupBox(QStringLiteral("课程列表"), dataPage);
    auto *courseLayout = new QVBoxLayout(courseGroup);
    d->courseTable = new QTableWidget(0, 5, courseGroup);
    d->courseTable->setHorizontalHeaderLabels(
        {QStringLiteral("课程"), QStringLiteral("来源"), QStringLiteral("课程代码"),
         QStringLiteral("教师"), QStringLiteral("最后观察")});
    d->courseTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    d->courseTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    d->courseTable->setAlternatingRowColors(true);
    d->courseTable->setShowGrid(false);
    d->courseTable->verticalHeader()->setVisible(false);
    d->courseTable->setMinimumHeight(160);
    d->courseTable->setMaximumHeight(260);   // 课程表只占上半部分，把空间留给作业表
    {
        QHeaderView *header = d->courseTable->horizontalHeader();
        header->setStretchLastSection(false);
        header->setSectionResizeMode(0, QHeaderView::Stretch);
        header->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        header->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        header->setSectionResizeMode(3, QHeaderView::Stretch);
        header->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    }
    d->courseEmptyHint = new QLabel(
        QStringLiteral("暂无课程数据。点「开始全量采集」后，这里会列出你有权限访问的课程。"),
        courseGroup);
    d->courseEmptyHint->setAlignment(Qt::AlignCenter);
    d->courseEmptyHint->setWordWrap(true);
    setLabelRole(d->courseEmptyHint, "empty");
    courseLayout->addWidget(d->courseEmptyHint);
    courseLayout->addWidget(d->courseTable);
    dataLayout->addWidget(courseGroup, 0);

    auto *assignmentGroup = new QGroupBox(QStringLiteral("作业列表"), dataPage);
    auto *assignmentLayout = new QVBoxLayout(assignmentGroup);

    auto *selectorRow = new QHBoxLayout();
    selectorRow->addWidget(new QLabel(QStringLiteral("选择课程："), assignmentGroup));
    d->courseSelector = new QComboBox(assignmentGroup);
    d->courseSelector->setMinimumWidth(320);
    selectorRow->addWidget(d->courseSelector);
    selectorRow->addStretch();
    d->assignmentSummary = new QLabel(assignmentGroup);
    selectorRow->addWidget(d->assignmentSummary);
    assignmentLayout->addLayout(selectorRow);

    d->assignmentTable = new QTableWidget(0, ColumnCount, assignmentGroup);
    d->assignmentTable->setHorizontalHeaderLabels(
        {QStringLiteral("状态"), QStringLiteral("类型"), QStringLiteral("标题"),
         QStringLiteral("截止时间（北京时间）"), QStringLiteral("剩余"), QStringLiteral("发布时间"),
         QStringLiteral("得分"), QStringLiteral("来源 ID"), QStringLiteral("来源")});
    d->assignmentTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    d->assignmentTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    d->assignmentTable->setAlternatingRowColors(true);
    d->assignmentTable->setShowGrid(false);
    d->assignmentTable->verticalHeader()->setVisible(false);
    {
        QHeaderView *header = d->assignmentTable->horizontalHeader();
        header->setStretchLastSection(false);
        header->setSectionResizeMode(ColStatus, QHeaderView::ResizeToContents);
        header->setSectionResizeMode(ColKind, QHeaderView::ResizeToContents);
        header->setSectionResizeMode(ColTitle, QHeaderView::Stretch);
        header->setSectionResizeMode(ColDue, QHeaderView::ResizeToContents);
        header->setSectionResizeMode(ColDueRelative, QHeaderView::ResizeToContents);
        header->setSectionResizeMode(ColPublished, QHeaderView::ResizeToContents);
        header->setSectionResizeMode(ColScore, QHeaderView::ResizeToContents);
        header->setSectionResizeMode(ColSourceId, QHeaderView::ResizeToContents);
    }
    d->assignmentEmptyHint = new QLabel(
        QStringLiteral("这门课程暂无可显示的作业记录。"), assignmentGroup);
    d->assignmentEmptyHint->setAlignment(Qt::AlignCenter);
    d->assignmentEmptyHint->setWordWrap(true);
    setLabelRole(d->assignmentEmptyHint, "empty");
    assignmentLayout->addWidget(d->assignmentEmptyHint);
    assignmentLayout->addWidget(d->assignmentTable);

    // 说明哪些字段是推导出来的，避免被误认为站点权威字段
    auto *derivedNote = new QLabel(
        QStringLiteral("说明：「状态」由接口的 submitted / is_closed 与截止时间推导，"
                       "「来源 ID」在作业列表接口未提供 unique_key 时按站点规则 "
                       "{type}-{id} 推导；（未提供）/（解析失败）表示该字段本次没有权威值，"
                       "数据库中保留旧值。"),
        assignmentGroup);
    derivedNote->setWordWrap(true);
    setLabelRole(derivedNote, "muted");
    assignmentLayout->addWidget(derivedNote);

    dataLayout->addWidget(assignmentGroup, 1);

    // ---- v2：危险操作（清空采集数据，保留登录状态）----
    auto *dangerGroup = new QGroupBox(QStringLiteral("危险操作"), dataPage);
    auto *dangerLayout = new QHBoxLayout(dangerGroup);
    d->clearDataButton =
        new QPushButton(QStringLiteral("删除全部数据（保留登录状态）"), dangerGroup);
    d->clearDataButton->setToolTip(
        QStringLiteral("清空本地数据库中的：课程、作业、同步断点、运行/批次记录、字段解析异常。\n"
                       "浏览器登录状态（Profile）与日志文件不受影响。"));
    setButtonVariant(d->clearDataButton, "danger");
    dangerLayout->addWidget(d->clearDataButton);
    auto *dangerNote = new QLabel(
        QStringLiteral("删除后下一次同步会重新做一次完整采集；登录状态保留，不需要重新扫码/过滑块。"),
        dangerGroup);
    dangerNote->setWordWrap(true);
    setLabelRole(dangerNote, "muted");
    dangerLayout->addWidget(dangerNote, 1);
    dataLayout->addWidget(dangerGroup);

    d->tabs->addTab(dataPage, QStringLiteral("数据"));

    // ======================= 日志 =======================
    auto *logPage = new QWidget();
    auto *logLayout = new QVBoxLayout(logPage);

    auto *logToolbar = new QHBoxLayout();
    d->autoScroll = new QCheckBox(QStringLiteral("自动滚动"), logPage);
    d->autoScroll->setChecked(true);
    auto *clearLog = new QPushButton(QStringLiteral("清空显示"), logPage);
    auto *openLogDir = new QPushButton(QStringLiteral("日志文件位置"), logPage);
    logToolbar->addWidget(d->autoScroll);
    logToolbar->addWidget(clearLog);
    logToolbar->addWidget(openLogDir);
    logToolbar->addStretch();
    logLayout->addLayout(logToolbar);

    d->logView = new QPlainTextEdit(logPage);
    d->logView->setReadOnly(true);
    d->logView->setMaximumBlockCount(5000);  // 限制行数，避免长时间运行后卡顿
    d->logView->setLineWrapMode(QPlainTextEdit::NoWrap);
    logLayout->addWidget(d->logView);

    auto *privacyNote = new QLabel(
        QStringLiteral("日志已统一脱敏：密码、Cookie、会话令牌、验证码与完整认证跳转 URL "
                       "都不会写入。"),
        logPage);
    setLabelRole(privacyNote, "muted");
    logLayout->addWidget(privacyNote);

    d->tabs->addTab(logPage, QStringLiteral("日志"));

    // 日志面板接线
    connect(clearLog, &QPushButton::clicked, d->logView, &QPlainTextEdit::clear);
    connect(openLogDir, &QPushButton::clicked, this, []() {
        // 只显示路径，不自动打开外部程序
        QMessageBox::information(nullptr, QStringLiteral("日志文件"),
                                 Logger::instance() != nullptr
                                     ? Logger::instance()->logFilePath()
                                     : Logger::defaultLogDir());
    });

    statusBar()->showMessage(QStringLiteral("就绪"));
}

// ---------------------------------------------------------------------------
// 信号接线
// ---------------------------------------------------------------------------

void MainWindow::wireSignals()
{
    // ---- 日志 ----
    if (Logger *logger = Logger::instance()) {
        connect(logger, &Logger::messageLogged, this,
                [this](Logger::Level level, const QString &line) {
                    appendLog(line, level == Logger::Level::Critical
                                        || level == Logger::Level::Warning);
                });
    }

    // ---- 浏览器 ----
    connect(d->openSiteButton, &QPushButton::clicked, this, [this]() {
        // v2：浏览器在独立窗口里，按钮把它叫出来；页面没就绪或不在本站时顺带打开首页
        showLoginWindow(QStringLiteral("已打开浏览器窗口；若显示课程页说明登录仍然有效。"));
        if (!d->bridge->isReady() || !d->bridge->looksAuthenticated()) {
            d->bridge->loadHome();
        }
    });

    // 页面被重定向到认证主机：这只是一个**提示**，不是授权结论。
    // 需求要求「不要仅凭首页加载成功就判断已登录」，对称地也不能仅凭跳转
    // 就断言失效 —— 权威判定必须来自接口响应（AuthManager::check）。
    // 因此这里不直接改状态，而是在采集进行中时暂停并提示，避免继续打无效请求。
    connect(d->bridge.get(), &WebEnginePageBridge::loginPageReached, this, [this]() {
        if (d->sync->isRunning()) {
            d->sync->pause();
            appendLog(QStringLiteral("检测到已跳转到认证页面，已暂停采集。"
                                     "请完成登录后点击「继续」。"),
                      true);
        }
        statusBar()->showMessage(
            QStringLiteral("当前停留在认证页面；完成登录后程序会自动继续（窗口自动隐藏）"), 20000);

        // v2：需要人工登录时才弹出浏览器窗口（登录成功后自动隐藏）
        showLoginWindow(QStringLiteral(
            "登录状态失效：已弹出浏览器窗口，请完成统一身份认证（成功后窗口会自动隐藏）"));
    });

    connect(d->bridge.get(), &WebEnginePageBridge::safeUrlChanged, this,
            [this](const QString &safeUrl) {
                statusBar()->showMessage(QStringLiteral("当前页面：%1").arg(safeUrl), 8000);
            });

    connect(d->bridge.get(), &WebEnginePageBridge::profileError, this,
            [this](const QString &reason) {
                appendLog(QStringLiteral("浏览器 Profile 错误：%1").arg(reason), true);
            });

    // v2：启动后第一次成功加载本站页面时执行「启动动作」：
    //   - 默认（设置开启）：做一次**完整全量扫描**（它自带会话检查，
    //     比"先检查登录再扫描"少一个请求）；
    //   - 设置关闭、或距上次成功全量扫描不足 kStartupFullScanMinGapMinutes 分钟时，
    //     退化为只检查一次会话状态。
    // 权威判定依旧来自接口响应（页面加载成功本身不算已登录）；
    // 停在统一身份认证页面时不触发，等登录完成、回跳到本站页面后再执行。
    connect(d->bridge.get(), &WebEnginePageBridge::loadFinished, this,
            [this](bool ok, const QString & /*url*/) {
                if (!ok || d->startupActionDone) {
                    return;
                }
                const QUrl current(d->bridge->currentUrl());
                if (current.host() != QLatin1String("lms.nju.edu.cn")) {
                    return;
                }
                d->startupActionDone = true;

                if (!d->startupFullScan->isChecked()) {
                    qCInfo(lcApp) << "startup: startup full scan disabled; checking session only";
                    d->auth->check(false);
                    return;
                }

                // 性能保护：距上次成功全量扫描太近就不重复扫
                const auto marker =
                    d->repository->findSyncState(SqliteRepository::fullReconcileTaskKey());
                const QDateTime lastFull = marker.has_value()
                                               ? marker->lastSuccessAt.value_or(QDateTime())
                                               : QDateTime();
                if (lastFull.isValid() &&
                    lastFull.secsTo(timeutil::nowUtc()) < kStartupFullScanMinGapMinutes * 60) {
                    appendLog(QStringLiteral(
                                  "启动检查：距上次全量扫描不足 %1 分钟，跳过启动全量，"
                                  "改为检查登录状态。")
                                  .arg(kStartupFullScanMinGapMinutes));
                    d->auth->check(false);
                    return;
                }

                qCInfo(lcApp) << "startup: running a full scan as configured";
                appendLog(QStringLiteral("启动检查：按设置执行一次完整全量扫描…"));
                beginSync(SyncManager::Mode::Full);
            });

    // ---- 会话检查 ----
    //
    // 状态由 AuthManager 统一维护，UI 只负责显示。
    // 这样「适配器在采集途中发现失效」与「用户手动检查发现失效」走同一条路径，
    // 不会出现两处各自维护一份状态、互相矛盾的情况。
    connect(d->checkSessionButton, &QPushButton::clicked, this, [this]() {
        d->auth->check(true);
    });

    connect(d->auth.get(), &AuthManager::stateChanged, this, [this](AuthState state) {
        switch (state) {
        case AuthState::Authenticated:
            d->authStatus->setText(
                QStringLiteral("已登录（可访问 %1 门课程）").arg(d->auth->lastKnownCourseCount()));
            setLabelState(d->authStatus, "good");
            hideLoginWindow();   // v2：登录成功 → 自动隐藏浏览器窗口
            break;
        case AuthState::NeedLogin:
            d->authStatus->setText(QStringLiteral("需要登录"));
            setLabelState(d->authStatus, "bad");
            // v2：需要人工登录 → 弹出浏览器窗口
            showLoginWindow(QStringLiteral("需要登录：请在浏览器窗口中完成统一身份认证"));
            break;
        case AuthState::Checking:
            d->authStatus->setText(QStringLiteral("检查中…"));
            setLabelState(d->authStatus, "busy");
            break;
        case AuthState::Unknown:
            d->authStatus->setText(QStringLiteral("未知"));
            setLabelState(d->authStatus, "unknown");
            break;
        }
        d->checkSessionButton->setEnabled(state != AuthState::Checking);
        updateActionStates();
    });

    connect(d->auth.get(), &AuthManager::checked, this,
            [this](bool authenticated, const QString &detail) {
                // 非登录类失败（网络/服务端）要如实说明，不要误导用户去重新登录
                if (!authenticated && d->auth->state() != AuthState::NeedLogin) {
                    d->authStatus->setText(QStringLiteral("检查失败：%1").arg(detail));
                    setLabelState(d->authStatus, "warn");
                }
            });

    connect(d->auth.get(), &AuthManager::loginRequired, this, [this](const QString &reason) {
        appendLog(QStringLiteral("会话需要人工登录：%1").arg(reason), true);
        // 需求场景 C：登录失效时暂停采集并提示重新登录，而不是继续跑
        if (d->sync->isRunning()) {
            d->sync->pause();
            appendLog(QStringLiteral("已暂停采集。请完成登录后点击「继续」。"), true);
        }
        QMessageBox::information(
            this, QStringLiteral("需要登录"),
            QStringLiteral("登录状态已失效或尚未登录。\n\n"
                           "请点击「打开智汇南雍并登录」，在浏览器窗口中手动完成"
                           "统一身份认证（可能需要滑块验证或扫码），"
                           "然后再次点击「检查登录状态」。"));
    });

    connect(d->auth.get(), &AuthManager::loginRestored, this, [this](int courseCount) {
        appendLog(QStringLiteral("登录已恢复，可访问 %1 门课程。").arg(courseCount));
        statusBar()->showMessage(QStringLiteral("登录成功，可以开始采集"), 15000);
    });

    // ---- 采集操作 ----
    connect(d->fullScanButton, &QPushButton::clicked, this,
            [this]() { beginSync(SyncManager::Mode::Full); });
    connect(d->incrementalButton, &QPushButton::clicked, this,
            [this]() { beginSync(SyncManager::Mode::Incremental); });

    // ---- v2：启动时自动全量扫描的开关（偏好持久化）----
    connect(d->startupFullScan, &QCheckBox::toggled, this, [this](bool on) {
        QSettings settings = makeAppSettings();
        settings.setValue(QString::fromLatin1(kStartupFullScanKey), on);
        appendLog(on
                      ? QStringLiteral("已启用「启动时自动全量扫描」。")
                      : QStringLiteral("已关闭「启动时自动全量扫描」，启动时只做一次登录检查。"));
    });

    // ---- v2：删除全部数据（保留登录状态）----
    connect(d->clearDataButton, &QPushButton::clicked, this, [this]() {
        if (d->sync->isRunning()) {
            QMessageBox::warning(this, QStringLiteral("正在同步"),
                                 QStringLiteral("当前有同步任务在运行。请先等它结束（或点「取消」），"
                                                "再删除数据。"));
            return;
        }

        const QMessageBox::StandardButton answer = QMessageBox::warning(
            this, QStringLiteral("确认删除全部数据"),
            QStringLiteral("将清空本地数据库中的：\n"
                           "  • 课程列表\n"
                           "  • 作业记录（含已软删除的历史记录）\n"
                           "  • 同步断点、运行记录与批次记录\n"
                           "  • 字段解析异常记录\n\n"
                           "不会删除：浏览器登录状态（不需要重新扫码/过滑块）、日志文件。\n\n"
                           "删除后，下一次同步会重新做一次完整采集。确定继续吗？"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) {
            appendLog(QStringLiteral("已取消「删除全部数据」。"));
            return;
        }

        QString error;
        const int removed = d->repository->clearAllData(&error);
        if (removed < 0) {
            appendLog(QStringLiteral("删除全部数据失败：%1").arg(error), true);
            QMessageBox::critical(this, QStringLiteral("删除失败"), error);
            return;
        }

        appendLog(QStringLiteral("已删除全部采集数据（共 %1 行），登录状态保留。").arg(removed));
        refreshCourseList();
        loadPersistedData();
        refreshLastRun();
        updateActionStates();
    });

    connect(d->pauseButton, &QPushButton::clicked, this, [this]() {
        d->sync->pause();
        updateActionStates();
    });
    connect(d->resumeButton, &QPushButton::clicked, this, [this]() {
        d->sync->resume();
        updateActionStates();
    });
    connect(d->cancelButton, &QPushButton::clicked, this, [this]() {
        // 南大与学习通是串行两段：取消时两段都取消（未开始的那段自然无操作）
        if (d->sync->isRunning()) {
            d->sync->cancel();
        }
        if (chaoxingBusy()) {
            d->chaoxingSync->cancel();
        }
        updateActionStates();
    });

    // ---- 同步进度与结果 ----
    connect(d->sync.get(), &SyncManager::runStarted, this,
            [this](SyncManager::Mode mode, qint64 runId, int totalCourses) {
                d->lastSyncMode = mode;
                d->progress->setValue(0);
                d->progressLabel->setText(QStringLiteral("%1 开始：共 %2 门课程（运行 #%3）")
                                              .arg(syncModeDisplay(mode))
                                              .arg(totalCourses)
                                              .arg(runId));
                updateActionStates();
            });

    connect(d->sync.get(), &SyncManager::progressChanged, this, [this](int percent) {
        d->progress->setValue(percent);
    });

    connect(d->sync.get(), &SyncManager::statusMessage, this, [this](const QString &message) {
        d->progressLabel->setText(message);
        statusBar()->showMessage(message, 10000);
    });

    connect(d->sync.get(), &SyncManager::courseStarted, this,
            [this](int index, int total, const QString &courseId, const QString &courseName) {
                Q_UNUSED(courseId)
                d->progressLabel->setText(QStringLiteral("正在采集 %1（%2/%3）…")
                                              .arg(courseName)
                                              .arg(index)
                                              .arg(total));
            });

    connect(d->sync.get(), &SyncManager::courseFinished, this,
            [this](const SyncManager::CourseOutcome &outcome) {
                if (!outcome.ok) {
                    appendLog(QStringLiteral("课程「%1」采集失败：%2")
                                  .arg(outcome.courseName, outcome.error),
                              true);
                }
            });

    connect(d->sync.get(), &SyncManager::loginRequired, this, [this](const QString &reason) {
        // 只记录与更新状态栏，**不弹对话框**。
        //
        // 同一次会话失效会同时被两条路径观察到：
        //   1. AuthManager 监听适配器的 sessionExpired；
        //   2. SyncManager 自己检查会话失败。
        // 若两边都弹窗，用户会连续看到 2–3 个「需要重新登录」（实测确认存在这个竞态）。
        // 因此弹窗统一由 AuthManager::loginRequired 负责（那是唯一的认证状态权威），
        // 这里只补充一条带上下文的日志。
        appendLog(QStringLiteral("登录状态失效，采集已暂停：%1").arg(reason), true);
        statusBar()->showMessage(QStringLiteral("登录状态失效，请重新登录后继续"), 30000);
        updateActionStates();
    });

    connect(d->sync.get(), &SyncManager::runFinished, this,
            [this](const SyncManager::RunSummary &summary) {
                d->progress->setValue(summary.outcome == SyncManager::Outcome::Success ? 100
                                                                                      : d->progress->value());
                // v2：只要有一门课程成功，就说明会话可用 —— 刷新登录状态，
                // 这也会触发「登录成功 → 自动隐藏浏览器窗口」（AuthState::Authenticated）。
                if (summary.coursesSucceeded > 0 &&
                    (summary.outcome == SyncManager::Outcome::Success ||
                     summary.outcome == SyncManager::Outcome::Partial)) {
                    d->auth->markAuthenticated(summary.coursesTotal);
                }
                // 一次性刷新：课程列表 + 最近一次结果 + 当前课程作业
                // （原来这里 refreshLastRun/refreshCourseList 会被执行两遍）
                loadPersistedData();
                updateActionStates();

                // WaitingForLogin 的提示由 AuthManager 负责（见上面的 loginRequired），
                // 这里不再重复弹窗，只在状态栏留一条简短结论。
                if (summary.outcome == SyncManager::Outcome::WaitingForLogin) {
                    statusBar()->showMessage(
                        QStringLiteral("同步已暂停：登录状态失效。已采集的数据不会丢失。"), 30000);
                }
            });

    // ---- 按钮可用性 ----
    connect(d->sync.get(), &SyncManager::runFinished, this, &MainWindow::updateActionStates);

    // ---- 定时同步 ----
    connect(d->scheduleEnabled, &QCheckBox::toggled, this, [this](bool on) {
        makeAppSettings().setValue(QString::fromLatin1(kScheduleEnabledKey), on);
        if (on) {
            d->scheduler->setIntervalMinutes(configuredIntervalMinutes());
            d->scheduler->start();
            appendLog(QStringLiteral("已启用定时同步，周期 %1 分钟。")
                          .arg(d->scheduler->intervalMinutes()));
        } else {
            d->scheduler->stop();
            appendLog(QStringLiteral("已停用定时同步。"));
        }
        updateActionStates();
    });

    // ---- v2：快速增量开关 ----
    connect(d->quickIncremental, &QCheckBox::toggled, this, [this](bool on) {
        makeAppSettings().setValue(QString::fromLatin1(kQuickIncrementalKey), on);
        SyncManager::Config config = d->sync->config();
        config.useTodoFastPath = on;
        d->sync->setConfig(config);
        appendLog(on ? QStringLiteral("已启用快速增量：先读待办判断变化，只深扫变化的课程。")
                     : QStringLiteral("已关闭快速增量：增量同步会完整扫描全部课程。"));
    });

    connect(d->scheduleMinutes, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int) {
        makeAppSettings().setValue(QString::fromLatin1(kScheduleIntervalKey),
                                   configuredIntervalMinutes());
        if (d->scheduler->isEnabled()) {
            d->scheduler->setIntervalMinutes(configuredIntervalMinutes());
        }
    });

    connect(d->scheduler.get(), &Scheduler::nextRunAtChanged, this,
            [this](const QDateTime &next) {
                d->nextRunLabel->setText(
                    next.isValid()
                        ? QStringLiteral("下一次同步：%1（北京时间）")
                              .arg(timeutil::toBeijingDisplay(next))
                        : QStringLiteral("下一次同步：未启用"));
            });

    connect(d->scheduler.get(), &Scheduler::skippedBecauseBusy, this, [this]() {
        appendLog(QStringLiteral("定时触发被跳过：已有采集任务在运行。"));
    });

    connect(d->scheduler.get(), &Scheduler::orphanedRunDetected, this, [this](int count) {
        appendLog(QStringLiteral("发现 %1 个上次未完成的采集任务，已标记为中断。"
                                 "可从最后一个已提交批次安全恢复。")
                      .arg(count),
                  true);
    });

    connect(d->scheduler.get(), &Scheduler::overdueSince, this,
            [this](const QDateTime &last, qint64 overdueMinutes) {
                appendLog(QStringLiteral("距上次成功同步已 %1 分钟（上次：%2）。程序未运行时"
                                         "定时器不会触发，建议立即同步一次。")
                              .arg(overdueMinutes)
                              .arg(timeutil::toBeijingDisplay(last)));
            });

    // 倒计时刷新
    auto *tick = new QTimer(this);
    connect(tick, &QTimer::timeout, this, [this]() {
        if (!d->scheduler->isEnabled()) {
            return;
        }
        const qint64 ms = d->scheduler->msUntilNextRun();
        if (ms >= 0) {
            const qint64 minutes = ms / 60000;
            d->nextRunLabel->setText(
                QStringLiteral("下一次同步：%1（约 %2 分钟后）")
                    .arg(timeutil::toBeijingDisplay(d->scheduler->nextRunAt()))
                    .arg(minutes));
        }
    });
    tick->start(30000);

    // ---- 数据页 ----
    connect(d->courseSelector, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int index) {
                if (index < 0) {
                    d->assignmentTable->setRowCount(0);
                    return;
                }
                const QString courseId = d->courseSelector->itemData(index).toString();
                refreshAssignmentTable(courseId);
            });

    connect(d->courseTable, &QTableWidget::itemSelectionChanged, this, [this]() {
        const int row = d->courseTable->currentRow();
        if (row < 0) {
            return;
        }
        const QString courseId =
            d->courseTable->item(row, 0)->data(Qt::UserRole).toString();
        const int comboIndex = d->courseSelector->findData(courseId);
        if (comboIndex >= 0) {
            d->courseSelector->setCurrentIndex(comboIndex);
        }
    });
}

// ---------------------------------------------------------------------------
// 数据展示
// ---------------------------------------------------------------------------

void MainWindow::loadPersistedData()
{
    refreshCourseList();
    refreshLastRun();
    // 当前选中课程的作业也刷新（v2：原来只在表格为空时刷新，导致同步后表格停留在旧数据）
    if (d->courseSelector->count() > 0) {
        refreshAssignmentTable(d->courseSelector->currentData().toString());
    }
}

void MainWindow::refreshCourseList()
{
    const QList<Course> courses = d->repository->listCourses();

    // 记录当前选择，刷新后尽量恢复
    const QString previous = d->courseSelector->currentData().toString();

    d->courseTable->setRowCount(courses.size());
    d->courseSelector->blockSignals(true);
    d->courseSelector->clear();

    for (int row = 0; row < courses.size(); ++row) {
        const Course &c = courses.at(row);

        auto *nameItem = new QTableWidgetItem(c.courseName);
        nameItem->setData(Qt::UserRole, c.courseId);
        d->courseTable->setItem(row, 0, nameItem);
        d->courseTable->setItem(row, 1, new QTableWidgetItem(sourceDisplay(c.source)));
        d->courseTable->setItem(row, 2, new QTableWidgetItem(c.courseCode));
        d->courseTable->setItem(row, 3,
                                new QTableWidgetItem(c.instructors.join(QStringLiteral("、"))));
        auto *seenItem = new QTableWidgetItem(c.lastSeenAt.isValid()
                                                  ? timeutil::toBeijingDisplay(c.lastSeenAt)
                                                  : QString());
        seenItem->setFont(monospaceFont());   // 时间列用等宽，便于纵向比对
        d->courseTable->setItem(row, 4, seenItem);

        const int count = d->repository->countAssignments(c.courseId);
        d->courseSelector->addItem(
            QStringLiteral("%1（%2 条作业）").arg(c.courseName).arg(count), c.courseId);
    }

    if (!previous.isEmpty()) {
        const int index = d->courseSelector->findData(previous);
        if (index >= 0) {
            d->courseSelector->setCurrentIndex(index);
        }
    }
    d->courseSelector->blockSignals(false);

    // 空状态：没有课程时显示提示，而不是一张空白表格
    const bool empty = courses.isEmpty();
    d->courseEmptyHint->setVisible(empty);
    d->courseTable->setVisible(!empty);
    statusBar()->showMessage(QStringLiteral("共 %1 门课程").arg(courses.size()), 5000);
}

void MainWindow::refreshAssignmentTable(const QString &courseId)
{
    const QList<Assignment> assignments = d->repository->listAssignments(courseId);

    // 作业本身不存来源（来源属于课程）：查一次课程拿来源标签
    QString courseSource;
    for (const Course &candidate : d->repository->listCourses()) {
        if (candidate.courseId == courseId) {
            courseSource = candidate.source;
            break;
        }
    }

    d->assignmentTable->setRowCount(assignments.size());
    int overdue = 0;
    int pending = 0;

    const QDateTime now = timeutil::nowUtc();

    for (int row = 0; row < assignments.size(); ++row) {
        const Assignment &a = assignments.at(row);

        // 状态
        QString statusText = QStringLiteral("未知");
        if (a.status.hasValue()) {
            statusText = assignmentStatusDisplay(a.status.require());
            if (a.status.require() == AssignmentStatus::Overdue) {
                ++overdue;
            } else if (a.status.require() == AssignmentStatus::Pending) {
                ++pending;
            }
        }
        d->assignmentTable->setItem(row, ColStatus, new QTableWidgetItem(statusText));

        // 类型（作业 / 考试）
        d->assignmentTable->setItem(row, ColKind,
                                    new QTableWidgetItem(assignmentKindDisplay(a.kind)));

        // 标题
        auto *titleItem = new QTableWidgetItem(
            a.title.hasValue() ? a.title.require() : QStringLiteral("（标题未获取）"));
        if (a.detailUrl.hasValue()) {
            titleItem->setToolTip(QStringLiteral("%1\n\n详情链接为按站点路由推导，未经实测验证。")
                                      .arg(a.detailUrl.require()));
        }
        d->assignmentTable->setItem(row, ColTitle, titleItem);

        // 截止时间（展示为北京时间；库中存 UTC）
        auto *dueItem = new QTableWidgetItem(formatTime(a.dueAt));
        dueItem->setFont(monospaceFont());
        d->assignmentTable->setItem(row, ColDue, dueItem);

        // 剩余时间
        QString relative;
        if (a.dueAt.hasValue()) {
            relative = timeutil::describeRelativeToNow(a.dueAt.require());
        }
        d->assignmentTable->setItem(row, ColDueRelative, new QTableWidgetItem(relative));

        auto *publishedItem = new QTableWidgetItem(formatTime(a.publishedAt));
        publishedItem->setFont(monospaceFont());
        d->assignmentTable->setItem(row, ColPublished, publishedItem);
        d->assignmentTable->setItem(row, ColScore, new QTableWidgetItem(formatScore(a.score)));
        auto *sourceItem = new QTableWidgetItem(a.sourceId);
        sourceItem->setFont(monospaceFont());
        d->assignmentTable->setItem(row, ColSourceId, sourceItem);
        d->assignmentTable->setItem(row, ColSource, new QTableWidgetItem(sourceDisplay(courseSource)));

        // 逾期高亮
        if (a.dueAt.hasValue() && a.dueAt.require() < now
            && a.status.hasValue() && a.status.require() == AssignmentStatus::Overdue) {
            for (int col = 0; col < ColumnCount; ++col) {
                if (QTableWidgetItem *item = d->assignmentTable->item(row, col)) {
                    item->setForeground(uitheme::tokens().danger);
                }
            }
        }
    }

    // 空状态：该课程没有作业时显示提示，而不是一张空白表格
    const bool empty = assignments.isEmpty();
    d->assignmentEmptyHint->setVisible(empty);
    d->assignmentTable->setVisible(!empty);
    d->assignmentSummary->setText(QStringLiteral("%1 条作业 · 未提交 %2 · 已逾期 %3")
                                      .arg(assignments.size())
                                      .arg(pending)
                                      .arg(overdue));
}

void MainWindow::refreshLastRun()
{
    const QList<CrawlRun> runs = d->repository->listRecentRuns(1);
    if (runs.isEmpty()) {
        d->lastRunLabel->setText(QStringLiteral("最近一次同步：尚未执行"));
        return;
    }

    const CrawlRun &run = runs.first();
    const QString statusText = runStatusDisplay(run.status);

    QString text = QStringLiteral("最近一次同步：#%1 %2 · 开始 %3 · %4")
                       .arg(run.runId)
                       .arg(statusText)
                       .arg(timeutil::toBeijingDisplay(run.startedAt))
                       .arg(run.finishedAt.has_value()
                                ? QStringLiteral("结束 %1")
                                      .arg(timeutil::toBeijingDisplay(*run.finishedAt))
                                : QStringLiteral("未结束"));
    if (!run.errorMessage.isEmpty()) {
        text += QStringLiteral(" · %1").arg(run.errorMessage);
    }
    d->lastRunLabel->setText(text);
}

void MainWindow::appendLog(const QString &line, bool isError)
{
    if (d->logView == nullptr) {
        return;
    }
    // 再走一次脱敏，作为写入界面的兜底（Logger 已是第一道防线）
    const QString safe = redact::text(line);
    d->logView->appendPlainText(safe);
    if (isError && d->autoScroll->isChecked()) {
        d->logView->verticalScrollBar()->setValue(d->logView->verticalScrollBar()->maximum());
    }
}

void MainWindow::updateActionStates()
{
    const bool njuRunning = d->sync->isRunning();
    const bool paused = d->sync->isPaused();
    const bool busy = njuRunning || chaoxingBusy();

    d->fullScanButton->setEnabled(!busy);
    d->incrementalButton->setEnabled(!busy);
    // 暂停/继续只作用于南大侧（学习通阶段是短任务，只支持整体取消）
    d->pauseButton->setEnabled(njuRunning && !paused);
    d->resumeButton->setEnabled(njuRunning && paused);
    d->cancelButton->setEnabled(busy);
    d->scheduleEnabled->setEnabled(true);
    d->scheduleMinutes->setEnabled(!busy);
}

int MainWindow::configuredIntervalMinutes() const
{
    return d->scheduleMinutes->value();
}

void MainWindow::showLoginWindow(const QString &reason)
{
    if (d->loginWindow == nullptr) {
        return;
    }
    // 已经可见时只把它提到前台，不重复写日志
    const bool alreadyVisible = d->loginWindow->isVisible();
    if (!alreadyVisible && !reason.isEmpty()) {
        appendLog(reason);
        statusBar()->showMessage(reason, 20000);
    }
    d->loginWindow->show();
    d->loginWindow->raise();
    d->loginWindow->activateWindow();
}

void MainWindow::hideLoginWindow()
{
    if (d->loginWindow == nullptr || !d->loginWindow->isVisible()) {
        return;
    }
    d->loginWindow->hide();
    appendLog(QStringLiteral("登录成功，已自动隐藏浏览器窗口。"));
}

// ---------------------------------------------------------------------------
// 学习通（超星）
// ---------------------------------------------------------------------------

bool MainWindow::chaoxingBusy() const
{
    return d->chaoxingSync != nullptr && d->chaoxingSync->isRunning();
}

/**
 * 创建学习通适配器、同步运行器与登录窗口，并接线。
 *
 * 放在浏览器 Profile 初始化之后调用：适配器需要共享同一个持久化
 * Profile（Cookie / 扫码登录态与主页面互通），而 Profile 在 initialize()
 * 之前不存在。
 */
void MainWindow::setupChaoxing()
{
    QWebEngineProfile *profile = d->bridge->profile();
    if (profile == nullptr) {
        appendLog(QStringLiteral("学习通采集不可用：浏览器 Profile 未初始化。"), true);
        return;
    }

    // 登录窗口：默认隐藏，只有需要扫码时才弹出（与南大登录窗口同款交互）。
    // 先建视图、再建页面：页面必须以视图为父构造（见 ChaoxingAdapter 的说明）。
    d->chaoxingLoginWindow = new LoginWindow(this);
    d->chaoxingLoginWindow->setWindowTitle(
        QStringLiteral("登录 · 学习通（扫码完成后窗口自动隐藏）"));
    auto *view = new QWebEngineView(d->chaoxingLoginWindow);
    {
        auto *layout = new QVBoxLayout(d->chaoxingLoginWindow);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(view);
    }

    d->chaoxing = std::make_unique<ChaoxingAdapter>(profile, view);
    d->chaoxingSync =
        std::make_unique<ChaoxingSyncRunner>(d->repository.get(), d->chaoxing.get());

    connect(d->chaoxing.get(), &ChaoxingAdapter::loginRequired, this,
            [this](const QString &reason) {
                appendLog(QStringLiteral("学习通需要登录：%1").arg(reason), true);
                d->chaoxingAuthStatus->setText(QStringLiteral("需要登录"));
                setLabelState(d->chaoxingAuthStatus, "bad");
                showChaoxingLoginWindow(reason);
            });
    connect(d->chaoxing.get(), &ChaoxingAdapter::stageMessage, this,
            [this](const QString &message) { statusBar()->showMessage(message, 8000); });

    // 登录状态区的两个按钮：打开学习通（扫码用）与检查登录（不采集数据）
    connect(d->openChaoxingButton, &QPushButton::clicked, this, [this]() {
        if (d->chaoxing == nullptr) {
            return;
        }
        d->chaoxing->openLoginPage();
        showChaoxingLoginWindow(
            QStringLiteral("已打开学习通：若显示登录页请扫码，成功后窗口会自动隐藏。"));
    });
    connect(d->checkChaoxingButton, &QPushButton::clicked, this,
            [this]() { checkChaoxingSession(); });

    connect(d->chaoxingSync.get(), &ChaoxingSyncRunner::runStarted, this,
            [this](qint64 runId, const QString &mode) {
                Q_UNUSED(mode)
                d->progress->setValue(0);
                d->progressLabel->setText(
                    QStringLiteral("学习通采集开始：正在获取课程列表（运行 #%1）").arg(runId));
                d->chaoxingAuthStatus->setText(QStringLiteral("采集中…"));
                setLabelState(d->chaoxingAuthStatus, "busy");
                updateActionStates();
            });
    connect(d->chaoxingSync.get(), &ChaoxingSyncRunner::progressChanged, this,
            [this](int percent) {
                if (chaoxingBusy()) {
                    d->progress->setValue(percent);
                }
            });
    connect(d->chaoxingSync.get(), &ChaoxingSyncRunner::statusMessage, this,
            [this](const QString &message) {
                d->progressLabel->setText(message);
                statusBar()->showMessage(message, 10000);
            });
    connect(d->chaoxingSync.get(), &ChaoxingSyncRunner::courseFinished, this,
            [this](const QString &courseName, bool ok, const QString &detail) {
                if (!ok) {
                    appendLog(QStringLiteral("学习通课程「%1」：%2").arg(courseName, detail),
                              true);
                }
            });
    connect(d->chaoxingSync.get(), &ChaoxingSyncRunner::loginRequired, this,
            [this](const QString &reason) {
                appendLog(QStringLiteral("学习通登录状态失效：%1").arg(reason), true);
            });
    connect(d->chaoxingSync.get(), &ChaoxingSyncRunner::runFinished, this,
            [this](ChaoxingSyncRunner::Outcome outcome, const QString &message, int succeeded,
                   int failed, int skipped) {
                Q_UNUSED(succeeded)
                Q_UNUSED(failed)
                Q_UNUSED(skipped)
                const bool isError = outcome == ChaoxingSyncRunner::Outcome::Failed
                                     || outcome == ChaoxingSyncRunner::Outcome::WaitingForLogin;
                appendLog(message, isError);
                // 收尾：把进度区从"正在采集…"落到最终结果，否则状态会一直停在中途文案上
                d->progressLabel->setText(message);
                switch (outcome) {
                case ChaoxingSyncRunner::Outcome::Success:
                case ChaoxingSyncRunner::Outcome::Partial:
                    d->progress->setValue(100);
                    d->chaoxingAuthStatus->setText(
                        QStringLiteral("已登录（可访问 %1 门课程）")
                            .arg(countChaoxingPresentCourses()));
                    setLabelState(d->chaoxingAuthStatus, "good");
                    // 数据能取到说明学习通会话有效 → 登录窗口可以收起来了
                    hideChaoxingLoginWindow();
                    break;
                case ChaoxingSyncRunner::Outcome::WaitingForLogin:
                    d->chaoxingAuthStatus->setText(QStringLiteral("需要登录"));
                    setLabelState(d->chaoxingAuthStatus, "bad");
                    break;
                case ChaoxingSyncRunner::Outcome::Failed:
                    d->chaoxingAuthStatus->setText(QStringLiteral("上次采集失败"));
                    setLabelState(d->chaoxingAuthStatus, "warn");
                    break;
                case ChaoxingSyncRunner::Outcome::Cancelled:
                    break;   // 保留取消前的状态文案
                }
                loadPersistedData();
                updateActionStates();
            });

    // 串行触发：每次南大同步结束（按钮 / 定时 / 启动扫描 / iPad）后采集学习通。
    // 取消的那一轮不跟随，避免"取消之后又自己跑起来"。
    connect(d->sync.get(), &SyncManager::runFinished, this,
            [this](const SyncManager::RunSummary &summary) {
                if (summary.outcome == SyncManager::Outcome::Cancelled) {
                    return;
                }
                maybeStartChaoxingSync();
            });
}

void MainWindow::maybeStartChaoxingSync()
{
    if (d->chaoxingSync == nullptr) {
        return;
    }
    if (d->chaoxingSync->isRunning()) {
        return;
    }
    if (d->chaoxing->isBusy()) {
        // 例如用户刚好在点「检查学习通登录」：本轮跳过，避免互相打断
        appendLog(QStringLiteral("学习通正在执行其它任务（如登录检查），本轮采集跳过。"), true);
        return;
    }

    const QString mode = d->lastSyncMode == SyncManager::Mode::Full
                             ? QStringLiteral("full")
                             : QStringLiteral("incremental");
    QString error;
    if (!d->chaoxingSync->start(mode, &error)) {
        appendLog(QStringLiteral("学习通采集未启动：%1").arg(error), true);
    }
}

void MainWindow::showChaoxingLoginWindow(const QString &reason)
{
    if (d->chaoxingLoginWindow == nullptr) {
        return;
    }
    const bool alreadyVisible = d->chaoxingLoginWindow->isVisible();
    if (!alreadyVisible && !reason.isEmpty()) {
        appendLog(reason);
        statusBar()->showMessage(reason, 20000);
    }
    d->chaoxingLoginWindow->show();
    d->chaoxingLoginWindow->raise();
    d->chaoxingLoginWindow->activateWindow();
}

void MainWindow::hideChaoxingLoginWindow()
{
    if (d->chaoxingLoginWindow == nullptr || !d->chaoxingLoginWindow->isVisible()) {
        return;
    }
    d->chaoxingLoginWindow->hide();
    appendLog(QStringLiteral("学习通登录有效，已自动隐藏登录窗口。"));
}

int MainWindow::countChaoxingPresentCourses() const
{
    int count = 0;
    const QString chaoxing = source_keys::kChaoxing.toString();
    for (const Course &course : d->repository->listCourses()) {
        if (course.source == chaoxing) {
            ++count;
        }
    }
    return count;
}

/**
 * 「检查学习通登录」：只做一次首页/课程中心取数，不写任何采集数据。
 *
 * 与南大「检查登录状态」对称：状态栏给出权威结论（是否已登录、能看到几门课程），
 * 需要登录时直接把扫码窗口弹出来。任务进行中时不打扰（避免打断状态机）。
 */
void MainWindow::checkChaoxingSession()
{
    if (d->chaoxing == nullptr) {
        return;
    }
    if (chaoxingBusy() || d->chaoxing->isBusy()) {
        statusBar()->showMessage(QStringLiteral("学习通正在采集，稍后再检查"), 8000);
        return;
    }

    d->chaoxingAuthStatus->setText(QStringLiteral("检查中…"));
    setLabelState(d->chaoxingAuthStatus, "busy");
    d->chaoxing->discoverCourses([this](const ChaoxingAdapter::CourseListResult &result) {
        if (result.error.ok()) {
            d->chaoxingAuthStatus->setText(
                QStringLiteral("已登录（可访问 %1 门课程）").arg(result.courses.size()));
            setLabelState(d->chaoxingAuthStatus, "good");
            hideChaoxingLoginWindow();
            return;
        }
        if (result.error.needsLogin()) {
            d->chaoxingAuthStatus->setText(QStringLiteral("需要登录"));
            setLabelState(d->chaoxingAuthStatus, "bad");
            showChaoxingLoginWindow(
                QStringLiteral("学习通需要登录：请扫码（成功后窗口会自动隐藏）"));
            return;
        }
        d->chaoxingAuthStatus->setText(QStringLiteral("检查失败"));
        setLabelState(d->chaoxingAuthStatus, "warn");
        appendLog(QStringLiteral("学习通登录检查失败：%1").arg(result.error.message), true);
    });
}

void MainWindow::beginSync(SyncManager::Mode mode)
{
    if (d->sync->isRunning()) {
        // 已有任务在跑（例如按钮刚点过、或定时任务触发）
        return;
    }

    // 页面尚未就绪时先自动打开站点：否则整轮同步只会以
    // "page is not ready" 的失败收场，用户看不出真正原因。
    if (!d->bridge->isReady()) {
        appendLog(QStringLiteral("页面尚未就绪，正在打开智汇南雍…"));
        d->bridge->loadHome();
        statusBar()->showMessage(
            QStringLiteral("正在打开站点，请在页面加载完成后再次点击同步"), 15000);
        return;
    }

    QString error;
    if (!d->sync->start(mode, &error)) {
        QMessageBox::warning(this, QStringLiteral("无法开始"),
                             QStringLiteral("启动同步失败：%1").arg(error));
        return;
    }
    d->tabs->setCurrentIndex(0);
    updateActionStates();
}

} // namespace njulms
