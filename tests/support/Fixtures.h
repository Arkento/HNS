#pragma once

#include <QDateTime>
#include <QString>
#include <QTimeZone>

namespace njulms {
namespace test {
namespace fixtures {

/**
 * 固定的采集时刻，用于让断言与时区/当前时间无关。
 *
 * ⚠️ 为什么必须固定：作业状态是由 submitted + 截止时间**相对采集时刻**推导的。
 * 若测试直接用「现在」作为采集时刻，那么一旦真实时间越过样本里的截止时间，
 * 原本断言 Pending 的用例会变成 Overdue，测试就会随日历漂移而失败。
 */
inline QDateTime observedBeforeDeadlines()
{
    // 早于全部样本截止时间（最早的截止为 2026-09-24T15:59Z）
    return QDateTime(QDate(2026, 9, 1), QTime(0, 0, 0), QTimeZone::UTC);
}

inline QDateTime observedAfterDeadlines()
{
    // 晚于全部样本截止时间（最晚的截止为 2026-10-08T15:59Z）
    return QDateTime(QDate(2026, 12, 1), QTime(0, 0, 0), QTimeZone::UTC);
}


/**
 * 测试用 JSON 样本。
 *
 * ── 来源与真实性声明 ──────────────────────────────────────────────────────
 * 这些样本的**结构与边界情形**来自 2026-10-09 对本机账号的真实抓取
 * （见 test/raw/*.json 与 v1/_probe/my_courses_full.json），
 * 但**字段值已替换为测试值**（课程名、ID、教师姓名、作业标题）。
 *
 * 这样做的两个理由：
 *   1. 不把真实个人信息（教师姓名、学号相关 ID）固化进源码；
 *   2. 仍然保证解析器面对的是**真实结构**，而不是凭空想象的结构。
 *      （需求文档第八节第 4、9 条：不得使用虚构的字段名，不得写入虚假的真实作业数据。）
 *
 * ── 覆盖的实测边界情形 ────────────────────────────────────────────────────
 *   - data.publish_time 存在 / 不存在（实测作业 44332 就没有）
 *   - deadline 与 end_time 同时存在且相同
 *   - score 以字符串给出（"100"）/ 以 null 给出 / 缺失
 *   - homework-activities 条目**没有** unique_key（实测 6/6 均无）
 *   - activities 条目**有** unique_key，格式为 "{type}-{id}"（实测 6/6 均符合）
 *   - start_time 为 null
 *   - /activities 响应**不含**任何分页元数据（实测）
 */

/// [实测结构] POST /api/my-courses 的响应：两门课程
inline QString myCoursesTwoCourses()
{
    return QStringLiteral(R"JSON({
  "courses": [
    {
      "id": 90001,
      "name": "测试课程甲",
      "url": "https://lms.nju.edu.cn/course/90001/index",
      "course_code": "TESTCODE001",
      "department": { "id": 133, "name": "测试学院" },
      "instructors": [ { "id": 1, "name": "教师甲" } ],
      "semester": { "code": "2026-1", "id": 8, "name": null, "real_name": null, "sort": 0 },
      "academic_year": { "code": "2026", "id": 3, "name": "2026-2027", "sort": 0 },
      "course_type": 1,
      "credit": "3.0",
      "study_completeness": null
    },
    {
      "id": 90002,
      "name": "测试课程乙",
      "url": "https://lms.nju.edu.cn/course/90002/index",
      "course_code": "TESTCODE002",
      "department": null,
      "instructors": [],
      "semester": { "code": "2026-1", "id": 8 },
      "academic_year": { "code": "2026", "id": 3 }
    }
  ]
})JSON");
}

/// 空课程列表：合法状态，必须与会话失效区分开
inline QString myCoursesEmpty()
{
    return QStringLiteral(R"JSON({"courses":[]})JSON");
}

/// 会话失效时站点极可能返回的形态：登录页 HTML，且 HTTP 状态码仍为 200
inline QString loginPageHtml()
{
    return QStringLiteral(
        "<!DOCTYPE html><html><head><title>统一身份认证</title></head>"
        "<body><form id=\"pwdFromId\"><input name=\"username\">"
        "<input id=\"saltPassword\" name=\"password\" type=\"password\">"
        "</form></body></html>");
}

/**
 * [实测结构] GET /api/courses/90001/homework-activities 的响应。
 *
 * 三条作业覆盖实测确认的边界：
 *   #1 有 data.publish_time、score 为字符串 "100"、已提交
 *   #2 **没有** data.publish_time（实测作业 44332 就是这样）、score 为 null
 *   #3 start_time 为 null
 * 三条都**没有** unique_key（与实测 6/6 条一致）。
 */
inline QString homeworkActivitiesThree()
{
    return QStringLiteral(R"JSON({
  "homework_activities": [
    {
      "id": 70001,
      "title": "测试作业一",
      "deadline": "2026-09-24T15:59:00Z",
      "end_time": "2026-09-24T15:59:00Z",
      "start_time": "2026-09-17T02:20:00Z",
      "created_at": "2026-09-17T02:14:44Z",
      "updated_at": "2026-09-17T02:14:44Z",
      "type": "homework",
      "submitted": true,
      "score": "100",
      "is_closed": true,
      "is_started": true,
      "published": true,
      "data": {
        "description": "<p>测试说明一</p>",
        "publish_time": "2026-09-17T02:14:00Z",
        "homework_type": "file_upload"
      }
    },
    {
      "id": 70002,
      "title": "测试作业二",
      "deadline": "2026-10-01T15:59:00Z",
      "end_time": "2026-10-01T15:59:00Z",
      "start_time": null,
      "created_at": "2026-09-24T10:32:07Z",
      "updated_at": "2026-09-30T09:30:38Z",
      "type": "homework",
      "submitted": true,
      "score": null,
      "is_closed": true,
      "is_started": true,
      "published": true,
      "data": {
        "description": "<p>测试说明二</p>",
        "homework_type": "file_upload"
      }
    },
    {
      "id": 70003,
      "title": "测试作业三",
      "deadline": "2026-10-08T15:59:00Z",
      "end_time": "2026-10-08T15:59:00Z",
      "start_time": "2026-09-29T09:59:00Z",
      "created_at": "2026-09-29T09:56:01Z",
      "updated_at": "2026-09-29T09:56:02Z",
      "type": "homework",
      "submitted": false,
      "score": null,
      "is_closed": false,
      "is_started": true,
      "published": true,
      "data": {
        "description": null,
        "publish_time": "2026-09-29T09:56:00Z"
      }
    }
  ],
  "page": 1,
  "page_size": 10,
  "pages": 1,
  "total": 3,
  "start": 1,
  "end": 3
})JSON");
}

// ---------------------------------------------------------------------------
// v2：/api/todos（跨课程待办，用作增量同步的「变更信号」）
// ---------------------------------------------------------------------------

/// 构造 /api/todos 响应。items = {{courseId, activityId, endTime}, ...}
inline QString todosResponse(const QList<QStringList> &items)
{
    QString json;
    for (const QStringList &item : items) {
        if (item.size() < 3) {
            continue;
        }
        if (!json.isEmpty()) {
            json += QLatin1Char(',');
        }
        json += QStringLiteral(
                    R"({"course_id":"%1","id":%2,"type":"homework","end_time":"%3"})")
                    .arg(item.at(0), item.at(1), item.at(2));
    }
    return QStringLiteral(R"({"todo_list":[%1]})").arg(json);
}

/// 与 homeworkActivitiesThree() 一致的待办集合（90001/90002 各 3 条，截止时间相同）
inline QString todosMatchingHomeworkThree()
{
    const QStringList deadlines = {QStringLiteral("2026-09-24T15:59:00Z"),
                                   QStringLiteral("2026-10-01T15:59:00Z"),
                                   QStringLiteral("2026-10-08T15:59:00Z")};
    QList<QStringList> items;
    for (const QString &course : {QStringLiteral("90001"), QStringLiteral("90002")}) {
        for (int i = 0; i < 3; ++i) {
            items << QStringList{course, QString::number(70001 + i), deadlines.at(i)};
        }
    }
    return todosResponse(items);
}

/// 90002 的待办已清空（模拟"提交后待办消失"），90001 保持三条
inline QString todosWith90002Cleared()
{
    const QStringList deadlines = {QStringLiteral("2026-09-24T15:59:00Z"),
                                   QStringLiteral("2026-10-01T15:59:00Z"),
                                   QStringLiteral("2026-10-08T15:59:00Z")};
    QList<QStringList> items;
    for (int i = 0; i < 3; ++i) {
        items << QStringList{QStringLiteral("90001"), QString::number(70001 + i), deadlines.at(i)};
    }
    return todosResponse(items);
}

/// 空作业列表（合法：该课程确实没有作业）
inline QString homeworkActivitiesEmpty()
{
    return QStringLiteral(R"JSON({
  "homework_activities": [],
  "page": 1, "page_size": 10, "pages": 1, "total": 0, "start": 1, "end": 0
})JSON");
}

