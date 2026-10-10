#include "SqliteRepository.h"

#include "parser/AssignmentParser.h"
#include "utils/Logger.h"
#include "utils/TimeUtils.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QStringList>
#include <QVariant>

#include <utility>

namespace njulms {
namespace {

// ---------------------------------------------------------------------------
// 建表语句
//
// 表与列严格对应需求文档第三节第 4 点，未新增未要求的列。
// schema_version 表用于结构变更管理（需求第八节第 8 点）。
//
// ⚠️ 必须拆成**单条语句**逐条执行：QSQLITE 驱动的 QSqlQuery::exec() 不接受
// 一次传入多条语句（会报 "Unable to execute multiple statements at a time"）。
// 因此这里返回语句列表，由 open() 循环执行。
// ---------------------------------------------------------------------------

QStringList schemaStatements()
{
    return {
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS schema_version ("
            "  version    INTEGER NOT NULL PRIMARY KEY,"
            "  applied_at TEXT    NOT NULL)"),

        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS courses ("
            "  course_id     TEXT NOT NULL PRIMARY KEY,"
            "  course_name   TEXT,"
            "  detail_url    TEXT,"
            "  course_code   TEXT,"
            "  department    TEXT,"
            "  instructors   TEXT,"
            "  source        TEXT NOT NULL DEFAULT 'nju-lms',"
            "  semester_code TEXT,"
            "  year_code     TEXT,"
            "  present       INTEGER NOT NULL DEFAULT 1,"
            "  last_seen_at  TEXT,"
            "  last_synced_at   TEXT,"
            "  assignment_count INTEGER)"),

        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS assignments ("
            "  id                INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT,"
            "  course_id         TEXT    NOT NULL,"
            "  source_id         TEXT    NOT NULL,"
            "  activity_id       TEXT,"
            "  title             TEXT,"
            "  description       TEXT,"
            "  detail_url        TEXT,"
            "  published_at      TEXT,"
            "  due_at            TEXT,"
            "  status            TEXT,"
            "  submitted         INTEGER,"
            "  score             REAL,"
            "  content_hash      TEXT,"
            "  content_signature TEXT,"
            "  observed_at       TEXT,"
            "  first_seen_at     TEXT,"
            "  last_seen_at      TEXT,"
            "  present           INTEGER NOT NULL DEFAULT 1,"
            "  UNIQUE (course_id, source_id))"),

        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS sync_state ("
            "  task_key            TEXT NOT NULL PRIMARY KEY,"
            "  mode                TEXT,"
            "  cursor              TEXT,"
            "  last_success_at     TEXT,"
            "  full_scan_completed INTEGER NOT NULL DEFAULT 0,"
            "  updated_at          TEXT)"),

        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS crawl_runs ("
            "  run_id        INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT,"
            "  task_key      TEXT,"
            "  mode          TEXT,"
            "  status        TEXT NOT NULL,"
            "  started_at    TEXT,"
            "  finished_at   TEXT,"
            "  error_message TEXT)"),

        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS crawl_batches ("
            "  batch_id      INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT,"
            "  run_id        INTEGER NOT NULL,"
            "  batch_no      INTEGER NOT NULL,"
            "  cursor_before TEXT,"
            "  cursor_after  TEXT,"
            "  status        TEXT NOT NULL,"
            "  committed_at  TEXT,"
            "  UNIQUE (run_id, batch_no))"),

        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS parse_errors ("
            "  id          INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT,"
            "  run_id      INTEGER,"
            "  course_id   TEXT,"
            "  source_id   TEXT,"
            "  field       TEXT NOT NULL,"
            "  reason      TEXT,"
            "  observed_at TEXT)"),

        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_assignments_course"
                       " ON assignments(course_id)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_assignments_due"
                       " ON assignments(due_at)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_assignments_course_due"
                       " ON assignments(course_id, due_at)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_runs_started"
                       " ON crawl_runs(started_at DESC)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_batches_run"
                       " ON crawl_batches(run_id, batch_no)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_parse_errors_observed"
                       " ON parse_errors(observed_at DESC)"),
    };
}

/// 把库中的可空字符串列还原为 Observed<QString>
Observed<QString> observedString(const QVariant &v)
{
    if (v.isNull()) {
        return Observed<QString>::absent();
    }
    return Observed<QString>::present(v.toString());
}

Observed<QDateTime> observedTime(const QVariant &v)
{
    if (v.isNull()) {
        return Observed<QDateTime>::absent();
    }
    if (const auto parsed = timeutil::parseSiteTime(v.toString()); parsed.has_value()) {
        return Observed<QDateTime>::present(*parsed);
    }
    // 库里的值解析不出来，视为无效：不得据此覆盖
    return Observed<QDateTime>::invalid();
}

Observed<bool> observedBool(const QVariant &v)
{
    if (v.isNull()) {
        return Observed<bool>::absent();
    }
    return Observed<bool>::present(v.toInt() != 0);
}

Observed<double> observedNumber(const QVariant &v)
{
    if (v.isNull()) {
        return Observed<double>::absent();
    }
    return Observed<double>::present(v.toDouble());
}

/**
 * 字段级合并：新值有权威结论则采用新值，否则保留旧值。
 * @return 是否保留了旧值（用于统计与测试断言）
 */
template <typename T>
bool mergeObserved(Observed<T> &incoming, const Observed<T> &existing)
{
    if (incoming.isAuthoritative()) {
        return false;  // 新值可信，采用新值
    }
    // 新值不可信（Missing / Invalid）→ 保留旧值。
    // 即使旧值是 Absent 也保留（Absent 代表「确认空」这一结论，比「不知道」更强）。
    incoming = existing;
    return true;
}

QString joinList(const QStringList &list)
{
    return list.join(QStringLiteral("\x1f"));
}

QStringList splitList(const QString &joined)
{
    if (joined.isEmpty()) {
        return {};
    }
    return joined.split(QLatin1Char('\x1f'), Qt::SkipEmptyParts);
}

} // namespace

// ---------------------------------------------------------------------------
// CrawlRun / CrawlBatch 枚举转换
// ---------------------------------------------------------------------------

const char *CrawlRun::statusKey(Status s) noexcept
{
    switch (s) {
    case Status::Running:   return "running";
    case Status::Success:   return "success";
    case Status::Partial:   return "partial";
    case Status::Failed:    return "failed";
    case Status::NeedLogin: return "need_login";
    case Status::Cancelled: return "cancelled";
    }
    return "failed";
}

CrawlRun::Status CrawlRun::statusFromKey(const QString &key)
{
    if (key == QLatin1String("running"))    return Status::Running;
    if (key == QLatin1String("success"))    return Status::Success;
    if (key == QLatin1String("partial"))    return Status::Partial;
    if (key == QLatin1String("need_login")) return Status::NeedLogin;
    if (key == QLatin1String("cancelled"))  return Status::Cancelled;
    return Status::Failed;
}

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

class SqliteRepository::Impl
{
public:
    QSqlDatabase db;
    QString connectionName;
    QString databasePath;
    mutable QString lastError;
    int transactionDepth = 0;

    bool execRaw(const QString &sql)
    {
        QSqlQuery query(db);
        if (!query.exec(sql)) {
            lastError = query.lastError().text();
            qCWarning(lcRepo) << "SQL failed:" << query.lastError().text();
            return false;
        }
        return true;
    }

    bool tableExists(const QString &name) const
    {
        QSqlQuery query(db);
        query.prepare(QStringLiteral(
            "SELECT 1 FROM sqlite_master WHERE type='table' AND name = :n"));
        query.bindValue(QStringLiteral(":n"), name);
        if (!query.exec()) {
            return false;
        }
        return query.next();
    }

