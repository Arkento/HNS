#include "parser/AssignmentParser.h"
#include "repository/SqliteRepository.h"
#include "support/Fixtures.h"

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>

using namespace njulms;
using namespace njulms::test;

namespace {

/// 把样本 JSON 解析成作业列表，供各用例复用
QList<Assignment> assignmentsFrom(const QString &json, const QString &courseId,
                                  ParseIssues *issues = nullptr)
{
    const QJsonObject root = QJsonDocument::fromJson(json.toUtf8()).object();
    const QJsonArray items = root.value(QStringLiteral("homework_activities")).toArray();
    return AssignmentParser::parseAssignments(courseId, items, fixtures::observedBeforeDeadlines(), issues);
}

QList<Course> coursesFrom(const QString &json)
{
    const QJsonObject root = QJsonDocument::fromJson(json.toUtf8()).object();
    return AssignmentParser::parseCourseList(root, fixtures::observedBeforeDeadlines(), nullptr);
}

} // namespace

class RepositoryTests : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;

    QString dbPath() const { return QDir(m_dir.path()).filePath(QStringLiteral("test.db")); }

    /// 打开一个独立仓储实例（每个用例互不干扰）
    std::unique_ptr<SqliteRepository> openRepo(const QString &suffix = QString())
    {
        auto repo = std::make_unique<SqliteRepository>();
        const QString path = QDir(m_dir.path()).filePath(QStringLiteral("test%1.db").arg(suffix));
        QString error;
        if (!repo->open(path, &error)) {
            qWarning("open failed: %s", qPrintable(error));
        }
        return repo;
    }

