#pragma once

#include "models/Assignment.h"
#include "models/Course.h"

#include <QDateTime>
#include <QList>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>

#include <optional>

namespace njulms {

/// 数据库当前结构版本。任何表结构变更都必须递增并在 migrate() 中提供迁移分支。
/// v3：courses 增加 source（多站点：nju-lms / chaoxing）
constexpr int kSchemaVersion = 3;

/// 采集任务运行状态（crawl_runs.status）
struct CrawlRun
{
    enum class Status { Running, Success, Partial, Failed, NeedLogin, Cancelled };

    qint64 runId = 0;
    QString taskKey;
    QString mode;  ///< "full" / "incremental"
    Status status = Status::Running;
    QDateTime startedAt;
    std::optional<QDateTime> finishedAt;
    QString errorMessage;

    static const char *statusKey(Status s) noexcept;
    static Status statusFromKey(const QString &key);
};

/// 采集批次（crawl_batches）
struct CrawlBatch
{
    enum class Status { Pending, Committed, Failed };

    qint64 batchId = 0;
    qint64 runId = 0;
    int batchNo = 0;
    QString cursorBefore;
    QString cursorAfter;
    Status status = Status::Pending;
    std::optional<QDateTime> committedAt;
};

/// 同步断点状态（sync_state）
struct SyncState
{
    QString taskKey;
    QString mode;
    QString cursor;
    std::optional<QDateTime> lastSuccessAt;
    bool fullScanCompleted = false;
    QDateTime updatedAt;
};

/// 一次批量写入的结果，用于 UI 展示与测试断言
struct BatchApplyResult
{
    bool ok = false;
    int inserted = 0;
    int updated = 0;
    int unchanged = 0;
    int markedMissing = 0;
    /// 本轮快照无法证明完整（total 缺失，或翻页期间列表变化导致条数对不上）。
    /// 此时**不做缺失检测、不推进断点**（需求第四节：不确定是否完整的批次不能标记成功）。
    bool partialSnapshot = false;
    /// 被保护而未覆盖的「旧值仍有效」字段名（用于验证场景 F）
    QStringList preservedFields;
    QString error;
};

/**
 * 字段级解析异常记录（parse_errors 表）。
 *
 * 只记录字段名与错误类型，**不记录字段正文**，避免把作业内容写入日志。
 */
struct ParseErrorRecord
{
    qint64 runId = 0;
    QString courseId;
    QString sourceId;
    QString field;
    QString reason;
    QDateTime observedAt;
};

/**
 * SQLite 持久化模块。
 *
 * 职责边界（对应需求文档第三节第 4 点）：
 *   - 只有本模块可以执行 SQL。BrowserSession 与 Parser 不得直接操作数据库。
 *   - 全部语句使用参数绑定，禁止字符串拼接 SQL。
 *   - 写入使用事务；启用 WAL 与外键约束。
 *
 * 线程约定：QSqlDatabase 连接不可跨线程使用。本类实例应在**单个线程**内使用
 * （当前实现运行在 GUI 线程的事件循环里，配合异步的页面访问）。
 */
class SqliteRepository
{
public:
    SqliteRepository();
    ~SqliteRepository();

    SqliteRepository(const SqliteRepository &) = delete;
    SqliteRepository &operator=(const SqliteRepository &) = delete;

    /**
     * 打开（或创建）数据库并初始化结构。
     * @param databasePath 数据库文件路径；目录不存在时创建。
     * @param error        失败时输出可读原因
     */
    bool open(const QString &databasePath, QString *error = nullptr);
    void close();
    bool isOpen() const;

    /// 运行数据目录：环境变量 NJULMS_DATA_DIR 优先，否则可执行文件同级 data/
    static QString defaultDataDir();
    static QString defaultDatabasePath();

    /// 最近一次错误
    QString lastError() const;

    // ---- 事务 ---------------------------------------------------------------

    bool beginTransaction(QString *error = nullptr);
    bool commit(QString *error = nullptr);
    bool rollback(QString *error = nullptr);
    bool inTransaction() const;

    // ---- 课程 ---------------------------------------------------------------

    /**
     * 以一次完整快照的方式写入课程列表。
     *
     * 快照语义：本次快照中**没有**出现的课程会被标记 present=0（软删除），
     * 而不是物理删除——课程可能只是本学期未开放，历史作业数据仍应保留
     * （需求场景 G：课程列表变化时必须重新发现，不能永久依赖首次列表）。
     *
     * ⚠️ `source` 必须给出：软删除（present=0）**只在同一个来源内生效**——
     * 否则写学习通的快照会把智汇南雍的课程全部标记为消失。
     * 参数同时是整批课程的唯一来源：写入行的 source 与软删除范围都由它决定；
     * 若某门课自带 source 且与参数不一致，快照整体失败（宁可失败也不能错删）。
     */
    bool applyCourseSnapshot(const QString &source, const QList<Course> &courses,
                             QString *error = nullptr);

