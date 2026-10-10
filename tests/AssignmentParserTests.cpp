#include "parser/AssignmentParser.h"
#include "support/Fixtures.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

using namespace njulms;
using namespace njulms::test;

class AssignmentParserTests : public QObject
{
    Q_OBJECT

private slots:
    // ---- 会话与信封：场景 C 的判定基础 -------------------------------------

    void envelope_acceptsJsonObject()
    {
        const ParseOutcome out = AssignmentParser::inspectEnvelope(
            fixtures::myCoursesTwoCourses());
        QVERIFY(out.ok());
        QVERIFY(!out.needsLogin());
    }

    void envelope_rejectsLoginPageHtml()
    {
        // 会话失效时站点返回登录页 HTML 且状态码可能仍是 200。
        // 判定必须是「不是 JSON」而不是看状态码。
        const ParseOutcome out = AssignmentParser::inspectEnvelope(fixtures::loginPageHtml());
        QVERIFY(!out.ok());
        QVERIFY(out.needsLogin());
    }

    void envelope_rejectsEmptyBody()
    {
        const ParseOutcome out = AssignmentParser::inspectEnvelope(QString());
        QVERIFY(!out.ok());
        QVERIFY(out.needsLogin());
    }

    void envelope_rejectsMalformedJson()
    {
        const ParseOutcome out = AssignmentParser::inspectEnvelope(QStringLiteral("{\"a\": }"));
        QVERIFY(!out.ok());
    }

    void envelope_rejectsJsonArray()
    {
        // 顶层是数组而不是对象：形状不符，不能当作正常响应
        const ParseOutcome out = AssignmentParser::inspectEnvelope(QStringLiteral("[1,2,3]"));
        QVERIFY(!out.ok());
    }

    // ---- 课程 ---------------------------------------------------------------

    void courses_parsesMeasuredFields()
    {
        const QJsonObject root =
            QJsonDocument::fromJson(fixtures::myCoursesTwoCourses().toUtf8()).object();
        ParseIssues issues;
        const QList<Course> courses = AssignmentParser::parseCourseList(
            root, QDateTime::fromSecsSinceEpoch(1000, QTimeZone::UTC), &issues);

        QCOMPARE(courses.size(), 2);

        const Course &a = courses.at(0);
        QCOMPARE(a.courseId, QStringLiteral("90001"));
        QCOMPARE(a.courseName, QStringLiteral("测试课程甲"));
        QCOMPARE(a.detailUrl, QStringLiteral("https://lms.nju.edu.cn/course/90001/index"));
        QCOMPARE(a.courseCode, QStringLiteral("TESTCODE001"));
        QVERIFY(a.department.has_value());
        QCOMPARE(*a.department, QStringLiteral("测试学院"));
        QCOMPARE(a.instructors, QStringList{QStringLiteral("教师甲")});
        QVERIFY(a.semesterCode.has_value());
        QCOMPARE(*a.semesterCode, QStringLiteral("2026-1"));
        QVERIFY(a.academicYearCode.has_value());
        QCOMPARE(*a.academicYearCode, QStringLiteral("2026"));

        // department 为 null / instructors 为空数组时必须容忍，而不是报错
        const Course &b = courses.at(1);
        QCOMPARE(b.courseId, QStringLiteral("90002"));
        QVERIFY(!b.department.has_value());
        QVERIFY(b.instructors.isEmpty());
    }

    void courses_emptyListIsNotAnError()
    {
        // 合法状态：确实一门课都没有。必须与「会话失效」区分开。
        const QJsonObject root =
            QJsonDocument::fromJson(fixtures::myCoursesEmpty().toUtf8()).object();
        ParseIssues issues;
        const QList<Course> courses =
            AssignmentParser::parseCourseList(root, fixtures::observedBeforeDeadlines(), &issues);
        QVERIFY(courses.isEmpty());
        QVERIFY(issues.isEmpty());
    }

    void courses_missingKeyIsReportedNotTreatedAsEmpty()
    {
        // 是 JSON 但没有 courses 键：不能静默当成 0 门课程
        const QJsonObject root =
            QJsonDocument::fromJson(QStringLiteral(R"({"other":[]})").toUtf8()).object();
        ParseIssues issues;
        const QList<Course> courses =
            AssignmentParser::parseCourseList(root, fixtures::observedBeforeDeadlines(), &issues);
        QVERIFY(courses.isEmpty());
        QCOMPARE(issues.size(), 1);
        QCOMPARE(issues.first().field, QStringLiteral("courses"));
        QCOMPARE(issues.first().state, FieldState::Missing);
    }

