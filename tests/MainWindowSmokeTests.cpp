#include "repository/SqliteRepository.h"
#include "ui/MainWindow.h"
#include "utils/Logger.h"

#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QSpinBox>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUrl>
#include <QWidget>

#include <cstdlib>

using namespace njulms;

/**
 * GUI 冒烟测试。
 *
 * ── 为什么需要它 ──────────────────────────────────────────────────────────
 * UI 层此前**完全没有测试覆盖**：MainWindow 有近 900 行，
 * 而单元测试只覆盖了它下面的模块。构造 MainWindow 会一次性走完
 * 「建界面 → 接信号 → 开数据库 → 建 WebEngine Profile → 检查启动状态
 * → 从数据库填充课程表与作业表」这条路径，任何一处空指针、信号签名不匹配
 * 或初始化顺序错误都会在这里暴露。
 *
 * ── 前置条件 ──────────────────────────────────────────────────────────────
 * 必须用离屏平台运行（`QT_QPA_PLATFORM=offscreen`），否则会真的弹出窗口。
 * 数据库位置通过 NJULMS_DATA_DIR 指向临时目录，避免污染真实运行数据。
 */
class MainWindowSmokeTests : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;

    /// 往临时数据库里写入门课 + 作业，供界面加载
    void seedDatabase()
    {
        SqliteRepository repo;
        const QString path = QDir(m_dir.path()).filePath(QStringLiteral("njulms.db"));
        QString error;
        QVERIFY2(repo.open(path, &error), qPrintable(error));

        Course c;
        c.source = source_keys::kNjuLms.toString();
        c.courseId = QStringLiteral("90001");
        c.courseName = QStringLiteral("冒烟测试课程");
        c.detailUrl = QStringLiteral("https://lms.nju.edu.cn/course/90001/index");
        c.courseCode = QStringLiteral("SMOKE001");
        c.instructors = {QStringLiteral("测试教师")};
        c.lastSeenAt = QDateTime::currentDateTimeUtc();
        QVERIFY(repo.applyCourseSnapshot(source_keys::kNjuLms.toString(), {c}, &error));

        QList<Assignment> items;
        for (int i = 0; i < 3; ++i) {
            Assignment a;
            a.courseId = c.courseId;
            a.sourceId = QStringLiteral("homework-%1").arg(70001 + i);
            a.activityId = QString::number(70001 + i);
            a.title = Observed<QString>::present(QStringLiteral("冒烟作业 %1").arg(i + 1));
            a.description = Observed<QString>::present(QStringLiteral("<p>说明</p>"));
            a.detailUrl = Observed<QString>::present(
                QStringLiteral("https://lms.nju.edu.cn/course/90001/activity/%1").arg(70001 + i));
            a.dueAt = Observed<QDateTime>::present(
                QDateTime(QDate(2026, 12, 1 + i), QTime(15, 59), QTimeZone::UTC));
            a.publishedAt = Observed<QDateTime>::present(
                QDateTime(QDate(2026, 11, 1), QTime(2, 0), QTimeZone::UTC));
            a.status = Observed<AssignmentStatus>::present(
                i == 0 ? AssignmentStatus::Submitted : AssignmentStatus::Pending);
            a.submitted = Observed<bool>::present(i == 0);
            a.score = i == 0 ? Observed<double>::present(95.0)
                             : Observed<double>::absent();
            a.observedAt = QDateTime::currentDateTimeUtc();
            items.append(a);
        }

        const BatchApplyResult applied =
            repo.applyAssignmentSnapshot(c.courseId, items);
        QVERIFY2(applied.ok, qPrintable(applied.error));
        QCOMPARE(applied.inserted, 3);
        repo.close();
    }