    /// 读取一门课程的全部既有记录（key = source_id），供快照合并使用。
    /// 一次查询代替"每条作业一次 SELECT"，把数据库往返从 O(行数) 降到 O(1)。
    QHash<QString, Assignment> loadExistingMap(const QString &courseId) const;

    /// 表里是否存在某一列（用于让迁移幂等）
    bool columnExists(const QString &table, const QString &column) const;

    /// 把旧结构迁移到当前版本（逐列、幂等）
    bool migrateSchema(int fromVersion);
};

bool SqliteRepository::Impl::columnExists(const QString &table, const QString &column) const
{
    QSqlQuery query(db);
    if (!query.exec(QStringLiteral("PRAGMA table_info(%1)").arg(table))) {
        return false;
    }
    while (query.next()) {
        if (query.value(1).toString() == column) {
            return true;
        }
    }
    return false;
}

bool SqliteRepository::Impl::migrateSchema(int fromVersion)
{
    // ALTER TABLE ADD COLUMN 是 SQLite 里可幂等重放的结构变更；
    // 每列都先查 PRAGMA，既避免重复添加报错，也兼容"全新库已含新列"。
    const auto addColumn = [this](const QString &table, const QString &column,
                                  const QString &type) {
        if (columnExists(table, column)) {
            return true;
        }
        return execRaw(QStringLiteral("ALTER TABLE %1 ADD COLUMN %2 %3")
                           .arg(table, column, type));
    };

    if (fromVersion < 2) {
        // v2：课程级同步元数据，取代「未变作业逐行 touch last_seen_at」的写放大
        if (!addColumn(QStringLiteral("courses"), QStringLiteral("last_synced_at"),
                       QStringLiteral("TEXT")) ||
            !addColumn(QStringLiteral("courses"), QStringLiteral("assignment_count"),
                       QStringLiteral("INTEGER"))) {
            return false;
        }
    }

    if (fromVersion < 3) {
        // v3：多站点支持——课程带上来源；历史数据默认视为智汇南雍。
        if (!addColumn(QStringLiteral("courses"), QStringLiteral("source"),
                       QStringLiteral("TEXT NOT NULL DEFAULT 'nju-lms'"))) {
            return false;
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// 构造 / 析构 / 打开
// ---------------------------------------------------------------------------

SqliteRepository::SqliteRepository()
    : d(std::make_unique<Impl>())
{
}

SqliteRepository::~SqliteRepository()
{
    close();
}

QString SqliteRepository::defaultDataDir()
{
    const QByteArray env = qgetenv("NJULMS_DATA_DIR");
    if (!env.isEmpty()) {
        return QString::fromLocal8Bit(env);
    }
    if (QCoreApplication::instance() != nullptr) {
        const QString appDir = QCoreApplication::applicationDirPath();
        if (!appDir.isEmpty()) {
            return QDir(appDir).filePath(QStringLiteral("data"));
        }
    }
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    return QDir(base).filePath(QStringLiteral("data"));
}

QString SqliteRepository::defaultDatabasePath()
{
    return QDir(defaultDataDir()).filePath(QStringLiteral("njulms.db"));
}

bool SqliteRepository::open(const QString &databasePath, QString *error)
{
    const auto fail = [&](const QString &message) {
        d->lastError = message;
        if (error != nullptr) {
            *error = message;
        }
        qCCritical(lcRepo) << "open failed:" << message;
        return false;
    };

    if (d->db.isOpen()) {
        close();
    }

    // 每个实例使用独立连接名，避免多实例互相覆盖连接
    if (d->connectionName.isEmpty()) {
        d->connectionName = QStringLiteral("njulms_%1")
                                .arg(reinterpret_cast<quintptr>(this), 0, 16);
    }

    const QFileInfo info(databasePath);
    if (!QDir().mkpath(info.absolutePath())) {
        return fail(QStringLiteral("cannot create data directory: %1").arg(info.absolutePath()));
    }

    QSqlDatabase db = QSqlDatabase::contains(d->connectionName)
                          ? QSqlDatabase::database(d->connectionName, false)
                          : QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), d->connectionName);
    db.setDatabaseName(databasePath);
    if (!db.open()) {
        return fail(QStringLiteral("cannot open database: %1").arg(db.lastError().text()));
    }
    d->db = db;
    d->databasePath = databasePath;

    // WAL 提升并发读写表现；foreign_keys 保证约束生效（SQLite 默认关闭）。
    if (!d->execRaw(QStringLiteral("PRAGMA journal_mode = WAL"))) {
        return fail(QStringLiteral("cannot enable WAL: %1").arg(d->lastError));
    }
    if (!d->execRaw(QStringLiteral("PRAGMA foreign_keys = ON"))) {
        return fail(QStringLiteral("cannot enable foreign keys: %1").arg(d->lastError));
    }
    if (!d->execRaw(QStringLiteral("PRAGMA synchronous = NORMAL"))) {
        return fail(QStringLiteral("cannot set synchronous: %1").arg(d->lastError));
    }
    // 事务期间不自动降级；busy_timeout 避免短时锁冲突直接报错
    if (!d->execRaw(QStringLiteral("PRAGMA busy_timeout = 5000"))) {
        return fail(QStringLiteral("cannot set busy_timeout: %1").arg(d->lastError));
    }
    // WAL 调优：限制日志体积、控制自动 checkpoint，避免长期运行后 WAL 无界增长
    if (!d->execRaw(QStringLiteral("PRAGMA journal_size_limit = 8388608"))) {
        return fail(QStringLiteral("cannot set journal_size_limit: %1").arg(d->lastError));
    }
    if (!d->execRaw(QStringLiteral("PRAGMA wal_autocheckpoint = 1000"))) {
        return fail(QStringLiteral("cannot set wal_autocheckpoint: %1").arg(d->lastError));
    }

    // QSQLITE 的 exec() 一次只能执行一条语句，因此逐条执行建表语句。
    for (const QString &statement : schemaStatements()) {
        if (!d->execRaw(statement)) {
            return fail(QStringLiteral("schema creation failed: %1").arg(d->lastError));
        }
    }

    // ---- 结构版本与迁移 ----
    // 顺序：读已有版本（空库 = 0）→ 必要时迁移 → 记录当前版本。
    int storedVersion = 0;
    {
        QSqlQuery query(d->db);
        if (query.exec(QStringLiteral("SELECT COALESCE(MAX(version), 0) FROM schema_version")) &&
            query.next()) {
            storedVersion = query.value(0).toInt();
        }
    }

    // 未来版本无法向后兼容
    if (storedVersion > kSchemaVersion) {
        return fail(QStringLiteral("database schema version %1 is newer than supported %2")
                        .arg(storedVersion)
                        .arg(kSchemaVersion));
    }

    if (storedVersion < kSchemaVersion) {
        qCInfo(lcRepo) << "migrating database schema from" << storedVersion << "to"
                       << kSchemaVersion;
        if (!d->migrateSchema(storedVersion)) {
            return fail(QStringLiteral("schema migration failed: %1").arg(d->lastError));
        }
    }

    {
        QSqlQuery query(d->db);
        query.prepare(QStringLiteral(
            "INSERT OR IGNORE INTO schema_version(version, applied_at) VALUES(:v, :t)"));
        query.bindValue(QStringLiteral(":v"), kSchemaVersion);
        query.bindValue(QStringLiteral(":t"), timeutil::toStorage(timeutil::nowUtc()));
        if (!query.exec()) {
            return fail(QStringLiteral("cannot record schema version: %1")
                            .arg(query.lastError().text()));
        }
    }

    qCInfo(lcRepo) << "database opened, schema version" << kSchemaVersion;
    return true;
}

void SqliteRepository::close()
{
    if (d->db.isOpen()) {
        if (d->transactionDepth > 0) {
            d->db.rollback();
            d->transactionDepth = 0;
        }
        d->db.close();
    }
    if (!d->connectionName.isEmpty()) {
        d->db = QSqlDatabase();
        QSqlDatabase::removeDatabase(d->connectionName);
        d->connectionName.clear();
    }
}

bool SqliteRepository::isOpen() const
{
    return d->db.isOpen();
}

QString SqliteRepository::lastError() const
{
    return d->lastError;
}

// ---------------------------------------------------------------------------
// 事务
// ---------------------------------------------------------------------------

bool SqliteRepository::beginTransaction(QString *error)
{
    if (!d->db.isOpen()) {
        if (error != nullptr) {
            *error = QStringLiteral("database is not open");
        }
        return false;
    }
    if (d->transactionDepth > 0) {
        // 嵌套事务不受支持，明确拒绝而不是静默产生错误的提交语义
        if (error != nullptr) {
            *error = QStringLiteral("nested transaction is not supported");
        }
        return false;
    }
    if (!d->db.transaction()) {
        d->lastError = d->db.lastError().text();
        if (error != nullptr) {
            *error = d->lastError;
        }
        return false;
    }
    d->transactionDepth = 1;
    return true;
}

bool SqliteRepository::commit(QString *error)
{
    if (d->transactionDepth == 0) {
        if (error != nullptr) {
            *error = QStringLiteral("no active transaction");
        }
        return false;
    }
    if (!d->db.commit()) {
        d->lastError = d->db.lastError().text();
        if (error != nullptr) {
            *error = d->lastError;
        }
        return false;
    }
    d->transactionDepth = 0;
    return true;
}

bool SqliteRepository::rollback(QString *error)
{
    if (d->transactionDepth == 0) {
        if (error != nullptr) {
            *error = QStringLiteral("no active transaction");
        }
        return false;
    }
    const bool ok = d->db.rollback();
    d->transactionDepth = 0;
    if (!ok) {
        d->lastError = d->db.lastError().text();
        if (error != nullptr) {
            *error = d->lastError;
        }
    }
    return ok;
}

bool SqliteRepository::inTransaction() const
{
    return d->transactionDepth > 0;
}

// ---------------------------------------------------------------------------
// 内部：自动事务守卫（保证任何提前返回都会回滚）
// ---------------------------------------------------------------------------

namespace {

class TransactionGuard
{
public:
    explicit TransactionGuard(SqliteRepository &repo) : m_repo(repo)
    {
        m_owner = !repo.inTransaction();
        if (m_owner) {
            m_active = repo.beginTransaction();
        } else {
            m_active = true;  // 复用外层事务，提交交给外层
        }
    }

    ~TransactionGuard()
    {
        if (m_owner && m_active && !m_committed) {
            m_repo.rollback();
        }
    }

    bool active() const { return m_active; }

    bool commit(QString *error)
    {
        if (!m_active) {
            return false;
        }
        if (!m_owner) {
            m_committed = true;  // 外层负责提交
            return true;
        }
        m_committed = m_repo.commit(error);
        return m_committed;
    }

private:
    SqliteRepository &m_repo;
    bool m_owner = false;
    bool m_active = false;
    bool m_committed = false;
};

} // namespace

// ---------------------------------------------------------------------------
// 课程
// ---------------------------------------------------------------------------

bool SqliteRepository::applyCourseSnapshot(const QString &source, const QList<Course> &courses,
                                           QString *error)
{
    TransactionGuard tx(*this);
    if (!tx.active()) {
        if (error != nullptr) {
            *error = QStringLiteral("cannot begin transaction");
        }
        return false;
    }

    if (source.isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral("course snapshot requires a non-empty source");
        }
        return false;   // 没有来源就无法安全地做"范围内的软删除"
    }

    QSqlQuery upsert(d->db);
    upsert.prepare(QStringLiteral(
        "INSERT INTO courses (course_id, course_name, detail_url, course_code, department,"
        "                     instructors, source, semester_code, year_code, present, last_seen_at)"
        " VALUES (:id, :name, :url, :code, :dept, :teachers, :source, :sem, :year, 1, :seen)"
        " ON CONFLICT(course_id) DO UPDATE SET"
        "   course_name   = excluded.course_name,"
        "   detail_url    = excluded.detail_url,"
        "   course_code   = excluded.course_code,"
        "   department    = excluded.department,"
        "   instructors   = excluded.instructors,"
        "   source        = excluded.source,"
        "   semester_code = excluded.semester_code,"
        "   year_code     = excluded.year_code,"
        "   present       = 1,"
        "   last_seen_at  = excluded.last_seen_at"));

    for (const Course &c : courses) {
        if (c.courseId.isEmpty()) {
            continue;
        }
        // 整批课程必须属于同一个来源：写入行的 source 与软删除范围都由参数决定。
        // 不一致说明调用方传错了来源——静默写下去会让另一来源的课程被错删，直接失败。
        if (!c.source.isEmpty() && c.source != source) {
            if (error != nullptr) {
                *error = QStringLiteral("course '%1' has source '%2' but snapshot source is '%3'")
                             .arg(c.courseId, c.source, source);
            }
            return false;
        }
        upsert.bindValue(QStringLiteral(":id"), c.courseId);
        upsert.bindValue(QStringLiteral(":name"), c.courseName);
        upsert.bindValue(QStringLiteral(":url"), c.detailUrl);
        upsert.bindValue(QStringLiteral(":code"), c.courseCode);
        upsert.bindValue(QStringLiteral(":dept"),
                         c.department.has_value() ? QVariant(*c.department) : QVariant());
        upsert.bindValue(QStringLiteral(":teachers"), joinList(c.instructors));
        upsert.bindValue(QStringLiteral(":source"), source);
        upsert.bindValue(QStringLiteral(":sem"),
                         c.semesterCode.has_value() ? QVariant(*c.semesterCode) : QVariant());
        upsert.bindValue(QStringLiteral(":year"),
                         c.academicYearCode.has_value() ? QVariant(*c.academicYearCode) : QVariant());
        upsert.bindValue(QStringLiteral(":seen"), timeutil::toStorage(c.lastSeenAt));
        if (!upsert.exec()) {
            d->lastError = upsert.lastError().text();
            if (error != nullptr) {
                *error = d->lastError;
            }
            return false;  // 守卫析构时回滚
        }
    }

    // 本次快照未出现的课程标记为 present=0（软删除，保留历史作业）。
    // 需求场景 G：不能永久依赖首次采集得到的课程列表。
    // ⚠️ 只在**同一来源**内软删除：写学习通的快照不能影响智汇南雍的课程。
    {
        QSqlQuery mark(d->db);
        if (courses.isEmpty()) {
            mark.prepare(QStringLiteral("UPDATE courses SET present = 0"
                                        " WHERE present = 1 AND source = :source"));
            mark.bindValue(QStringLiteral(":source"), source);
        } else {
            QStringList placeholders;
            placeholders.reserve(courses.size());
            for (int i = 0; i < courses.size(); ++i) {
                placeholders << QStringLiteral(":id%1").arg(i);
            }
            mark.prepare(QStringLiteral("UPDATE courses SET present = 0 WHERE present = 1"
                                        " AND source = :source AND course_id NOT IN (%1)")
                             .arg(placeholders.join(QLatin1Char(','))));
            mark.bindValue(QStringLiteral(":source"), source);
            for (int i = 0; i < courses.size(); ++i) {
                mark.bindValue(QStringLiteral(":id%1").arg(i), courses.at(i).courseId);
            }
        }
        if (!mark.exec()) {
            d->lastError = mark.lastError().text();
            if (error != nullptr) {
                *error = d->lastError;
            }
            return false;
        }
    }

    return tx.commit(error);
}