private slots:

    void initTestCase()
    {
        QVERIFY2(m_dir.isValid(), "temporary directory must be usable");
    }

    // ---- 结构 ---------------------------------------------------------------

    void schema_isCreatedAndVersioned()
    {
        auto repo = openRepo(QStringLiteral("_schema"));
        QVERIFY(repo->isOpen());

        // 通过公共接口验证结构可用：课程与作业都能写入并读回。
        // 表名/列名的正确性由 open() 内的建表语句与后续 CRUD 共同保证。
        const QList<Course> courses = coursesFrom(fixtures::myCoursesTwoCourses());
        QString error;
        QVERIFY2(repo->applyCourseSnapshot(source_keys::kNjuLms.toString(), courses, &error),
                 qPrintable(error));
        QCOMPARE(repo->listCourses().size(), 2);

        const QString courseId = QStringLiteral("90001");
        const BatchApplyResult r = repo->applyAssignmentSnapshot(
            courseId, assignmentsFrom(fixtures::homeworkActivitiesThree(), courseId));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(repo->countAssignments(courseId), 3);
    }

    void schema_rejectsNewerVersion()
    {
        // 未来版本的数据库不得被静默打开（结构未知会导致数据损坏）
        const QString path = QDir(m_dir.path()).filePath(QStringLiteral("future.db"));
        {
            auto repo = std::make_unique<SqliteRepository>();
            QString error;
            QVERIFY(repo->open(path, &error));
            repo->close();
        }
        {
            // 手工把版本号改大
            const QString conn = QStringLiteral("bump_conn");
            {
                QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
                db.setDatabaseName(path);
                QVERIFY(db.open());
                QSqlQuery q(db);
                QVERIFY(q.exec(QStringLiteral("UPDATE schema_version SET version = 999")));
                db.close();
            }
            QSqlDatabase::removeDatabase(conn);

            auto repo = std::make_unique<SqliteRepository>();
            QString error;
            QVERIFY(!repo->open(path, &error));
            QVERIFY(error.contains(QStringLiteral("newer")));
        }
    }

    // ---- 课程：场景 G -------------------------------------------------------

    void courses_firstSnapshotInserts()
    {
        auto repo = openRepo(QStringLiteral("_courses1"));
        QVERIFY(repo->applyCourseSnapshot(source_keys::kNjuLms.toString(),
                                          coursesFrom(fixtures::myCoursesTwoCourses())));
        QCOMPARE(repo->listCourses().size(), 2);
        QCOMPARE(repo->listCourses(true).size(), 2);
    }

    void courses_removedCourseIsSoftDeletedNotErased()
    {
        auto repo = openRepo(QStringLiteral("_courses2"));
        QVERIFY(repo->applyCourseSnapshot(source_keys::kNjuLms.toString(),
                                          coursesFrom(fixtures::myCoursesTwoCourses())));

        // 第二次快照只剩一门课 → 另一门应被标记 present=0，但数据保留。
        //
        // 注意：这里直接构造 Course，而不是写一段多行的 JSON 原始字符串。
        // 原因见 docs/01-实现决策.md「moc 与原始字符串字面量」一节：
        // moc 会把字符串字面量里的换行替换成空格（因为它要在字面量内识别 #define），
        // 于是字面量之后的代码会错位；再叠加 moc 单 token 上限，最终 moc
        // 报 "No relevant classes found" 并生成 0 字节的 .moc，链接期才报未解析符号。
        Course only;
        only.courseId = QStringLiteral("90001");
        only.courseName = QStringLiteral("测试课程甲");
        only.detailUrl = QStringLiteral("https://lms.nju.edu.cn/course/90001/index");
        only.lastSeenAt = fixtures::observedBeforeDeadlines();

        QVERIFY(repo->applyCourseSnapshot(source_keys::kNjuLms.toString(), {only}));

        QCOMPARE(repo->listCourses(false).size(), 1);
        QCOMPARE(repo->listCourses(true).size(), 2);

        // 场景 G：课程列表变化时下一次同步必须重新发现，不能永久依赖首次列表
        QVERIFY(repo->applyCourseSnapshot(source_keys::kNjuLms.toString(),
                                          coursesFrom(fixtures::myCoursesTwoCourses())));
        QCOMPARE(repo->listCourses(false).size(), 2);
    }

    /// 多站点：一个来源的快照绝不能软删除另一个来源的课程
    void courses_snapshotsAreIsolatedPerSource()
    {
        auto repo = openRepo(QStringLiteral("_courses_src"));
        QVERIFY(repo->applyCourseSnapshot(source_keys::kNjuLms.toString(),
                                          coursesFrom(fixtures::myCoursesTwoCourses())));

        Course cx;
        cx.source = source_keys::kChaoxing.toString();
        cx.courseId = QStringLiteral("111111111-222222222");
        cx.courseName = QStringLiteral("学习通测试课程");
        cx.lastSeenAt = fixtures::observedBeforeDeadlines();
        QVERIFY(repo->applyCourseSnapshot(source_keys::kChaoxing.toString(), {cx}));
        QCOMPARE(repo->listCourses().size(), 3);

        // 快照参数与课程自带来源不一致 → 整体失败（防止把学习通课程写进南大的范围）
        QString mismatchError;
        QVERIFY(!repo->applyCourseSnapshot(source_keys::kNjuLms.toString(), {cx},
                                           &mismatchError));
        QVERIFY(!mismatchError.isEmpty());
        QCOMPARE(repo->listCourses(true).size(), 3);   // 事务回滚，没有半成品数据

        // 学习通第二次快照为空（例如课程全部结课消失）→ 只影响学习通自己
        QVERIFY(repo->applyCourseSnapshot(source_keys::kChaoxing.toString(), {}));
        const QList<Course> remaining = repo->listCourses();
        QCOMPARE(remaining.size(), 2);
        for (const Course &c : remaining) {
            QCOMPARE(c.source, source_keys::kNjuLms.toString());
        }
        // 软删除保留历史：包括被软删的学习通课程
        QCOMPARE(repo->listCourses(true).size(), 3);
    }

    // ---- 作业：重复采集不产生重复记录 --------------------------------------

    void assignments_repeatedSnapshotDoesNotDuplicate()
    {
        auto repo = openRepo(QStringLiteral("_dup"));
        const QString courseId = QStringLiteral("90001");
        const QList<Assignment> first = assignmentsFrom(fixtures::homeworkActivitiesThree(), courseId);

        const BatchApplyResult r1 = repo->applyAssignmentSnapshot(courseId, first);
        QVERIFY2(r1.ok, qPrintable(r1.error));
        QCOMPARE(r1.inserted, 3);
        QCOMPARE(r1.updated, 0);
        QCOMPARE(repo->countAssignments(courseId), 3);

        // 再采集一次同样的数据：不得新增行（UNIQUE(course_id, source_id) 生效）
        const QList<Assignment> again =
            assignmentsFrom(fixtures::homeworkActivitiesThree(), courseId);
        const BatchApplyResult r2 = repo->applyAssignmentSnapshot(courseId, again);
        QVERIFY2(r2.ok, qPrintable(r2.error));
        QCOMPARE(r2.inserted, 0);
        QCOMPARE(r2.updated, 0);
        QCOMPARE(r2.unchanged, 3);   // 摘要未变 → 只刷新 last_seen_at
        QCOMPARE(repo->countAssignments(courseId), 3);
    }

    // ---- 场景 E：截止时间 / 标题发生变化 → 更新原记录 ----------------------

    void assignments_deadlineChangeUpdatesInPlace()
    {
        auto repo = openRepo(QStringLiteral("_due"));
        const QString courseId = QStringLiteral("90001");

        QVERIFY(repo->applyAssignmentSnapshot(
                     courseId, assignmentsFrom(fixtures::homeworkActivitiesThree(), courseId))
                     .ok);

        const auto before = repo->findAssignment(courseId, QStringLiteral("homework-70001"));
        QVERIFY(before.has_value());
        QCOMPARE(before->dueAt.require(),
                 QDateTime(QDate(2026, 9, 24), QTime(15, 59, 0), QTimeZone::UTC));
        const QDateTime firstSeen = before->firstSeenAt;

        // 只有这一条作业，截止时间被改到 10-15
        const BatchApplyResult r = repo->applyAssignmentSnapshot(
            courseId, assignmentsFrom(fixtures::homeworkActivitiesDeadlineChanged(), courseId));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.updated, 1);
        QCOMPARE(r.inserted, 0);

        // 仍然是同一条记录，不产生重复
        QCOMPARE(repo->countAssignments(courseId), 1);

        const auto after = repo->findAssignment(courseId, QStringLiteral("homework-70001"));
        QVERIFY(after.has_value());
        QCOMPARE(after->dueAt.require(),
                 QDateTime(QDate(2026, 10, 15), QTime(15, 59, 0), QTimeZone::UTC));

        // 内容变化时必须记录新的观察时间，但首次发现时间保持不变
        QVERIFY(after->observedAt >= before->observedAt);
        QCOMPARE(after->firstSeenAt, firstSeen);
    }

    void assignments_titleChangeUpdatesInPlace()
    {
        auto repo = openRepo(QStringLiteral("_title"));
        const QString courseId = QStringLiteral("90001");

        QVERIFY(repo->applyAssignmentSnapshot(
                     courseId, assignmentsFrom(fixtures::homeworkActivitiesThree(), courseId))
                     .ok);
        const BatchApplyResult r = repo->applyAssignmentSnapshot(
            courseId, assignmentsFrom(fixtures::homeworkActivitiesTitleChanged(), courseId));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.updated, 1);
        QCOMPARE(repo->countAssignments(courseId), 1);

        const auto after = repo->findAssignment(courseId, QStringLiteral("homework-70001"));
        QVERIFY(after.has_value());
        QCOMPARE(after->title.require(), QStringLiteral("测试作业一（已改名）"));
    }

    // ---- 场景 F：字段解析不到 → 不得用空值覆盖有效数据 --------------------

    void assignments_missingFieldsDoNotOverwriteValidData()
    {
        auto repo = openRepo(QStringLiteral("_sceneF"));
        const QString courseId = QStringLiteral("90001");

        // 第一次：完整数据
        QVERIFY(repo->applyAssignmentSnapshot(
                     courseId, assignmentsFrom(fixtures::homeworkActivitiesThree(), courseId))
                     .ok);

        const auto before = repo->findAssignment(courseId, QStringLiteral("homework-70001"));
        QVERIFY(before.has_value());
        QVERIFY(before->title.hasValue());
        QVERIFY(before->dueAt.hasValue());
        QVERIFY(before->score.hasValue());

        // 第二次：标题/截止时间的 key 消失，score 变成非法类型
        const BatchApplyResult r = repo->applyAssignmentSnapshot(
            courseId, assignmentsFrom(fixtures::homeworkActivitiesFieldsDropped(), courseId));
        QVERIFY2(r.ok, qPrintable(r.error));

        // 被保护的字段必须被报告出来
        QVERIFY(!r.preservedFields.isEmpty());
        QVERIFY(r.preservedFields.contains(QStringLiteral("title")));
        QVERIFY(r.preservedFields.contains(QStringLiteral("dueAt")));
        QVERIFY(r.preservedFields.contains(QStringLiteral("score")));

        const auto after = repo->findAssignment(courseId, QStringLiteral("homework-70001"));
        QVERIFY(after.has_value());

        // 核心断言：旧的有效值全部保留，没有被写成 NULL
        QVERIFY2(after->title.hasValue(), "title must not be cleared by a missing field");
        QCOMPARE(after->title.require(), QStringLiteral("测试作业一"));

        QVERIFY2(after->dueAt.hasValue(), "dueAt must not be cleared by a missing field");
        QCOMPARE(after->dueAt.require(),
                 QDateTime(QDate(2026, 9, 24), QTime(15, 59, 0), QTimeZone::UTC));

        QVERIFY2(after->score.hasValue(), "score must not be cleared by an invalid field");
        QCOMPARE(after->score.require(), 100.0);

        // 确实提供了新值的字段应当照常更新
        QVERIFY(after->submitted.hasValue());
        QVERIFY(after->submitted.require());
    }

    void assignments_invalidTimeDoesNotOverwrite()
    {
        auto repo = openRepo(QStringLiteral("_badtime"));
        const QString courseId = QStringLiteral("90001");

        QVERIFY(repo->applyAssignmentSnapshot(
                     courseId, assignmentsFrom(fixtures::homeworkActivitiesThree(), courseId))
                     .ok);
        const BatchApplyResult r = repo->applyAssignmentSnapshot(
            courseId, assignmentsFrom(fixtures::homeworkActivitiesBadTime(), courseId));
        QVERIFY2(r.ok, qPrintable(r.error));

        const auto after = repo->findAssignment(courseId, QStringLiteral("homework-70001"));
        QVERIFY(after.has_value());
        QVERIFY(after->dueAt.hasValue());
        QCOMPARE(after->dueAt.require(),
                 QDateTime(QDate(2026, 9, 24), QTime(15, 59, 0), QTimeZone::UTC));
    }

    // ---- 分页 / 快照变化：不因列表变化而丢项 ------------------------------

    void assignments_shrinkingSnapshotMarksMissingNotDeletes()
    {
        auto repo = openRepo(QStringLiteral("_shrink"));
        const QString courseId = QStringLiteral("90001");

        QVERIFY(repo->applyAssignmentSnapshot(
                     courseId, assignmentsFrom(fixtures::homeworkActivitiesThree(), courseId))
                     .ok);
        QCOMPARE(repo->countAssignments(courseId), 3);

        // 快照缩到 1 条（模拟作业被取消发布/移出可见范围）
        const BatchApplyResult r = repo->applyAssignmentSnapshot(
            courseId, assignmentsFrom(fixtures::homeworkActivitiesTitleChanged(), courseId));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.markedMissing, 2);

        // 可见列表只剩 1 条，但另 2 条仍在库中（保留历史，可追溯）
        QCOMPARE(repo->countAssignments(courseId), 1);
        QCOMPARE(repo->listAssignments(courseId, true).size(), 3);

        // 重新出现时必须恢复 present，而不是新增重复记录
        const BatchApplyResult r2 = repo->applyAssignmentSnapshot(
            courseId, assignmentsFrom(fixtures::homeworkActivitiesThree(), courseId));
        QVERIFY2(r2.ok, qPrintable(r2.error));
        QCOMPARE(r2.inserted, 0);
        QCOMPARE(repo->countAssignments(courseId), 3);
        QCOMPARE(repo->listAssignments(courseId, true).size(), 3);
    }

    void assignments_emptySnapshotMarksAllMissing()
    {
        auto repo = openRepo(QStringLiteral("_empty"));
        const QString courseId = QStringLiteral("90001");

        QVERIFY(repo->applyAssignmentSnapshot(
                     courseId, assignmentsFrom(fixtures::homeworkActivitiesThree(), courseId))
                     .ok);
        const BatchApplyResult r = repo->applyAssignmentSnapshot(courseId, {});
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.markedMissing, 3);
        QCOMPARE(repo->countAssignments(courseId), 0);
        QCOMPARE(repo->listAssignments(courseId, true).size(), 3);
    }

    // ---- 事务 ---------------------------------------------------------------

    void transaction_commitPersistsAndRollbackDiscards()
    {
        auto repo = openRepo(QStringLiteral("_tx"));
        const QString courseId = QStringLiteral("90001");
        const QList<Assignment> data =
            assignmentsFrom(fixtures::homeworkActivitiesThree(), courseId);

        // 显式事务：提交后可见
        QVERIFY(repo->beginTransaction());
        QVERIFY(repo->inTransaction());
        const BatchApplyResult ok = repo->applyAssignmentSnapshot(courseId, data);
        QVERIFY2(ok.ok, qPrintable(ok.error));
        QVERIFY(repo->commit());
        QVERIFY(!repo->inTransaction());
        QCOMPARE(repo->countAssignments(courseId), 3);

        // 回滚：整体撤销。
        // 注意这里断言 3 而不是 1：基线虽然只有 1 条 present=1，
        // 但「截止时间变化」的快照只含 1 条作业，会把另外 2 条标记为 present=0。
        // 回滚必须把这次标记也一并撤销，三条记录的 present 都回到 1。
        // 若错误地得到 1，说明标记缺失的那条 UPDATE 逃出了事务。
        QVERIFY(repo->beginTransaction());
        const BatchApplyResult r = repo->applyAssignmentSnapshot(
            courseId, assignmentsFrom(fixtures::homeworkActivitiesDeadlineChanged(), courseId));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.markedMissing, 2);
        // 事务内已可见被标记的效果
        QCOMPARE(repo->countAssignments(courseId), 1);

        QVERIFY(repo->rollback());
        QCOMPARE(repo->countAssignments(courseId), 3);
        QCOMPARE(repo->listAssignments(courseId, true).size(), 3);
    }

    void transaction_nestedBeginIsRejected()
    {
        auto repo = openRepo(QStringLiteral("_nested"));
        QVERIFY(repo->beginTransaction());
        // 嵌套事务不受支持，必须明确拒绝而不是静默产生错误的提交语义
        QString error;
        QVERIFY(!repo->beginTransaction(&error));
        QVERIFY(error.contains(QStringLiteral("nested")));
        QVERIFY(repo->rollback());
    }

    // ---- 断点与批次 ---------------------------------------------------------

    void snapshot_recordsBatchAndAdvancesCheckpoint()
    {
        auto repo = openRepo(QStringLiteral("_cp"));
        const QString courseId = QStringLiteral("90001");

        QString error;
        const qint64 runId = repo->startRun(QStringLiteral("full-scan"), QStringLiteral("full"),
                                           &error);
        QVERIFY2(runId > 0, qPrintable(error));

        const BatchApplyResult r = repo->applyAssignmentSnapshot(
            courseId, assignmentsFrom(fixtures::homeworkActivitiesThree(), courseId), runId, 1,
            QStringLiteral("start"), QStringLiteral("course:90001"));
        QVERIFY2(r.ok, qPrintable(r.error));

        const auto batch = repo->findLastCommittedBatch(runId);
        QVERIFY(batch.has_value());
        QCOMPARE(batch->batchNo, 1);
        QVERIFY(batch->committedAt.has_value());

        const auto state = repo->findSyncState(SqliteRepository::courseTaskKey(courseId));
        QVERIFY(state.has_value());
        QVERIFY(state->fullScanCompleted);
        QCOMPARE(state->cursor, QStringLiteral("course:90001"));
    }

    void runs_orphanedRunningIsReapedOnStartup()
    {
        // 场景 A：上次异常退出，run 仍停留在 running
        auto repo = openRepo(QStringLiteral("_orphan"));
        const qint64 runId = repo->startRun(QStringLiteral("full-scan"), QStringLiteral("full"));
        QVERIFY(runId > 0);

        const auto before = repo->findRun(runId);
        QVERIFY(before.has_value());
        QCOMPARE(before->status, CrawlRun::Status::Running);
        QVERIFY(!before->finishedAt.has_value());

        // 重启后回收
        QCOMPARE(repo->reapOrphanedRuns(), 1);

        const auto after = repo->findRun(runId);
        QVERIFY(after.has_value());
        QCOMPARE(after->status, CrawlRun::Status::Failed);
        QVERIFY(after->finishedAt.has_value());
        QVERIFY(after->errorMessage.contains(QStringLiteral("interrupted")));
    }

    void runs_allStatusesRoundTrip()
    {
        auto repo = openRepo(QStringLiteral("_statuses"));
        const QList<CrawlRun::Status> all = {
            CrawlRun::Status::Success,   CrawlRun::Status::Partial,
            CrawlRun::Status::Failed,    CrawlRun::Status::NeedLogin,
            CrawlRun::Status::Cancelled,
        };
        for (CrawlRun::Status s : all) {
            const qint64 id = repo->startRun(QStringLiteral("t"), QStringLiteral("full"));
            QVERIFY(id > 0);
            QVERIFY(repo->finishRun(id, s));
            const auto run = repo->findRun(id);
            QVERIFY(run.has_value());
            QCOMPARE(run->status, s);
        }
    }

    // ---- 解析异常表 --------------------------------------------------------

    void parseErrors_areRecordedAndListed()
    {
        auto repo = openRepo(QStringLiteral("_errors"));
        QList<ParseErrorRecord> records;
        ParseErrorRecord rec;
        rec.runId = 7;
        rec.courseId = QStringLiteral("90001");
        rec.sourceId = QStringLiteral("homework-70001");
        rec.field = QStringLiteral("dueAt");
        rec.reason = QStringLiteral("unrecognized time format");
        rec.observedAt = fixtures::observedBeforeDeadlines();
        records.append(rec);

        QString error;
        QVERIFY2(repo->recordParseErrors(records, &error), qPrintable(error));

        const QList<ParseErrorRecord> loaded = repo->listRecentParseErrors();
        QCOMPARE(loaded.size(), 1);
        QCOMPARE(loaded.first().field, QStringLiteral("dueAt"));
        QCOMPARE(loaded.first().sourceId, QStringLiteral("homework-70001"));
    }

    // ---- 参数校验 -----------------------------------------------------------

    void snapshot_rejectsEmptyCourseId()
    {
        auto repo = openRepo(QStringLiteral("_nocid"));
        const BatchApplyResult r = repo->applyAssignmentSnapshot(QString(), {});
        QVERIFY(!r.ok);
        QVERIFY(!r.error.isEmpty());
    }

    void snapshot_onClosedDatabaseFailsCleanly()
    {
        auto repo = std::make_unique<SqliteRepository>();
        // 未 open 即使用：必须失败并给出原因，而不是崩溃
        const BatchApplyResult r = repo->applyAssignmentSnapshot(
            QStringLiteral("90001"),
            assignmentsFrom(fixtures::homeworkActivitiesThree(), QStringLiteral("90001")));
        QVERIFY(!r.ok);
        QVERIFY(!r.error.isEmpty());
    }

    // ---- v2：快照完整性 / 写放大 / 复活 / 保留策略 / 结构迁移 ------------------

    /// 不完整的快照：不做缺失检测、不把该课程断点标记为完成
    void snapshot_incompleteSnapshotKeepsRowsAndBreakpoint()
    {
        auto repo = openRepo(QStringLiteral("_incomplete"));
        const QString courseId = QStringLiteral("90001");

        const QList<Assignment> three =
            assignmentsFrom(fixtures::homeworkActivitiesThree(), courseId);
        QCOMPARE(three.size(), 3);
        const BatchApplyResult first =
            repo->applyAssignmentSnapshot(courseId, three, 1, 1, QStringLiteral("before"),
                                          QStringLiteral("after"), true, QStringLiteral("full"));
        QVERIFY2(first.ok, qPrintable(first.error));
        QVERIFY(!first.partialSnapshot);
        QCOMPARE(repo->countAssignments(courseId), 3);
        const auto stateAfterFull = repo->findSyncState(SqliteRepository::courseTaskKey(courseId));
        QVERIFY(stateAfterFull.has_value());
        QVERIFY(stateAfterFull->fullScanCompleted);
        QCOMPARE(stateAfterFull->mode, QStringLiteral("full"));

        // 只有 1 条且 snapshotComplete=false：与库中 70001 同 ID，另外两条"本轮没出现"
        const QList<Assignment> partial =
            assignmentsFrom(fixtures::homeworkActivitiesDeadlineChanged(), courseId);
        QCOMPARE(partial.size(), 1);
        const BatchApplyResult second =
            repo->applyAssignmentSnapshot(courseId, partial, 1, 2, QStringLiteral("before2"),
                                          QStringLiteral("after2"), false,
                                          QStringLiteral("incremental"));
        QVERIFY2(second.ok, qPrintable(second.error));
        QVERIFY(second.partialSnapshot);
        QCOMPARE(second.markedMissing, 0);            // 关键：不得误软删除
        QCOMPARE(repo->countAssignments(courseId), 3);
        QCOMPARE(repo->listAssignments(courseId, true).size(), 3);

        const auto stateAfterPartial = repo->findSyncState(SqliteRepository::courseTaskKey(courseId));
        QVERIFY(stateAfterPartial.has_value());
        QVERIFY(!stateAfterPartial->fullScanCompleted);   // 关键：断点不被推进
        QCOMPARE(stateAfterPartial->mode, QStringLiteral("incremental"));
        QCOMPARE(repo->listBatches(1).size(), 2);         // 批次仍然各自记账
    }

    /// 未变记录不再被逐行重写（v2 去掉写放大），但内容仍原样保留
    void snapshot_unchangedRowsAreNotRewritten()
    {
        auto repo = openRepo(QStringLiteral("_touch"));
        const QString courseId = QStringLiteral("90001");
        const QList<Assignment> three =
            assignmentsFrom(fixtures::homeworkActivitiesThree(), courseId);
        QVERIFY(repo->applyAssignmentSnapshot(courseId, three, 1, 1, QString(), QString(), true,
                                             QStringLiteral("full"))
                    .ok);

        const auto before =
            repo->findAssignment(courseId, QStringLiteral("homework-70001"));
        QVERIFY(before.has_value());

        QTest::qWait(5);   // 让"现在"至少前进一点，否则时间戳相同说明不了问题
        const BatchApplyResult again =
            repo->applyAssignmentSnapshot(courseId, three, 1, 2, QString(), QString(), true,
                                          QStringLiteral("incremental"));
        QVERIFY2(again.ok, qPrintable(again.error));
        QCOMPARE(again.inserted, 0);
        QCOMPARE(again.updated, 0);
        QCOMPARE(again.unchanged, 3);

        const auto after = repo->findAssignment(courseId, QStringLiteral("homework-70001"));
        QVERIFY(after.has_value());
        QCOMPARE(after->lastSeenAt, before->lastSeenAt);   // 未变 → 一个字节都没写
        QCOMPARE(after->contentHash, before->contentHash);
    }

    /// 被软删除的作业重新出现在快照里时必须复活（present 回到 1）
    void snapshot_reappearingAssignmentIsRestored()
    {
        auto repo = openRepo(QStringLiteral("_revive"));
        const QString courseId = QStringLiteral("90001");
        const QList<Assignment> three =
            assignmentsFrom(fixtures::homeworkActivitiesThree(), courseId);
        QVERIFY(repo->applyAssignmentSnapshot(courseId, three, 1, 1, QString(), QString(), true,
                                             QStringLiteral("full"))
                    .ok);

        // 只给两条 → 第三条被软删除
        QList<Assignment> two = three;
        two.removeLast();
        const BatchApplyResult shrunk =
            repo->applyAssignmentSnapshot(courseId, two, 1, 2, QString(), QString(), true,
                                          QStringLiteral("incremental"));
        QVERIFY2(shrunk.ok, qPrintable(shrunk.error));
        QCOMPARE(shrunk.markedMissing, 1);
        QCOMPARE(repo->countAssignments(courseId), 2);
        QCOMPARE(repo->listAssignments(courseId, true).size(), 3);

        // 再给三条（内容与首次完全相同）→ 那条必须复活
        const BatchApplyResult back =
            repo->applyAssignmentSnapshot(courseId, three, 1, 3, QString(), QString(), true,
                                          QStringLiteral("incremental"));
        QVERIFY2(back.ok, qPrintable(back.error));
        QCOMPARE(back.unchanged, 3);
        QCOMPARE(repo->countAssignments(courseId), 3);
    }

    /// 历史保留策略：只保留最近 N 次运行（并连带清理批次）
    void runs_pruneKeepsRecentHistoryOnly()
    {
        auto repo = openRepo(QStringLiteral("_prune"));
        for (int i = 0; i < 5; ++i) {
            const qint64 runId = repo->startRun(QStringLiteral("sync"), QStringLiteral("full"));
            QVERIFY(runId > 0);
            QVERIFY(repo->finishRun(runId, CrawlRun::Status::Success));
        }
        QCOMPARE(repo->listRecentRuns(100).size(), 5);

        QString error;
        const int removed = repo->pruneCrawlHistory(2, 30, &error);
        QVERIFY2(removed >= 0, qPrintable(error));
        QCOMPARE(removed, 3);
        QCOMPARE(repo->listRecentRuns(100).size(), 2);
    }

    /// v1 结构（courses 缺 v2/v3 各列）必须能就地迁移到当前版本
    void schema_migratesV1DatabaseToCurrent()
    {
        const QString path = QDir(m_dir.path()).filePath(QStringLiteral("legacy.db"));
        const QString setup = QStringLiteral("legacy_setup");
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), setup);
            db.setDatabaseName(path);
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec(QStringLiteral(
                "CREATE TABLE schema_version (version INTEGER NOT NULL PRIMARY KEY,"
                " applied_at TEXT NOT NULL)")));
            QVERIFY(q.exec(QStringLiteral(
                "INSERT INTO schema_version(version, applied_at) VALUES(1, '2026-01-01T00:00:00Z')")));
            QVERIFY(q.exec(QStringLiteral(
                "CREATE TABLE courses (course_id TEXT NOT NULL PRIMARY KEY, course_name TEXT,"
                " detail_url TEXT, course_code TEXT, department TEXT, instructors TEXT,"
                " semester_code TEXT, year_code TEXT, present INTEGER NOT NULL DEFAULT 1,"
                " last_seen_at TEXT)")));
            QVERIFY(q.exec(QStringLiteral(
                "INSERT INTO courses(course_id, course_name) VALUES('90001', '旧课程')")));
            db.close();
        }
        QSqlDatabase::removeDatabase(setup);

        SqliteRepository repo;
        QString error;
        QVERIFY2(repo.open(path, &error), qPrintable(error));
        QCOMPARE(repo.listCourses(true).size(), 1);
        // 迁移时 source 回填默认值：历史数据视为智汇南雍
        QCOMPARE(repo.listCourses(true).first().source, QStringLiteral("nju-lms"));

        // 迁移成功的判据：写入快照时会更新 courses.last_synced_at / assignment_count
        const QList<Assignment> three =
            assignmentsFrom(fixtures::homeworkActivitiesThree(), QStringLiteral("90001"));
        const BatchApplyResult applied =
            repo.applyAssignmentSnapshot(QStringLiteral("90001"), three, 1, 1, QString(), QString(),
                                         true, QStringLiteral("full"));
        QVERIFY2(applied.ok, qPrintable(applied.error));
        repo.close();

        const QString check = QStringLiteral("legacy_check");
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), check);
            db.setDatabaseName(path);
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec(QStringLiteral("SELECT MAX(version) FROM schema_version")));
            QVERIFY(q.next());
            QCOMPARE(q.value(0).toInt(), 3);
            QVERIFY(q.exec(QStringLiteral("PRAGMA table_info(courses)")));
            QStringList columns;
            while (q.next()) {
                columns << q.value(1).toString();
            }
            QVERIFY(columns.contains(QStringLiteral("last_synced_at")));
            QVERIFY(columns.contains(QStringLiteral("assignment_count")));
            QVERIFY(columns.contains(QStringLiteral("source")));
            db.close();
        }
        QSqlDatabase::removeDatabase(check);
    }

    /// v2：清空采集数据（保留结构版本，表可继续使用）
    void clearAllData_removesCollectedDataAndKeepsSchema()
    {
        auto repo = openRepo(QStringLiteral("_clear"));
        const QList<Course> courses = coursesFrom(fixtures::myCoursesTwoCourses());
        QVERIFY(repo->applyCourseSnapshot(source_keys::kNjuLms.toString(), courses));

        const QList<Assignment> three =
            assignmentsFrom(fixtures::homeworkActivitiesThree(), QStringLiteral("90001"));
        const BatchApplyResult applied =
            repo->applyAssignmentSnapshot(QStringLiteral("90001"), three, 1, 1, QString(),
                                          QString(), true, QStringLiteral("full"));
        QVERIFY2(applied.ok, qPrintable(applied.error));

        const qint64 runId = repo->startRun(QStringLiteral("sync"), QStringLiteral("full"));
        QVERIFY(runId > 0);
        QVERIFY(repo->finishRun(runId, CrawlRun::Status::Success));

        ParseErrorRecord record;
        record.runId = runId;
        record.courseId = QStringLiteral("90001");
        record.field = QStringLiteral("dueAt");
        record.reason = QStringLiteral("invalid");
        record.observedAt = QDateTime::currentDateTimeUtc();
        QVERIFY(repo->recordParseErrors({record}));

        QCOMPARE(repo->listCourses(true).size(), 2);
        QCOMPARE(repo->countAssignments(QStringLiteral("90001")), 3);
        QVERIFY(repo->findSyncState(SqliteRepository::courseTaskKey(QStringLiteral("90001")))
                    .has_value());
        QCOMPARE(repo->listRecentRuns(10).size(), 1);
        QCOMPARE(repo->listRecentParseErrors().size(), 1);

        QString error;
        const int removed = repo->clearAllData(&error);
        QVERIFY2(removed > 0, qPrintable(error));

        QCOMPARE(repo->listCourses(true).size(), 0);
        QCOMPARE(repo->countAssignments(QStringLiteral("90001")), 0);
        QVERIFY(!repo->findSyncState(SqliteRepository::courseTaskKey(QStringLiteral("90001")))
                     .has_value());
        QCOMPARE(repo->listSyncStates().size(), 0);
        QCOMPARE(repo->listRecentRuns(10).size(), 0);
        QCOMPARE(repo->listRecentParseErrors().size(), 0);

        // 结构没变：清空后可以继续正常写入
        QVERIFY(repo->applyCourseSnapshot(source_keys::kNjuLms.toString(), courses));
        QCOMPARE(repo->listCourses().size(), 2);
    }
};

QTEST_MAIN(RepositoryTests)
#include "RepositoryTests.moc"