    QList<Course> listCourses(bool includeMissing = false) const;

    // ---- 作业 ---------------------------------------------------------------

    /**
     * 以一次「课程作业完整快照」为单位写入。
     *
     * 这是断点续传的原子单元（需求第四节场景 A、B）：一次调用内的
     * 插入 / 更新 / 缺失标记 / 批次记录 / 断点更新处于**同一个事务**，
     * 任何一步失败都整体回滚，不会出现「数据没写但断点前进」。
     *
     * 字段级合并规则（需求场景 F）：
     *   - Present / Absent → 覆盖数据库中的值（Absent 写 NULL）
     *   - Missing / Invalid → **保留库中原值**，并记入 preservedFields
     *
     * @param courseId    课程 ID
     * @param assignments 本次快照解析出的作业（应为完整集合）
     * @param runId       所属运行 ID；0 表示不属于任何运行
     * @param batchNo     批次序号；0 表示不记录批次行
     * @param cursorBefore/After 断点快照，用于审计
     */
    BatchApplyResult applyAssignmentSnapshot(const QString &courseId,
                                             const QList<Assignment> &assignments,
                                             qint64 runId = 0,
                                             int batchNo = 0,
                                             const QString &cursorBefore = QString(),
                                             const QString &cursorAfter = QString(),
                                             bool snapshotComplete = true,
                                             const QString &mode = QStringLiteral("full"));

    QList<Assignment> listAssignments(const QString &courseId, bool includeMissing = false) const;
    std::optional<Assignment> findAssignment(const QString &courseId, const QString &sourceId) const;
    int countAssignments(const QString &courseId) const;

    // ---- 解析异常 -----------------------------------------------------------

    bool recordParseErrors(const QList<ParseErrorRecord> &records, QString *error = nullptr);
    QList<ParseErrorRecord> listRecentParseErrors(int limit = 100) const;

    // ---- 运行记录 -----------------------------------------------------------

    qint64 startRun(const QString &taskKey, const QString &mode, QString *error = nullptr);
    bool finishRun(qint64 runId, CrawlRun::Status status, const QString &errorMessage = QString(),
                   QString *error = nullptr);
    std::optional<CrawlRun> findRun(qint64 runId) const;
    QList<CrawlRun> listRecentRuns(int limit = 20) const;

    /// 程序启动时调用：把上次异常退出（仍为 Running）的运行标记为失败。
    /// 返回被修正的运行数量（需求场景 A 的「重启后检查未完成任务」）。
    int reapOrphanedRuns(QString *error = nullptr);

    /**
     * 历史表保留策略（v2 新增）：删除过旧的运行记录、批次与字段级解析异常，
     * 避免长期运行后 crawl_runs / crawl_batches / parse_errors 无限增长。
     *
     * 同时满足两个下限：至少保留最近 keepRuns 次运行，且至少保留最近 keepDays 天的记录。
     * @return 删除的行数；失败返回 -1
     */
    int pruneCrawlHistory(int keepRuns = 200, int keepDays = 30, QString *error = nullptr);

    /**
     * 清空全部**采集数据**（v2 新增，界面上是「删除全部数据」按钮）：
     * courses / assignments / sync_state / crawl_runs / crawl_batches / parse_errors，
     * 保留 schema_version（结构版本不变，表结构继续可用）。
     *
     * 不涉及浏览器 Profile（登录状态保留）、也不动日志文件。
     * @return 删除的行数；失败返回 -1（事务整体回滚）
     */
    int clearAllData(QString *error = nullptr);

    // ---- 批次 ---------------------------------------------------------------

    std::optional<CrawlBatch> findLastCommittedBatch(qint64 runId) const;
    QList<CrawlBatch> listBatches(qint64 runId) const;

    // ---- 同步断点 -----------------------------------------------------------

    std::optional<SyncState> findSyncState(const QString &taskKey) const;
    bool upsertSyncState(const SyncState &state, QString *error = nullptr);
    QList<SyncState> listSyncStates() const;

    /// 构造课程级任务键，例如 "course:11311"
    static QString courseTaskKey(const QString &courseId);

    /// 课程发现任务的键
    static QString courseDiscoveryTaskKey();

    /// 「上一次成功完成完整全量扫描」的标记（v2：定时调度据此决定何时做全量对账）
    static QString fullReconcileTaskKey();

    /// 待办集合快照的键（v2：增量同步据此做差异比对，内容存在 sync_state.cursor）
    static QString todoSnapshotTaskKey();

private:
    class Impl;
    std::unique_ptr<Impl> d;
};

} // namespace njulms