QList<Course> SqliteRepository::listCourses(bool includeMissing) const
{
    QList<Course> out;
    QSqlQuery query(d->db);
    const QString sql = includeMissing
                            ? QStringLiteral("SELECT course_id, course_name, detail_url, course_code,"
                                             " department, instructors, source, semester_code,"
                                             " year_code, present, last_seen_at FROM courses"
                                             " ORDER BY course_name")
                            : QStringLiteral("SELECT course_id, course_name, detail_url, course_code,"
                                             " department, instructors, source, semester_code,"
                                             " year_code, present, last_seen_at FROM courses"
                                             " WHERE present = 1 ORDER BY course_name");
    if (!query.exec(sql)) {
        d->lastError = query.lastError().text();
        return out;
    }

    while (query.next()) {
        Course c;
        c.courseId = query.value(0).toString();
        c.courseName = query.value(1).toString();
        c.detailUrl = query.value(2).toString();
        c.courseCode = query.value(3).toString();
        if (!query.value(4).isNull()) {
            c.department = query.value(4).toString();
        }
        c.instructors = splitList(query.value(5).toString());
        c.source = query.value(6).toString();
        if (!query.value(7).isNull()) {
            c.semesterCode = query.value(7).toString();
        }
        if (!query.value(8).isNull()) {
            c.academicYearCode = query.value(8).toString();
        }
        c.present = query.value(9).toInt() != 0;
        if (const auto seen = timeutil::parseSiteTime(query.value(10).toString());
            seen.has_value()) {
            c.lastSeenAt = *seen;
        }
        out.append(c);
    }
    return out;
}