    // ---- 作业：实测字段 -----------------------------------------------------

    void assignments_parsesMeasuredFields()
    {
        const QJsonObject root =
            QJsonDocument::fromJson(fixtures::homeworkActivitiesThree().toUtf8()).object();
        const QJsonArray items = root.value(QStringLiteral("homework_activities")).toArray();
        ParseIssues issues;
        const QDateTime observedAt = fixtures::observedBeforeDeadlines();
        const QList<Assignment> list =
            AssignmentParser::parseAssignments(QStringLiteral("90001"), items, observedAt, &issues);

        QCOMPARE(list.size(), 3);

        const Assignment &a1 = list.at(0);
        QCOMPARE(a1.courseId, QStringLiteral("90001"));
        QCOMPARE(a1.activityId, QStringLiteral("70001"));
        // 实测 homework-activities 条目没有 unique_key，稳定标识按 "{type}-{id}" 推导
        QCOMPARE(a1.sourceId, QStringLiteral("homework-70001"));
        QCOMPARE(a1.title.require(), QStringLiteral("测试作业一"));
        QCOMPARE(a1.description.require(), QStringLiteral("<p>测试说明一</p>"));

        // 截止时间必须是**站点的 deadline**，而不是采集时间
        QVERIFY(a1.dueAt.hasValue());
        QCOMPARE(a1.dueAt.require(), QDateTime(QDate(2026, 9, 24), QTime(15, 59, 0), QTimeZone::UTC));

        // 发布时间来自 data.publish_time
        QVERIFY(a1.publishedAt.hasValue());
        QCOMPARE(a1.publishedAt.require(),
                 QDateTime(QDate(2026, 9, 17), QTime(2, 14, 0), QTimeZone::UTC));

        // score 以字符串给出也必须能解析
        QVERIFY(a1.score.hasValue());
        QCOMPARE(a1.score.require(), 100.0);

        QVERIFY(a1.submitted.hasValue());
        QVERIFY(a1.submitted.require());

        // 状态是推导字段，已提交 → Submitted
        QVERIFY(a1.status.hasValue());
        QCOMPARE(a1.status.require(), AssignmentStatus::Submitted);

        // 详情链接：站点不提供现成 URL，按实测的路由规则推导
        // （2026-10-10 在已登录站点点开作业实测：learning-activity#/{activityId}；
        //   旧的 /activity/{id} 会显示"找不到资源"）
        QVERIFY(a1.detailUrl.hasValue());
        QCOMPARE(a1.detailUrl.require(),
                 QStringLiteral("https://lms.nju.edu.cn/course/90001/learning-activity#/70001"));
    }

    void assignments_missingPublishTimeIsAbsentNotGuessed()
    {
        // 实测：作业 44332 的 data 里完全没有 publish_time。
        // 必须解析为 Missing，绝不能用采集时间兜底。
        const QJsonObject root =
            QJsonDocument::fromJson(fixtures::homeworkActivitiesThree().toUtf8()).object();
        const QJsonArray items = root.value(QStringLiteral("homework_activities")).toArray();
        ParseIssues issues;
        const QList<Assignment> list = AssignmentParser::parseAssignments(
            QStringLiteral("90001"), items, fixtures::observedBeforeDeadlines(), &issues);

        const Assignment &a2 = list.at(1);
        QCOMPARE(a2.activityId, QStringLiteral("70002"));
        QCOMPARE(a2.publishedAt.state(), FieldState::Missing);
        QVERIFY(!a2.publishedAt.hasValue());

        // score 为 null → 确认空值（Absent），是可以入 NULL 的权威结论
        QCOMPARE(a2.score.state(), FieldState::Absent);
        QVERIFY(!a2.score.hasValue());
    }

    void assignments_nullDescriptionBecomesAbsent()
    {
        const QJsonObject root =
            QJsonDocument::fromJson(fixtures::homeworkActivitiesThree().toUtf8()).object();
        const QJsonArray items = root.value(QStringLiteral("homework_activities")).toArray();
        ParseIssues issues;
        const QList<Assignment> list = AssignmentParser::parseAssignments(
            QStringLiteral("90001"), items, fixtures::observedBeforeDeadlines(), &issues);

        const Assignment &a3 = list.at(2);
        QCOMPARE(a3.description.state(), FieldState::Absent);
        // 未提交 + 未关闭 → Pending
        QVERIFY(a3.status.hasValue());
        QCOMPARE(a3.status.require(), AssignmentStatus::Pending);
    }

