#pragma once

#include "adapters/NjuLmsAdapter.h"
#include "browser/WebEnginePageBridge.h"
#include "repository/SqliteRepository.h"
#include "scheduler/Scheduler.h"
#include "sync/SyncManager.h"
#include "web/WebApi.h"

#include <QMainWindow>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <memory>

QT_BEGIN_NAMESPACE
class QCheckBox;
class QCloseEvent;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTabWidget;
class QTableWidget;
QT_END_NAMESPACE

namespace njulms {

/**
 * 桌面管理界面（第一版：简单实用的 Qt Widgets）。
 *
 * 职责边界（需求文档第三节第 7 点）：
 *   - 只负责显示与转发用户操作
 *   - **不**执行耗时的采集与数据库操作；全部通过信号槽交给
 *     BrowserSession / SyncManager / Scheduler，并在界面显示进度
 *
 * 界面元素（对应需求列出的清单）：
 *   登录/打开智汇南雍、登录状态、开始全量采集、立即增量同步、
 *   暂停/继续/取消、课程列表、作业列表、截止时间、当前进度、
 *   最近一次同步结果、错误日志与重新登录提示、定时同步开关与周期。
 */
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    /// 关窗 = 隐藏到托盘（托盘不可用时才真正退出）：iPad 前端与定时同步靠它继续活着
    void closeEvent(QCloseEvent *event) override;

private:
    void buildUi();
    void wireSignals();
    void loadPersistedData();

    /// 启动 iPad 前端（只读 HTTP 服务，仅监听回环地址；失败只记日志）
    void startWebServer();

    /// 创建托盘图标：关窗后程序继续在后台运行
    void buildTray();

    /// 前端用的 JSON 快照；全部由 WebApi 投递到主线程后调用
    QJsonObject buildStatusJson() const;
    QJsonArray buildCoursesJson() const;
    QJsonArray buildAssignmentsJson(const QString &courseId) const;
    /// "pending"（未提交 + 已逾期，按截止时间升序）/ "submitted"（按发布时间降序）
    QJsonArray buildAssignmentsByScope(const QString &scope) const;

    /// 前端的写操作：复用桌面端同一批动作与同一套前置检查
    WebActionResult applyWebSettings(const QJsonObject &patch);
    WebActionResult startWebSync(const QString &mode);
    WebActionResult pauseWebSync();
    WebActionResult resumeWebSync();
    WebActionResult cancelWebSync();
    /// 清空采集数据（保留登录状态）；confirm 必须等于 "DELETE"
    WebActionResult purgeWebData(const QString &confirm);

    /// 访问令牌（首次调用时生成并持久化）；只用于写操作
    QString webAccessToken();

    /// 读日志文件尾部（最多 256 KB，避免整文件读入）
    static QString readLogTail(int lines);

    /// 刷新课程下拉/列表
    void refreshCourseList();

    /// 刷新某门课程的作业表格
    void refreshAssignmentTable(const QString &courseId);

    /// 刷新「最近一次同步结果」区域
    void refreshLastRun();

    /// 把一行文本追加到日志面板（已由 Logger 脱敏）
    void appendLog(const QString &line, bool isError = false);

    /// 依据运行状态更新按钮可用性
    void updateActionStates();

    /// 统一的同步入口（按钮、定时任务、启动扫描都走这里）：
    /// 页面未就绪时先自动打开站点，已有任务在跑时直接忽略。
    void beginSync(SyncManager::Mode mode);

    /// 显示登录窗口（浏览器页面独立成窗口，默认隐藏；会话失效时才弹出）
    void showLoginWindow(const QString &reason = QString());

    /// 隐藏登录窗口（登录成功后自动调用；只隐藏，不销毁页面）
    void hideLoginWindow();

    // ---- 学习通（超星）----

    /// 创建学习通适配器、同步运行器与登录窗口并接线。
    /// 必须在浏览器 Profile 初始化成功之后调用（需要共享同一个 Profile）。
    void setupChaoxing();

    /// 南大同步结束后串行触发学习通采集（开关关闭 / 正在运行 / 未初始化时跳过）
    void maybeStartChaoxingSync();

    /// 学习通采集是否正在运行
    bool chaoxingBusy() const;

    /// 显示学习通登录窗口（会话失效、需要扫码时调用）
    void showChaoxingLoginWindow(const QString &reason);

    /// 隐藏学习通登录窗口（采集成功说明会话有效）
    void hideChaoxingLoginWindow();

    /// 「检查学习通登录」：只取课程列表判断会话，不写采集数据
    void checkChaoxingSession();

    /// 当前库里 present 的学习通课程数（登录状态文案用）
    int countChaoxingPresentCourses() const;

    // ---- 雨课堂（yuketang.cn）----

    /// 创建雨课堂的站点页面桥与登录窗口（阶段 0：人工登录）
    void setupYuketang();

    /// 显示雨课堂登录窗口
    void showYuketangLoginWindow(const QString &reason = QString());

    /// 学习通结束后串行触发雨课堂采集
    void maybeStartYuketangSync();

    /// 雨课堂采集是否正在运行
    bool yuketangBusy() const;

    /// 「检查雨课堂登录」：只取课程列表判断会话
    void checkYuketangSession();

    /// 当前库里 present 的雨课堂课程数（登录状态文案用）
    int countYuketangPresentCourses() const;

    /// 读取界面上的定时周期设置
    int configuredIntervalMinutes() const;

    class Impl;
    std::unique_ptr<Impl> d;
};

} // namespace njulms