// ---------------------------------------------------------------------------
// 作业：单次课程快照写入（断点续传的原子单元）
// ---------------------------------------------------------------------------

BatchApplyResult SqliteRepository::applyAssignmentSnapshot(const QString &courseId,
                                                           const QList<Assignment> &assignments,
                                                           qint64 runId, int batchNo,
                                                           const QString &cursorBefore,
                                                           const QString &cursorAfter,
                                                           bool snapshotComplete,
                                                           const QString &mode)
{
    BatchApplyResult result;
    result.partialSnapshot = !snapshotComplete;

    if (courseId.isEmpty()) {
        result.error = QStringLiteral("courseId is empty");
        return result;
    }

    TransactionGuard tx(*this);
    if (!tx.active()) {
        result.error = QStringLiteral("cannot begin transaction");
        return result;
    }

    const auto fail = [&](const QString &message) {
        result.ok = false;
        result.error = message;
        d->lastError = message;
        return result;  // 守卫析构时回滚：数据与断点一起回退
    };

    QSqlQuery insert(d->db);
    insert.prepare(QStringLiteral(
        "INSERT INTO assignments (course_id, source_id, activity_id, title, description,"
        " detail_url, published_at, due_at, status, submitted, score, content_hash,"
        " content_signature, observed_at, first_seen_at, last_seen_at, present)"
        " VALUES (:c, :s, :aid, :title, :desc, :url, :pub, :due, :status, :submitted, :score,"
        " :hash, :sig, :observed, :first, :last, 1)"));

    QSqlQuery update(d->db);
    update.prepare(QStringLiteral(
        "UPDATE assignments SET activity_id=:aid, title=:title, description=:desc,"
        " detail_url=:url, published_at=:pub, due_at=:due, status=:status,"
        " submitted=:submitted, score=:score, content_hash=:hash, content_signature=:sig,"
        " observed_at=:observed, last_seen_at=:last, present=1"
        " WHERE course_id=:c AND source_id=:s"));

    QSqlQuery touch(d->db);
    touch.prepare(QStringLiteral(
        "UPDATE assignments SET last_seen_at=:last, present=1"
        " WHERE course_id=:c AND source_id=:s"));

    const QString nowStorage = timeutil::toStorage(timeutil::nowUtc());

    // v2 性能：整门课的既有记录一次读入内存，避免"每条作业一次 SELECT"。
    // 课程快照通常只有几十条，内存开销可忽略；数据库往返从 O(行数) 降到 O(1)。
    const QHash<QString, Assignment> existingMap = d->loadExistingMap(courseId);

    for (const Assignment &incomingRaw : assignments) {
        Assignment incoming = incomingRaw;
        if (incoming.sourceId.isEmpty()) {
            continue;
        }

        const auto existingIt = existingMap.constFind(incoming.sourceId);
        const std::optional<Assignment> existing =
            existingIt == existingMap.constEnd() ? std::nullopt
                                                 : std::optional<Assignment>(*existingIt);

        // ---- 字段级合并：不可信的新值绝不覆盖旧值（需求场景 F）----
        if (existing.has_value()) {
            if (mergeObserved(incoming.title, existing->title)) result.preservedFields << QStringLiteral("title");
            if (mergeObserved(incoming.description, existing->description)) result.preservedFields << QStringLiteral("description");
            if (mergeObserved(incoming.detailUrl, existing->detailUrl)) result.preservedFields << QStringLiteral("detailUrl");
            if (mergeObserved(incoming.publishedAt, existing->publishedAt)) result.preservedFields << QStringLiteral("publishedAt");
            if (mergeObserved(incoming.dueAt, existing->dueAt)) result.preservedFields << QStringLiteral("dueAt");
            if (mergeObserved(incoming.status, existing->status)) result.preservedFields << QStringLiteral("status");
            if (mergeObserved(incoming.submitted, existing->submitted)) result.preservedFields << QStringLiteral("submitted");
            if (mergeObserved(incoming.score, existing->score)) result.preservedFields << QStringLiteral("score");
        }

        // 摘要只由权威字段构成；合并之后再算，保证与最终写入的值一致
        incoming.contentSignature = AssignmentParser::computeContentSignature(incoming);
        incoming.contentHash = QString::fromLatin1(
            QCryptographicHash::hash(incoming.contentSignature.toUtf8(),
                                     QCryptographicHash::Sha256).toHex());

        const QVariant title = incoming.title.hasValue() ? QVariant(incoming.title.require()) : QVariant();
        const QVariant desc = incoming.description.hasValue() ? QVariant(incoming.description.require()) : QVariant();
        const QVariant url = incoming.detailUrl.hasValue() ? QVariant(incoming.detailUrl.require()) : QVariant();
        const QVariant pub = incoming.publishedAt.hasValue()
                                 ? QVariant(timeutil::toStorage(incoming.publishedAt.require()))
                                 : QVariant();
        const QVariant due = incoming.dueAt.hasValue()
                                 ? QVariant(timeutil::toStorage(incoming.dueAt.require()))
                                 : QVariant();
        const QVariant status = incoming.status.hasValue()
                                    ? QVariant(QString::fromLatin1(assignmentStatusKey(incoming.status.require())))
                                    : QVariant();
        const QVariant submitted = incoming.submitted.hasValue()
                                       ? QVariant(incoming.submitted.require() ? 1 : 0)
                                       : QVariant();
        const QVariant score = incoming.score.hasValue() ? QVariant(incoming.score.require()) : QVariant();

        if (!existing.has_value()) {
            // ---- 新记录 ----
            insert.bindValue(QStringLiteral(":c"), courseId);
            insert.bindValue(QStringLiteral(":s"), incoming.sourceId);
            insert.bindValue(QStringLiteral(":aid"), incoming.activityId);
            insert.bindValue(QStringLiteral(":title"), title);
            insert.bindValue(QStringLiteral(":desc"), desc);
            insert.bindValue(QStringLiteral(":url"), url);
            insert.bindValue(QStringLiteral(":pub"), pub);
            insert.bindValue(QStringLiteral(":due"), due);
            insert.bindValue(QStringLiteral(":status"), status);
            insert.bindValue(QStringLiteral(":submitted"), submitted);
            insert.bindValue(QStringLiteral(":score"), score);
            insert.bindValue(QStringLiteral(":hash"), incoming.contentHash);
            insert.bindValue(QStringLiteral(":sig"), incoming.contentSignature);
            insert.bindValue(QStringLiteral(":observed"), timeutil::toStorage(incoming.observedAt));
            insert.bindValue(QStringLiteral(":first"), nowStorage);
            insert.bindValue(QStringLiteral(":last"), nowStorage);
            if (!insert.exec()) {
                return fail(insert.lastError().text());
            }
            ++result.inserted;
            continue;
        }

        // ---- 已存在且内容未变 ----
        if (existing->contentHash == incoming.contentHash) {
            // v2：内容没变的记录不再逐行 UPDATE —— 那是每次同步 O(作业数) 的写放大。
            // 课程级的 last_synced_at 会在事务末尾统一更新，语义不变。
            // 唯一例外：这条记录之前被软删除过（present = 0），现在又出现在快照里，
            // 此时必须把 present 恢复成 1，所以仍然写一次。
            if (existing->present) {
                ++result.unchanged;
                continue;
            }
            touch.bindValue(QStringLiteral(":last"), nowStorage);
            touch.bindValue(QStringLiteral(":c"), courseId);
            touch.bindValue(QStringLiteral(":s"), incoming.sourceId);
            if (!touch.exec()) {
                return fail(touch.lastError().text());
            }
            ++result.unchanged;
            continue;
        }

        qCDebug(lcRepo) << "content changed for" << incoming.sourceId
                        << "existing hash:" << existing->contentHash
                        << "incoming hash:" << incoming.contentHash;

        update.bindValue(QStringLiteral(":aid"), incoming.activityId);
        update.bindValue(QStringLiteral(":title"), title);
        update.bindValue(QStringLiteral(":desc"), desc);
        update.bindValue(QStringLiteral(":url"), url);
        update.bindValue(QStringLiteral(":pub"), pub);
        update.bindValue(QStringLiteral(":due"), due);
        update.bindValue(QStringLiteral(":status"), status);
        update.bindValue(QStringLiteral(":submitted"), submitted);
        update.bindValue(QStringLiteral(":score"), score);
        update.bindValue(QStringLiteral(":hash"), incoming.contentHash);
        update.bindValue(QStringLiteral(":sig"), incoming.contentSignature);
        // 内容确实变化时记录新的观察时间（需求场景 E）
        update.bindValue(QStringLiteral(":observed"), timeutil::toStorage(incoming.observedAt));
        update.bindValue(QStringLiteral(":last"), nowStorage);
        update.bindValue(QStringLiteral(":c"), courseId);
        update.bindValue(QStringLiteral(":s"), incoming.sourceId);
        if (!update.exec()) {
            return fail(update.lastError().text());
        }
        ++result.updated;
    }

    // ---- 缺失检测：本次快照中没有出现的旧记录标记 present=0 ----
    //
    // v2 修正：只有「能证明完整」的快照才允许做缺失检测。
    // 快照不完整时（服务端没给 total，或翻页期间列表变化导致条数对不上），
    // "本轮没出现"并不代表站点上真的没有了；直接标记 present = 0 会把仍然存在的
    // 作业从界面上抹掉（误软删除）。因此跳过缺失检测，等下一轮完整快照再对账。
    if (snapshotComplete) {
        QSqlQuery mark(d->db);
        if (assignments.isEmpty()) {
            mark.prepare(QStringLiteral(
                "UPDATE assignments SET present = 0 WHERE course_id = :c AND present = 1"));
            mark.bindValue(QStringLiteral(":c"), courseId);
        } else {
            QStringList placeholders;
            placeholders.reserve(assignments.size());
            for (int i = 0; i < assignments.size(); ++i) {
                placeholders << QStringLiteral(":s%1").arg(i);
            }
            mark.prepare(QStringLiteral(
                "UPDATE assignments SET present = 0"
                " WHERE course_id = :c AND present = 1 AND source_id NOT IN (%1)")
                .arg(placeholders.join(QLatin1Char(','))));
            mark.bindValue(QStringLiteral(":c"), courseId);
            for (int i = 0; i < assignments.size(); ++i) {
                mark.bindValue(QStringLiteral(":s%1").arg(i), assignments.at(i).sourceId);
            }
        }
        if (!mark.exec()) {
            return fail(mark.lastError().text());
        }
        result.markedMissing = mark.numRowsAffected() > 0 ? int(mark.numRowsAffected()) : 0;
    }

    // ---- 课程级同步元数据（v2）----
    // 一门课一条 UPDATE：记录最近同步时间与当前在场的作业数。
    // 它替代了「未变作业逐行 touch last_seen_at」，把单轮写入量从 O(作业总数)
    // 降到 O(真正变化的条数)+O(课程数)。
    {
        QSqlQuery course(d->db);
        course.prepare(QStringLiteral(
            "UPDATE courses SET last_synced_at = :at,"
            " assignment_count = (SELECT COUNT(*) FROM assignments"
            "                      WHERE course_id = :c2 AND present = 1)"
            " WHERE course_id = :c1"));
        course.bindValue(QStringLiteral(":at"), nowStorage);
        course.bindValue(QStringLiteral(":c1"), courseId);
        course.bindValue(QStringLiteral(":c2"), courseId);
        if (!course.exec()) {
            return fail(course.lastError().text());
        }
    }

    // ---- 批次记录与断点更新必须与数据写入同事务（需求第四节场景 A/B）----
    if (runId > 0 && batchNo > 0) {
        QSqlQuery batch(d->db);
        batch.prepare(QStringLiteral(
            "INSERT INTO crawl_batches (run_id, batch_no, cursor_before, cursor_after, status,"
            " committed_at) VALUES (:run, :no, :before, :after, 'committed', :at)"
            " ON CONFLICT(run_id, batch_no) DO UPDATE SET"
            "   cursor_before=excluded.cursor_before, cursor_after=excluded.cursor_after,"
            "   status='committed', committed_at=excluded.committed_at"));
        batch.bindValue(QStringLiteral(":run"), runId);
        batch.bindValue(QStringLiteral(":no"), batchNo);
        batch.bindValue(QStringLiteral(":before"), cursorBefore);
        batch.bindValue(QStringLiteral(":after"), cursorAfter);
        batch.bindValue(QStringLiteral(":at"), nowStorage);
        if (!batch.exec()) {
            return fail(batch.lastError().text());
        }

        SyncState state;
        state.taskKey = courseTaskKey(courseId);
        state.mode = mode;
        state.cursor = cursorAfter;
        state.lastSuccessAt = timeutil::nowUtc();
        // 不确定是否完整 → 不标记完成（需求第四节：不能把不完整的批次当成功）
        state.fullScanCompleted = snapshotComplete;
        state.updatedAt = timeutil::nowUtc();

        QSqlQuery sync(d->db);
        sync.prepare(QStringLiteral(
            "INSERT INTO sync_state (task_key, mode, cursor, last_success_at,"
            " full_scan_completed, updated_at)"
            " VALUES (:k, :m, :cur, :ok, :full, :up)"
            " ON CONFLICT(task_key) DO UPDATE SET"
            "   mode=excluded.mode, cursor=excluded.cursor,"
            "   last_success_at=excluded.last_success_at,"
            "   full_scan_completed=excluded.full_scan_completed, updated_at=excluded.updated_at"));
        sync.bindValue(QStringLiteral(":k"), state.taskKey);
        sync.bindValue(QStringLiteral(":m"), state.mode);
        sync.bindValue(QStringLiteral(":cur"), state.cursor);
        sync.bindValue(QStringLiteral(":ok"), timeutil::toStorage(*state.lastSuccessAt));
        sync.bindValue(QStringLiteral(":full"), state.fullScanCompleted ? 1 : 0);
        sync.bindValue(QStringLiteral(":up"), timeutil::toStorage(state.updatedAt));
        if (!sync.exec()) {
            return fail(sync.lastError().text());
        }
    }

    if (!tx.commit(&result.error)) {
        result.ok = false;
        return result;
    }
    result.ok = true;
    return result;
}