    // ---- 三态区分：场景 F 的基础 -------------------------------------------

    void assignments_droppedKeysAreMissingNotAbsent()
    {
        // 标题与截止时间的 key 整个消失 → Missing（不得覆盖库中有效值）
        // score 被写成对象 → Invalid（同样不得覆盖）
        const QJsonObject root =
            QJsonDocument::fromJson(fixtures::homeworkActivitiesFieldsDropped().toUtf8()).object();
        const QJsonArray items = root.value(QStringLiteral("homework_activities")).toArray();
        ParseIssues issues;
        const QList<Assignment> list = AssignmentParser::parseAssignments(
            QStringLiteral("90001"), items, fixtures::observedBeforeDeadlines(), &issues);

        QCOMPARE(list.size(), 1);
        const Assignment &a = list.first();

        QCOMPARE(a.title.state(), FieldState::Missing);
        QCOMPARE(a.dueAt.state(), FieldState::Missing);
        QCOMPARE(a.score.state(), FieldState::Invalid);

        // 关键断言：这些状态都不是「权威」，因此仓储不得写入
        QVERIFY(!a.title.isAuthoritative());
        QVERIFY(!a.dueAt.isAuthoritative());
        QVERIFY(!a.score.isAuthoritative());

        // submitted 存在且为 true → 仍有权威结论
        QVERIFY(a.submitted.isAuthoritative());
        QVERIFY(a.submitted.require());
    }

    void assignments_invalidTimeIsInvalidNotAbsent()
    {
        const QJsonObject root =
            QJsonDocument::fromJson(fixtures::homeworkActivitiesBadTime().toUtf8()).object();
        const QJsonArray items = root.value(QStringLiteral("homework_activities")).toArray();
        ParseIssues issues;
        const QList<Assignment> list = AssignmentParser::parseAssignments(
            QStringLiteral("90001"), items, fixtures::observedBeforeDeadlines(), &issues);

        QCOMPARE(list.size(), 1);
        const Assignment &a = list.first();
        QCOMPARE(a.dueAt.state(), FieldState::Invalid);
        QCOMPARE(a.publishedAt.state(), FieldState::Invalid);
        QVERIFY(!a.dueAt.isAuthoritative());

        // 解析异常必须被记录下来，便于之后重试
        bool sawDueIssue = false;
        for (const ParseIssue &issue : issues) {
            if (issue.field == QLatin1String("assignment.deadline")) {
                sawDueIssue = true;
                QCOMPARE(issue.state, FieldState::Invalid);
            }
        }
        QVERIFY(sawDueIssue);
    }

    // ---- 内容摘要 -----------------------------------------------------------

    void signature_isStableAcrossObservations()
    {
        // 同一份数据、不同采集时刻 → 摘要必须相同，
        // 否则每次同步都会误判为「内容已变化」
        const QJsonObject root =
            QJsonDocument::fromJson(fixtures::homeworkActivitiesThree().toUtf8()).object();
        const QJsonArray items = root.value(QStringLiteral("homework_activities")).toArray();

        ParseIssues i1;
        ParseIssues i2;
        const QList<Assignment> first = AssignmentParser::parseAssignments(
            QStringLiteral("90001"), items, QDateTime::fromSecsSinceEpoch(1000, QTimeZone::UTC), &i1);
        const QList<Assignment> second = AssignmentParser::parseAssignments(
            QStringLiteral("90001"), items, QDateTime::fromSecsSinceEpoch(999999, QTimeZone::UTC), &i2);

        QCOMPARE(first.size(), second.size());
        for (int i = 0; i < first.size(); ++i) {
            QCOMPARE(first.at(i).contentHash, second.at(i).contentHash);
        }
    }

    void signature_changesWhenTitleChanges()
    {
        const QJsonObject base =
            QJsonDocument::fromJson(fixtures::homeworkActivitiesThree().toUtf8()).object();
        const QJsonObject changed =
            QJsonDocument::fromJson(fixtures::homeworkActivitiesTitleChanged().toUtf8()).object();

        ParseIssues i1;
        ParseIssues i2;
        const QList<Assignment> a = AssignmentParser::parseAssignments(
            QStringLiteral("90001"),
            base.value(QStringLiteral("homework_activities")).toArray(), fixtures::observedBeforeDeadlines(), &i1);
        const QList<Assignment> b = AssignmentParser::parseAssignments(
            QStringLiteral("90001"),
            changed.value(QStringLiteral("homework_activities")).toArray(), fixtures::observedBeforeDeadlines(), &i2);

        QVERIFY(!a.isEmpty());
        QVERIFY(!b.isEmpty());
        QVERIFY(a.first().contentHash != b.first().contentHash);
    }