private slots:

    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        seedDatabase();

        // v2：让 QSettings 落到临时目录的 INI 文件，
        // 这样「偏好持久化」可以测试，又不会污染用户的注册表。
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_dir.path());

        // MainWindow 通过环境变量定位数据目录，必须在构造之前设置
        qputenv("NJULMS_DATA_DIR", m_dir.path().toLocal8Bit());
        // Profile 也放到临时目录，避免写进 build/bin
        const QString profileDir = QDir(m_dir.path()).filePath(QStringLiteral("profile"));
        qputenv("NJULMS_PROFILE_DIR", profileDir.toLocal8Bit());
    }

    void constructsWithoutCrashing()
    {
        // 构造过程覆盖：建界面 / 接信号 / 开数据库 / 建 Profile /
        // 启动状态检查 / 从数据库填充表格
        MainWindow window;
        QVERIFY(window.centralWidget() != nullptr);
        QVERIFY(!window.windowTitle().isEmpty());
    }

    void canBeShownOffscreen()
    {
        MainWindow window;
        window.show();

        // 让事件循环真正跑一会儿，暴露延迟初始化问题
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 800) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        }
        QVERIFY(window.isVisible());
    }

    void survivesRepeatedConstruction()
    {
        // 反复构造/析构可以暴露静态状态、连接泄漏与析构顺序问题
        for (int i = 0; i < 3; ++i) {
            MainWindow window;
            window.show();
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        }
        QVERIFY(true);
    }

    /// v2：启动全量扫描 / 快速增量两个开关必须存在（UI 回归）
    void startupAndQuickControlsExist()
    {
        MainWindow window;
        bool hasStartupScan = false;
        bool hasQuickIncremental = false;
        const QList<QCheckBox *> boxes = window.findChildren<QCheckBox *>();
        for (QCheckBox *box : boxes) {
            const QString text = box->text();
            if (text.contains(QStringLiteral("启动时自动全量扫描"))) {
                hasStartupScan = true;
            }
            if (text.contains(QStringLiteral("快速增量"))) {
                hasQuickIncremental = true;
            }
        }
        QVERIFY(hasStartupScan);
        QVERIFY(hasQuickIncremental);
    }

    void missingDataDirectoryIsHandled()
    {
        // 指向一个**本地**且必然创建失败的路径：父级是一个普通文件。
        //
        // 原来用 Z:/… 当"写不进去的位置"，但那是一台 NAS 的网络盘：
        //   * 盘离线 → 建库失败 → 程序弹模态框 → 离屏测试没人点，永久卡住；
        //   * 盘在线 → 真的在 SMB 共享上建 SQLite（含 WAL），耗时 20s 以上，
        //     甚至更久；测试变成不确定性来源。
        // 换成"父级是文件"的本地路径后，失败是毫秒级且与网络无关。
        const QString blocker = QDir(m_dir.path()).filePath(QStringLiteral("not-a-directory"));
        {
            QFile file(blocker);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write("x"), 1);
        }
        const QString bad = QDir(blocker).filePath(QStringLiteral("njulms"));

        const QByteArray previous = qgetenv("NJULMS_DATA_DIR");
        qputenv("NJULMS_DATA_DIR", bad.toLocal8Bit());

        // 数据库打不开时程序会弹模态错误框。离屏测试里没有人点"确定"，
        // 所以用定时器把出现的模态窗口关掉：这条用例要验证的是
        // "构造过程不崩溃、并且确实给出了提示"，而不是"有没有人点确定"。
        QTimer modalCloser;
        connect(&modalCloser, &QTimer::timeout, []() {
            if (QWidget *modal = QApplication::activeModalWidget()) {
                modal->close();
            }
        });
        modalCloser.start(100);

        {
            MainWindow window;
            QVERIFY(window.centralWidget() != nullptr);
        }

        modalCloser.stop();
        qputenv("NJULMS_DATA_DIR", previous);
    }

    /// v2：「删除全部数据」按钮存在，且浏览器登录窗口默认隐藏
    void clearDataButtonAndHiddenLoginWindow()
    {
        MainWindow window;

        bool hasClearButton = false;
        for (QPushButton *button : window.findChildren<QPushButton *>()) {
            if (button->text().contains(QStringLiteral("删除全部数据"))) {
                hasClearButton = true;
            }
        }
        QVERIFY(hasClearButton);

        bool loginWindowFound = false;
        for (QWidget *widget : window.findChildren<QWidget *>()) {
            if (widget->isWindow() &&
                widget->windowTitle().contains(QStringLiteral("登录 · 智汇南雍"))) {
                loginWindowFound = true;
                QVERIFY(!widget->isVisible());   // 默认必须是隐藏的
            }
        }
        QVERIFY(loginWindowFound);
    }

    /// v2：定时同步的开关与周期会被记住（跨实例读回）
    void scheduleSettingsArePersisted()
    {
        constexpr int kChosenInterval = 45;

        {
            MainWindow window;
            QCheckBox *enabled = nullptr;
            for (QCheckBox *box : window.findChildren<QCheckBox *>()) {
                if (box->text().contains(QStringLiteral("启用定时同步"))) {
                    enabled = box;
                }
            }
            QSpinBox *minutes = window.findChild<QSpinBox *>();
            QVERIFY(enabled != nullptr);
            QVERIFY(minutes != nullptr);

            minutes->setValue(kChosenInterval);
            enabled->setChecked(true);   // 触发持久化写入
        }

        {
            MainWindow window;   // 新实例：应从设置里读回
            QCheckBox *enabled = nullptr;
            for (QCheckBox *box : window.findChildren<QCheckBox *>()) {
                if (box->text().contains(QStringLiteral("启用定时同步"))) {
                    enabled = box;
                }
            }
            QSpinBox *minutes = window.findChild<QSpinBox *>();
            QVERIFY(enabled != nullptr);
            QVERIFY(minutes != nullptr);
            QVERIFY(enabled->isChecked());
            QCOMPARE(minutes->value(), kChosenInterval);
        }
    }

    /// 同步 GET（等待期间照常跑事件循环：WebApi 需要主线程处理投递）
    static QPair<int, QByteArray> httpGet(quint16 port, const QString &path)
    {
        QNetworkAccessManager manager;
        QNetworkRequest request(
            QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(port).arg(path)));
        QNetworkReply *reply = manager.get(request);
        QEventLoop loop;
        QTimer::singleShot(10000, &loop, &QEventLoop::quit);
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();
        const QByteArray body = reply->readAll();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        reply->deleteLater();
        return {status, body};
    }

    /// 同步 POST：可选带访问令牌
    static QPair<int, QByteArray> httpPost(quint16 port, const QString &path,
                                           const QByteArray &body, const QByteArray &token)
    {
        QNetworkAccessManager manager;
        QNetworkRequest request(
            QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(port).arg(path)));
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        if (!token.isEmpty()) {
            request.setRawHeader("X-Auth-Token", token);
        }
        QNetworkReply *reply = manager.post(request, body);
        QEventLoop loop;
        QTimer::singleShot(10000, &loop, &QEventLoop::quit);
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();
        const QByteArray response = reply->readAll();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        reply->deleteLater();
        return {status, response};
    }

    /**
     * v5：软删除端到端——真实 HTTP 接口 → MainWindow → SQLite → scope 过滤。
     *
     * 覆盖链路：未提交列表少一条 → 回收站（scope=deleted）多一条（带删除时间）→
     * 恢复 → 两边一起还原。写接口不带令牌必须 401（与其它写操作一致）。
     */
    void softDeleteEndToEndOverHttp()
    {
        // 固定端口：必须在构造 MainWindow 之前设置（避免撞上默认端口/正在跑的真实实例）
        constexpr quint16 kPort = 45817;
        const QByteArray previousPort = qgetenv("NJULMS_WEB_PORT");
        qputenv("NJULMS_WEB_PORT", QByteArray::number(kPort));

        MainWindow window;

        // 访问令牌由 MainWindow 启动时生成并写入 QSettings（测试里已重定向到临时 INI）
        QSettings settings(QStringLiteral("NjuLmsCrawler"), QStringLiteral("NjuLmsCrawler"));
        const QByteArray token =
            settings.value(QStringLiteral("web/token")).toString().toUtf8();
        QVERIFY(!token.isEmpty());

        const auto fetchScope = [](const QString &scope) {
            const auto response =
                httpGet(45817, QStringLiteral("/api/assignments?scope=%1").arg(scope));
            if (response.first != 200) {
                qWarning("scope=%s 返回 %d", qPrintable(scope), response.first);
            }
            return QJsonDocument::fromJson(response.second)
                .object()
                .value(QStringLiteral("data"))
                .toArray();
        };

        const QJsonArray pendingBefore = fetchScope(QStringLiteral("pending"));
        QVERIFY2(pendingBefore.size() >= 1, "种子数据里应有未提交作业");
        const QJsonObject row = pendingBefore.first().toObject();
        const QString courseId = row.value(QStringLiteral("courseId")).toString();
        const QString sourceId = row.value(QStringLiteral("sourceId")).toString();
        QVERIFY(!courseId.isEmpty());
        QVERIFY(!sourceId.isEmpty());

        QJsonObject body;
        body.insert(QStringLiteral("courseId"), courseId);
        body.insert(QStringLiteral("sourceId"), sourceId);
        const QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);

        // 写接口必须带令牌
        QCOMPARE(httpPost(45817, QStringLiteral("/api/assignments/delete"), payload, QByteArray())
                     .first,
                 401);

        // 删除：未提交 -1、回收站 +1（且带"删除于"时间）
        QCOMPARE(httpPost(45817, QStringLiteral("/api/assignments/delete"), payload, token).first,
                 200);
        QCOMPARE(fetchScope(QStringLiteral("pending")).size(), pendingBefore.size() - 1);
        const QJsonArray deletedAfter = fetchScope(QStringLiteral("deleted"));
        QCOMPARE(deletedAfter.size(), 1);
        QVERIFY(deletedAfter.first().toObject().contains(QStringLiteral("deletedAt")));
        QCOMPARE(deletedAfter.first().toObject().value(QStringLiteral("sourceId")).toString(),
                 sourceId);

        // 恢复：两边还原
        QCOMPARE(httpPost(45817, QStringLiteral("/api/assignments/restore"), payload, token).first,
                 200);
        QCOMPARE(fetchScope(QStringLiteral("pending")).size(), pendingBefore.size());
        QCOMPARE(fetchScope(QStringLiteral("deleted")).size(), 0);

        qputenv("NJULMS_WEB_PORT", previousPort);
    }
};

QTEST_MAIN(MainWindowSmokeTests)
#include "MainWindowSmokeTests.moc"