// ---------------------------------------------------------------------------
// 作业查询
// ---------------------------------------------------------------------------

namespace {

Assignment assignmentFromQuery(const QSqlQuery &query)
{
    Assignment a;
    a.courseId = query.value(0).toString();
    a.sourceId = query.value(1).toString();
    a.activityId = query.value(2).toString();
    a.title = observedString(query.value(3));
    a.description = observedString(query.value(4));
    a.detailUrl = observedString(query.value(5));
    a.publishedAt = observedTime(query.value(6));
    a.dueAt = observedTime(query.value(7));
    {
        const QVariant v = query.value(8);
        if (v.isNull()) {
            a.status = Observed<AssignmentStatus>::absent();
        } else if (const auto parsed = assignmentStatusFromKey(v.toString()); parsed.has_value()) {
            a.status = Observed<AssignmentStatus>::present(*parsed);
        } else {
            a.status = Observed<AssignmentStatus>::invalid();
        }
    }
    a.submitted = observedBool(query.value(9));
    a.score = observedNumber(query.value(10));
    a.contentHash = query.value(11).toString();
    a.contentSignature = query.value(12).toString();
    if (const auto o = timeutil::parseSiteTime(query.value(13).toString()); o.has_value()) {
        a.observedAt = *o;
    }
    if (const auto f = timeutil::parseSiteTime(query.value(14).toString()); f.has_value()) {
        a.firstSeenAt = *f;
    }
    if (const auto l = timeutil::parseSiteTime(query.value(15).toString()); l.has_value()) {
        a.lastSeenAt = *l;
    }
    a.present = query.value(16).isNull() ? true : (query.value(16).toInt() != 0);
    return a;
}

constexpr auto kAssignmentColumns =
    "course_id, source_id, activity_id, title, description, detail_url, published_at, due_at,"
    " status, submitted, score, content_hash, content_signature, observed_at, first_seen_at,"
    " last_seen_at, present";

} // namespace