/**
 * 服务端夹取 page_size 的情形：请求 page_size=200，只回来 10 条，
 * 但 total=25 → 适配器必须判定 snapshotComplete=false，断点不得推进。
 */
inline QString homeworkActivitiesTruncated()
{
    QString items;
    for (int i = 0; i < 10; ++i) {
        if (i > 0) {
            items += QLatin1Char(',');
        }
        items += QStringLiteral(
                     R"({"id":%1,"title":"分页作业%2","deadline":"2026-10-08T15:59:00Z",)"
                     R"("type":"homework","submitted":false,"is_closed":false,)"
                     R"("data":{"description":"x"}})")
                     .arg(80000 + i)
                     .arg(i);
    }

    // 服务端夹取了 page_size：请求 page_size=200，只回 10 条，但 total=25。
    const QString envelope =
        QStringLiteral(R"({"homework_activities":[%1],"page":1,"page_size":10,)"
                       R"("pages":3,"total":25,"start":1,"end":10})")
            .arg(items);
    return envelope;
}

/**
 * [实测结构] GET /api/courses/90001/activities 的响应。
 * 实测：每条 homework 都有 unique_key，格式 "{type}-{id}"；
 *       响应**不含** total/page/page_size/pages 任何分页元数据。
 */
inline QString activitiesMixed()
{
    return QStringLiteral(R"JSON({
  "activities": [
    {
      "id": 70001,
      "unique_key": "homework-70001",
      "title": "测试作业一",
      "type": "homework",
      "deadline": "2026-09-24T15:59:00Z",
      "end_time": "2026-09-24T15:59:00Z",
      "start_time": "2026-09-17T02:20:00Z",
      "updated_at": "2026-09-17T02:14:44Z",
      "is_closed": true,
      "is_started": true,
      "published": true,
      "data": { "description": "<p>测试说明一</p>", "publish_time": "2026-09-17T02:14:00Z" }
    },
    {
      "id": 60001,
      "unique_key": "exam-60001",
      "title": "测试测验",
      "type": "exam",
      "deadline": "2026-11-01T15:59:00Z",
      "data": {}
    }
  ]
})JSON");
}