    void signature_changesWhenDeadlineChanges()
    {
        const QJsonObject base =
            QJsonDocument::fromJson(fixtures::homeworkActivitiesThree().toUtf8()).object();
        const QJsonObject changed =
            QJsonDocument::fromJson(fixtures::homeworkActivitiesDeadlineChanged().toUtf8()).object();

        ParseIssues i1;
        ParseIssues i2;
        const QList<Assignment> a = AssignmentParser::parseAssignments(
            QStringLiteral("90001"),
            base.value(QStringLiteral("homework_activities")).toArray(), fixtures::observedBeforeDeadlines(), &i1);
        const QList<Assignment> b = AssignmentParser::parseAssignments(
            QStringLiteral("90001"),
            changed.value(QStringLiteral("homework_activities")).toArray(), fixtures::observedBeforeDeadlines(), &i2);

        QVERIFY(a.first().contentHash != b.first().contentHash);
    }

    void signature_isAmbiguityFree()
    {
        // 长度前缀编码：字段内容里含分隔符也不应产生相同摘要
        Assignment one;
        one.title = Observed<QString>::present(QStringLiteral("a"));
        one.description = Observed<QString>::present(QStringLiteral("b"));

        Assignment two;
        two.title = Observed<QString>::present(QStringLiteral("a\x1f"));  // 注入分隔符
        two.description = Observed<QString>::present(QStringLiteral("b"));

        QVERIFY(AssignmentParser::computeContentSignature(one)
                != AssignmentParser::computeContentSignature(two));
    }

    // ---- 稳定标识 -----------------------------------------------------------

    void sourceId_usesUniqueKeyWhenPresent()
    {
        // /activities 条目带 unique_key，必须直接采用站点给的值
        const QJsonObject root =
            QJsonDocument::fromJson(fixtures::activitiesMixed().toUtf8()).object();
        const QJsonArray items = root.value(QStringLiteral("activities")).toArray();
        ParseIssues issues;
        const QList<Assignment> list = AssignmentParser::parseAssignments(
            QStringLiteral("90001"), items, fixtures::observedBeforeDeadlines(), &issues);

        QCOMPARE(list.size(), 2);
        QCOMPARE(list.at(0).sourceId, QStringLiteral("homework-70001"));
        QCOMPARE(list.at(1).sourceId, QStringLiteral("exam-60001"));
    }

    void sourceId_derivationMatchesMeasuredRule()
    {
        // 实测规则：unique_key == "{type}-{id}"。推导值必须与之一致，
        // 否则同一个作业在两个接口下会得到不同的去重键，产生重复记录。
        const QJsonObject acts =
            QJsonDocument::fromJson(fixtures::activitiesMixed().toUtf8()).object();
        ParseIssues i1;
        const QList<Assignment> fromActivities = AssignmentParser::parseAssignments(
            QStringLiteral("90001"),
            acts.value(QStringLiteral("activities")).toArray(), fixtures::observedBeforeDeadlines(), &i1);

        const QJsonObject hw =
            QJsonDocument::fromJson(fixtures::homeworkActivitiesThree().toUtf8()).object();
        ParseIssues i2;
        const QList<Assignment> fromHomework = AssignmentParser::parseAssignments(
            QStringLiteral("90001"),
            hw.value(QStringLiteral("homework_activities")).toArray(), fixtures::observedBeforeDeadlines(), &i2);

        QCOMPARE(fromActivities.at(0).sourceId, fromHomework.at(0).sourceId);
    }

    void assignments_withoutAnyIdAreSkipped()
    {
        const QJsonArray items{
            QJsonObject{{QStringLiteral("title"), QStringLiteral("没有 id 的条目")}}};
        ParseIssues issues;
        const QList<Assignment> list = AssignmentParser::parseAssignments(
            QStringLiteral("90001"), items, fixtures::observedBeforeDeadlines(), &issues);

        // 没有稳定标识就无法安全去重，宁可跳过也不要生成重复记录
        QVERIFY(list.isEmpty());
        QVERIFY(!issues.isEmpty());
    }
};

QTEST_MAIN(AssignmentParserTests)
#include "AssignmentParserTests.moc"