QHash<QString, Assignment> SqliteRepository::Impl::loadExistingMap(const QString &courseId) const
{
    QHash<QString, Assignment> out;

    QSqlQuery query(db);
    // 列顺序与 kAssignmentColumns 完全一致，可直接复用 assignmentFromQuery()
    query.prepare(QStringLiteral("SELECT %1 FROM assignments WHERE course_id = :c")
                      .arg(QString::fromLatin1(kAssignmentColumns)));
    query.bindValue(QStringLiteral(":c"), courseId);
    if (!query.exec()) {
        lastError = query.lastError().text();
        return out;
    }

    while (query.next()) {
        const Assignment a = assignmentFromQuery(query);
        out.insert(a.sourceId, a);
    }
    return out;
}

QList<Assignment> SqliteRepository::listAssignments(const QString &courseId,
                                                    bool includeMissing) const
{
    QList<Assignment> out;
    QSqlQuery query(d->db);
    const QString sql =
        QStringLiteral("SELECT %1 FROM assignments WHERE course_id = :c%2"
                       " ORDER BY (due_at IS NULL), due_at, title")
            .arg(QString::fromLatin1(kAssignmentColumns),
                 includeMissing ? QString() : QStringLiteral(" AND present = 1"));
    query.prepare(sql);
    query.bindValue(QStringLiteral(":c"), courseId);
    if (!query.exec()) {
        d->lastError = query.lastError().text();
        return out;
    }
    while (query.next()) {
        out.append(assignmentFromQuery(query));
    }
    return out;
}

std::optional<Assignment> SqliteRepository::findAssignment(const QString &courseId,
                                                           const QString &sourceId) const
{
    QSqlQuery query(d->db);
    query.prepare(QStringLiteral("SELECT %1 FROM assignments WHERE course_id = :c AND source_id = :s")
                      .arg(QString::fromLatin1(kAssignmentColumns)));
    query.bindValue(QStringLiteral(":c"), courseId);
    query.bindValue(QStringLiteral(":s"), sourceId);
    if (!query.exec() || !query.next()) {
        return std::nullopt;
    }
    return assignmentFromQuery(query);
}

int SqliteRepository::countAssignments(const QString &courseId) const
{
    QSqlQuery query(d->db);
    query.prepare(QStringLiteral(
        "SELECT COUNT(*) FROM assignments WHERE course_id = :c AND present = 1"));
    query.bindValue(QStringLiteral(":c"), courseId);
    if (!query.exec() || !query.next()) {
        return 0;
    }
    return query.value(0).toInt();
}

// ---------------------------------------------------------------------------
// 解析异常
// ---------------------------------------------------------------------------

bool SqliteRepository::recordParseErrors(const QList<ParseErrorRecord> &records, QString *error)
{
    if (records.isEmpty()) {
        return true;
    }
    TransactionGuard tx(*this);
    if (!tx.active()) {
        if (error != nullptr) {
            *error = QStringLiteral("cannot begin transaction");
        }
        return false;
    }

    QSqlQuery insert(d->db);
    insert.prepare(QStringLiteral(
        "INSERT INTO parse_errors (run_id, course_id, source_id, field, reason, observed_at)"
        " VALUES (:run, :c, :s, :f, :r, :at)"));
    for (const ParseErrorRecord &rec : records) {
        insert.bindValue(QStringLiteral(":run"), rec.runId);
        insert.bindValue(QStringLiteral(":c"), rec.courseId);
        insert.bindValue(QStringLiteral(":s"), rec.sourceId);
        insert.bindValue(QStringLiteral(":f"), rec.field);
        insert.bindValue(QStringLiteral(":r"), rec.reason);
        insert.bindValue(QStringLiteral(":at"), timeutil::toStorage(rec.observedAt));
        if (!insert.exec()) {
            d->lastError = insert.lastError().text();
            if (error != nullptr) {
                *error = d->lastError;
            }
            return false;
        }
    }
    return tx.commit(error);
}

QList<ParseErrorRecord> SqliteRepository::listRecentParseErrors(int limit) const
{
    QList<ParseErrorRecord> out;
    QSqlQuery query(d->db);
    query.prepare(QStringLiteral(
        "SELECT run_id, course_id, source_id, field, reason, observed_at FROM parse_errors"
        " ORDER BY id DESC LIMIT :n"));
    query.bindValue(QStringLiteral(":n"), limit);
    if (!query.exec()) {
        d->lastError = query.lastError().text();
        return out;
    }
    while (query.next()) {
        ParseErrorRecord r;
        r.runId = query.value(0).toLongLong();
        r.courseId = query.value(1).toString();
        r.sourceId = query.value(2).toString();
        r.field = query.value(3).toString();
        r.reason = query.value(4).toString();
        if (const auto at = timeutil::parseSiteTime(query.value(5).toString()); at.has_value()) {
            r.observedAt = *at;
        }
        out.append(r);
    }
    return out;
}