/// 作业标题被修改（用于验证「不产生重复记录，且更新变化字段」）
inline QString homeworkActivitiesTitleChanged()
{
    return QStringLiteral(R"JSON({
  "homework_activities": [
    {
      "id": 70001,
      "title": "测试作业一（已改名）",
      "deadline": "2026-09-24T15:59:00Z",
      "end_time": "2026-09-24T15:59:00Z",
      "start_time": "2026-09-17T02:20:00Z",
      "updated_at": "2026-09-25T00:00:00Z",
      "type": "homework",
      "submitted": true,
      "score": "100",
      "is_closed": true,
      "is_started": true,
      "published": true,
      "data": { "description": "<p>测试说明一</p>", "publish_time": "2026-09-17T02:14:00Z" }
    }
  ],
  "page": 1, "page_size": 10, "pages": 1, "total": 1, "start": 1, "end": 1
})JSON");
}

/// 截止时间被修改（验证场景 E）
inline QString homeworkActivitiesDeadlineChanged()
{
    return QStringLiteral(R"JSON({
  "homework_activities": [
    {
      "id": 70001,
      "title": "测试作业一",
      "deadline": "2026-10-15T15:59:00Z",
      "end_time": "2026-10-15T15:59:00Z",
      "start_time": "2026-09-17T02:20:00Z",
      "updated_at": "2026-09-25T00:00:00Z",
      "type": "homework",
      "submitted": true,
      "score": "100",
      "is_closed": true,
      "is_started": true,
      "published": true,
      "data": { "description": "<p>测试说明一</p>", "publish_time": "2026-09-17T02:14:00Z" }
    }
  ],
  "page": 1, "page_size": 10, "pages": 1, "total": 1, "start": 1, "end": 1
})JSON");
}

/**
 * 场景 F 的核心样本：标题与截止时间**整个 key 都消失**，
 * 且 score 被写成非法类型（对象）以触发 Invalid。
 * 期望：库中原有的有效值全部保留，不被空值覆盖。
 */
inline QString homeworkActivitiesFieldsDropped()
{
    return QStringLiteral(R"JSON({
  "homework_activities": [
    {
      "id": 70001,
      "type": "homework",
      "submitted": true,
      "score": { "unexpected": true },
      "data": {}
    }
  ],
  "page": 1, "page_size": 10, "pages": 1, "total": 1, "start": 1, "end": 1
})JSON");
}

/// 逾期作业（用于验证「已提交且已截止」不得被误判为 Overdue）
inline QString homeworkActivitiesOverdue()
{
    return QStringLiteral(R"JSON({
  "homework_activities": [
    {
      "id": 70001,
      "title": "测试作业一",
      "deadline": "2026-09-24T15:59:00Z",
      "end_time": "2026-09-24T15:59:00Z",
      "type": "homework",
      "submitted": false,
      "is_closed": true,
      "is_started": true,
      "data": { "description": "x", "publish_time": "2026-09-17T02:14:00Z" }
    }
  ],
  "page": 1, "page_size": 10, "pages": 1, "total": 1, "start": 1, "end": 1
})JSON");
}