// ---------------------------------------------------------------------------
// 运行记录
// ---------------------------------------------------------------------------

qint64 SqliteRepository::startRun(const QString &taskKey, const QString &mode, QString *error)
{
    QSqlQuery query(d->db);
    query.prepare(QStringLiteral(
        "INSERT INTO crawl_runs (task_key, mode, status, started_at)"
        " VALUES (:k, :m, 'running', :at)"));
    query.bindValue(QStringLiteral(":k"), taskKey);
    query.bindValue(QStringLiteral(":m"), mode);
    query.bindValue(QStringLiteral(":at"), timeutil::toStorage(timeutil::nowUtc()));
    if (!query.exec()) {
        d->lastError = query.lastError().text();
        if (error != nullptr) {
            *error = d->lastError;
        }
        return 0;
    }
    return query.lastInsertId().toLongLong();
}

bool SqliteRepository::finishRun(qint64 runId, CrawlRun::Status status,
                                 const QString &errorMessage, QString *error)
{
    QSqlQuery query(d->db);
    query.prepare(QStringLiteral(
        "UPDATE crawl_runs SET status = :s, finished_at = :at, error_message = :err"
        " WHERE run_id = :id"));
    query.bindValue(QStringLiteral(":s"), QString::fromLatin1(CrawlRun::statusKey(status)));
    query.bindValue(QStringLiteral(":at"), timeutil::toStorage(timeutil::nowUtc()));
    query.bindValue(QStringLiteral(":err"), errorMessage);
    query.bindValue(QStringLiteral(":id"), runId);
    if (!query.exec()) {
        d->lastError = query.lastError().text();
        if (error != nullptr) {
            *error = d->lastError;
        }
        return false;
    }
    return true;
}

std::optional<CrawlRun> SqliteRepository::findRun(qint64 runId) const
{
    QSqlQuery query(d->db);
    query.prepare(QStringLiteral(
        "SELECT run_id, task_key, mode, status, started_at, finished_at, error_message"
        " FROM crawl_runs WHERE run_id = :id"));
    query.bindValue(QStringLiteral(":id"), runId);
    if (!query.exec() || !query.next()) {
        return std::nullopt;
    }
    CrawlRun run;
    run.runId = query.value(0).toLongLong();
    run.taskKey = query.value(1).toString();
    run.mode = query.value(2).toString();
    run.status = CrawlRun::statusFromKey(query.value(3).toString());
    if (const auto s = timeutil::parseSiteTime(query.value(4).toString()); s.has_value()) {
        run.startedAt = *s;
    }
    if (const auto f = timeutil::parseSiteTime(query.value(5).toString()); f.has_value()) {
        run.finishedAt = *f;
    }
    run.errorMessage = query.value(6).toString();
    return run;
}

QList<CrawlRun> SqliteRepository::listRecentRuns(int limit) const
{
    QList<CrawlRun> out;
    QSqlQuery query(d->db);
    query.prepare(QStringLiteral(
        "SELECT run_id, task_key, mode, status, started_at, finished_at, error_message"
        " FROM crawl_runs ORDER BY run_id DESC LIMIT :n"));
    query.bindValue(QStringLiteral(":n"), limit);
    if (!query.exec()) {
        d->lastError = query.lastError().text();
        return out;
    }
    while (query.next()) {
        CrawlRun run;
        run.runId = query.value(0).toLongLong();
        run.taskKey = query.value(1).toString();
        run.mode = query.value(2).toString();
        run.status = CrawlRun::statusFromKey(query.value(3).toString());
        if (const auto s = timeutil::parseSiteTime(query.value(4).toString()); s.has_value()) {
            run.startedAt = *s;
        }
        if (const auto f = timeutil::parseSiteTime(query.value(5).toString()); f.has_value()) {
            run.finishedAt = *f;
        }
        run.errorMessage = query.value(6).toString();
        out.append(run);
    }
    return out;
}

int SqliteRepository::reapOrphanedRuns(QString *error)
{
    QSqlQuery query(d->db);
    query.prepare(QStringLiteral(
        "UPDATE crawl_runs SET status = 'failed', finished_at = :at,"
        " error_message = :msg WHERE status = 'running'"));
    query.bindValue(QStringLiteral(":at"), timeutil::toStorage(timeutil::nowUtc()));
    query.bindValue(QStringLiteral(":msg"),
                    QStringLiteral("interrupted: process exited while run was in progress"));
    if (!query.exec()) {
        d->lastError = query.lastError().text();
        if (error != nullptr) {
            *error = d->lastError;
        }
        return -1;
    }
    const int affected = int(query.numRowsAffected());
    if (affected > 0) {
        qCInfo(lcRepo) << "reaped" << affected << "orphaned running run(s) from a previous session";
    }
    return affected;
}

int SqliteRepository::pruneCrawlHistory(int keepRuns, int keepDays, QString *error)
{
    TransactionGuard tx(*this);
    if (!tx.active()) {
        if (error != nullptr) {
            *error = QStringLiteral("cannot begin transaction");
        }
        return -1;
    }

    const int keep = qMax(1, keepRuns);
    const int days = qMax(0, keepDays);
    const QString cutoff = timeutil::toStorage(timeutil::nowUtc().addDays(-days));
    int removed = 0;

    const auto runDelete = [&](QSqlQuery &query) {
        if (!query.exec()) {
            if (error != nullptr) {
                *error = query.lastError().text();
            }
            return false;
        }
        removed += qMax(0, int(query.numRowsAffected()));
        return true;
    };

    // 顺序：批次（引用运行）→ 运行记录 → 字段级解析异常。
    // 两个下限同时满足：既保留最近 keep 次运行，也保留最近 days 天的记录。
    {
        QSqlQuery query(d->db);
        query.prepare(QStringLiteral(
            "DELETE FROM crawl_batches WHERE run_id IN ("
            "  SELECT run_id FROM crawl_runs"
            "   WHERE started_at < :cutoff"
            "      OR run_id NOT IN (SELECT run_id FROM crawl_runs"
            "                         ORDER BY run_id DESC LIMIT :keep))"));
        query.bindValue(QStringLiteral(":cutoff"), cutoff);
        query.bindValue(QStringLiteral(":keep"), keep);
        if (!runDelete(query)) {
            return -1;
        }
    }
    {
        QSqlQuery query(d->db);
        query.prepare(QStringLiteral(
            "DELETE FROM crawl_runs WHERE started_at < :cutoff"
            "   OR run_id NOT IN (SELECT run_id FROM crawl_runs"
            "                      ORDER BY run_id DESC LIMIT :keep)"));
        query.bindValue(QStringLiteral(":cutoff"), cutoff);
        query.bindValue(QStringLiteral(":keep"), keep);
        if (!runDelete(query)) {
            return -1;
        }
    }
    {
        QSqlQuery query(d->db);
        query.prepare(QStringLiteral("DELETE FROM parse_errors WHERE observed_at < :cutoff"));
        query.bindValue(QStringLiteral(":cutoff"), cutoff);
        if (!runDelete(query)) {
            return -1;
        }
    }

    if (removed > 0) {
        qCInfo(lcRepo) << "pruned" << removed << "old crawl history row(s)";
    }

    if (!tx.commit(error)) {
        return -1;
    }
    return removed;
}

int SqliteRepository::clearAllData(QString *error)
{
    TransactionGuard tx(*this);
    if (!tx.active()) {
        if (error != nullptr) {
            *error = QStringLiteral("cannot begin transaction");
        }
        return -1;
    }

    // 顺序无所谓（都在同一个事务里），但先删"子表"更符合直觉。
    // 保留 schema_version：清空的是数据，不是结构。
    const QStringList tables = {
        QStringLiteral("assignments"),  QStringLiteral("courses"),
        QStringLiteral("crawl_batches"), QStringLiteral("crawl_runs"),
        QStringLiteral("parse_errors"), QStringLiteral("sync_state"),
    };

    int removed = 0;
    for (const QString &table : tables) {
        QSqlQuery query(d->db);
        if (!query.exec(QStringLiteral("DELETE FROM %1").arg(table))) {
            if (error != nullptr) {
                *error = query.lastError().text();
            }
            return -1;   // 守卫析构时整体回滚：要么全删，要么不动
        }
        removed += qMax(0, int(query.numRowsAffected()));
    }

    if (!tx.commit(error)) {
        return -1;
    }

    qCInfo(lcRepo) << "cleared all collected data; rows removed:" << removed;

    // 尽力回收空间；失败不影响"已清空"这个结果
    if (!d->execRaw(QStringLiteral("VACUUM"))) {
        qCWarning(lcRepo) << "VACUUM after clearAllData failed:" << d->lastError;
    }
    return removed;
}

// ---------------------------------------------------------------------------
// 批次
// ---------------------------------------------------------------------------

std::optional<CrawlBatch> SqliteRepository::findLastCommittedBatch(qint64 runId) const
{
    QSqlQuery query(d->db);
    query.prepare(QStringLiteral(
        "SELECT batch_id, run_id, batch_no, cursor_before, cursor_after, status, committed_at"
        " FROM crawl_batches WHERE run_id = :r AND status = 'committed'"
        " ORDER BY batch_no DESC LIMIT 1"));
    query.bindValue(QStringLiteral(":r"), runId);
    if (!query.exec() || !query.next()) {
        return std::nullopt;
    }
    CrawlBatch b;
    b.batchId = query.value(0).toLongLong();
    b.runId = query.value(1).toLongLong();
    b.batchNo = query.value(2).toInt();
    b.cursorBefore = query.value(3).toString();
    b.cursorAfter = query.value(4).toString();
    b.status = CrawlBatch::Status::Committed;
    if (const auto at = timeutil::parseSiteTime(query.value(6).toString()); at.has_value()) {
        b.committedAt = *at;
    }
    return b;
}

QList<CrawlBatch> SqliteRepository::listBatches(qint64 runId) const
{
    QList<CrawlBatch> out;
    QSqlQuery query(d->db);
    query.prepare(QStringLiteral(
        "SELECT batch_id, run_id, batch_no, cursor_before, cursor_after, status, committed_at"
        " FROM crawl_batches WHERE run_id = :r ORDER BY batch_no"));
    query.bindValue(QStringLiteral(":r"), runId);
    if (!query.exec()) {
        d->lastError = query.lastError().text();
        return out;
    }
    while (query.next()) {
        CrawlBatch b;
        b.batchId = query.value(0).toLongLong();
        b.runId = query.value(1).toLongLong();
        b.batchNo = query.value(2).toInt();
        b.cursorBefore = query.value(3).toString();
        b.cursorAfter = query.value(4).toString();
        const QString status = query.value(5).toString();
        b.status = status == QLatin1String("committed") ? CrawlBatch::Status::Committed
                   : status == QLatin1String("failed") ? CrawlBatch::Status::Failed
                                                       : CrawlBatch::Status::Pending;
        if (const auto at = timeutil::parseSiteTime(query.value(6).toString()); at.has_value()) {
            b.committedAt = *at;
        }
        out.append(b);
    }
    return out;
}

// ---------------------------------------------------------------------------
// 同步断点
// ---------------------------------------------------------------------------

std::optional<SyncState> SqliteRepository::findSyncState(const QString &taskKey) const
{
    QSqlQuery query(d->db);
    query.prepare(QStringLiteral(
        "SELECT task_key, mode, cursor, last_success_at, full_scan_completed, updated_at"
        " FROM sync_state WHERE task_key = :k"));
    query.bindValue(QStringLiteral(":k"), taskKey);
    if (!query.exec() || !query.next()) {
        return std::nullopt;
    }
    SyncState s;
    s.taskKey = query.value(0).toString();
    s.mode = query.value(1).toString();
    s.cursor = query.value(2).toString();
    if (const auto at = timeutil::parseSiteTime(query.value(3).toString()); at.has_value()) {
        s.lastSuccessAt = *at;
    }
    s.fullScanCompleted = query.value(4).toInt() != 0;
    if (const auto at = timeutil::parseSiteTime(query.value(5).toString()); at.has_value()) {
        s.updatedAt = *at;
    }
    return s;
}

bool SqliteRepository::upsertSyncState(const SyncState &state, QString *error)
{
    QSqlQuery query(d->db);
    query.prepare(QStringLiteral(
        "INSERT INTO sync_state (task_key, mode, cursor, last_success_at,"
        " full_scan_completed, updated_at)"
        " VALUES (:k, :m, :cur, :ok, :full, :up)"
        " ON CONFLICT(task_key) DO UPDATE SET"
        "   mode=excluded.mode, cursor=excluded.cursor,"
        "   last_success_at=excluded.last_success_at,"
        "   full_scan_completed=excluded.full_scan_completed, updated_at=excluded.updated_at"));
    query.bindValue(QStringLiteral(":k"), state.taskKey);
    query.bindValue(QStringLiteral(":m"), state.mode);
    query.bindValue(QStringLiteral(":cur"), state.cursor);
    query.bindValue(QStringLiteral(":ok"), timeutil::toStorage(state.lastSuccessAt));
    query.bindValue(QStringLiteral(":full"), state.fullScanCompleted ? 1 : 0);
    query.bindValue(QStringLiteral(":up"), timeutil::toStorage(state.updatedAt));
    if (!query.exec()) {
        d->lastError = query.lastError().text();
        if (error != nullptr) {
            *error = d->lastError;
        }
        return false;
    }
    return true;
}

QList<SyncState> SqliteRepository::listSyncStates() const
{
    QList<SyncState> out;
    QSqlQuery query(d->db);
    if (!query.exec(QStringLiteral(
            "SELECT task_key, mode, cursor, last_success_at, full_scan_completed, updated_at"
            " FROM sync_state ORDER BY task_key"))) {
        d->lastError = query.lastError().text();
        return out;
    }
    while (query.next()) {
        SyncState s;
        s.taskKey = query.value(0).toString();
        s.mode = query.value(1).toString();
        s.cursor = query.value(2).toString();
        if (const auto at = timeutil::parseSiteTime(query.value(3).toString()); at.has_value()) {
            s.lastSuccessAt = *at;
        }
        s.fullScanCompleted = query.value(4).toInt() != 0;
        if (const auto at = timeutil::parseSiteTime(query.value(5).toString()); at.has_value()) {
            s.updatedAt = *at;
        }
        out.append(s);
    }
    return out;
}

QString SqliteRepository::courseTaskKey(const QString &courseId)
{
    return QStringLiteral("course:") + courseId;
}

QString SqliteRepository::courseDiscoveryTaskKey()
{
    return QStringLiteral("courses:discovery");
}

QString SqliteRepository::fullReconcileTaskKey()
{
    return QStringLiteral("sync:full-reconcile");
}

QString SqliteRepository::todoSnapshotTaskKey()
{
    return QStringLiteral("sync:todos");
}

} // namespace njulms