// ---------------------------------------------------------------------------
// 多页快照：用于验证服务端夹取 page_size 时的自动翻页与合并
// 每页 10 条（与服务端默认页大小一致），共 2.5 页 → total = 25
// ---------------------------------------------------------------------------

inline QString homeworkPage(int pageNo, int total, int perPage, int idBase);

/// 第 1 页：10 条
inline QString homeworkPage1()
{
    return homeworkPage(1, 25, 10, 80000);
}

/// 第 2 页：10 条
inline QString homeworkPage2()
{
    return homeworkPage(2, 25, 10, 80010);
}

/// 第 3 页：5 条（最后一页）
inline QString homeworkPage3()
{
    return homeworkPage(3, 25, 10, 80020);
}

/**
 * 生成一页作业数据。
 * @param pageNo  页码（从 1 开始）
 * @param total   服务端报告的总条数
 * @param perPage 每页条数
 * @param idBase  本页第一条的 id
 */
inline QString homeworkPage(int pageNo, int total, int perPage, int idBase)
{
    const int remaining = total - (pageNo - 1) * perPage;
    const int count = qBound(0, remaining, perPage);

    QString items;
    for (int i = 0; i < count; ++i) {
        if (i > 0) {
            items += QLatin1Char(',');
        }
        items += QStringLiteral(
                     R"({"id":%1,"title":"分页作业%2","deadline":"2026-10-08T15:59:00Z",)"
                     R"("type":"homework","submitted":false,"is_closed":false,)"
                     R"("data":{"description":"x"}})")
                     .arg(idBase + i)
                     .arg(idBase + i);
    }

    const int pages = (total + perPage - 1) / perPage;
    return QStringLiteral(R"({"homework_activities":[%1],"page":%2,"page_size":%3,)"
                          R"("pages":%4,"total":%5,"start":1,"end":%6})")
        .arg(items)
        .arg(pageNo)
        .arg(perPage)
        .arg(pages)
        .arg(total)
        .arg(count);
}

/// 时间格式非法（触发 FieldState::Invalid，而不是被当成空值）
inline QString homeworkActivitiesBadTime(){
    return QStringLiteral(R"JSON({
  "homework_activities": [
    {
      "id": 70001,
      "title": "测试作业一",
      "deadline": "not-a-timestamp",
      "type": "homework",
      "submitted": false,
      "is_closed": false,
      "data": { "description": "x", "publish_time": "also-bad" }
    }
  ],
  "page": 1, "page_size": 10, "pages": 1, "total": 1, "start": 1, "end": 1
})JSON");
}

/**
 * 考试接入（v4）样本：GET /api/courses/{id}/exam-list 的响应。
 *
 * 结构与字段的来源：站点自己的考试列表页 bundle（static/12308.f90daf57.js）
 * 对 exams[] 的读取（id/title/start_time/end_time/is_closed/is_started/
 * submission_count/makeup_exam_submission_count/score/final_score/data）。
 * 本账号当前 exams 为空，故样本值匿名化为测试值，仅保证结构一致。
 */

/// 空考试列表（大多数课程的现状）
inline QString examListEmpty()
{
    return QStringLiteral(
        R"({"exams":[],"total":0,"page":1,"pages":1,"page_size":50})");
}

/// 两场考试：一场已交并公布成绩，一场未开始
inline QString examListTwoExams()
{
    return QStringLiteral(R"JSON({
  "exams": [
    {
      "id": 81001,
      "title": "期中测试",
      "type": "exam",
      "start_time": "2026-10-01T02:00:00Z",
      "end_time": "2026-10-01T03:00:00Z",
      "is_closed": true,
      "is_started": true,
      "submission_count": 1,
      "makeup_exam_submission_count": 0,
      "score": "80",
      "final_score": "88.5",
      "data": { "description": "闭卷，允许带一张 A4 纸。" }
    },
    {
      "id": 81002,
      "title": "期末测试",
      "type": "exam",
      "start_time": "2026-12-20T06:00:00Z",
      "end_time": "2026-12-20T08:00:00Z",
      "is_closed": false,
      "is_started": false,
      "submission_count": 0,
      "makeup_exam_submission_count": 0,
      "score": null,
      "final_score": null
    }
  ],
  "total": 2, "page": 1, "pages": 1, "page_size": 50
})JSON");
}

} // namespace fixtures
} // namespace test
} // namespace njulms
