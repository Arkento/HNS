#pragma once

#include "models/Assignment.h"
#include "models/Course.h"
#include "parser/AssignmentParser.h"

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>

namespace njulms {

/**
 * 雨课堂（yuketang.cn）JSON 解析。
 *
 * 与学习通不同，雨课堂的学生侧数据是**结构化 JSON**（探测报告 docs/07 + 2026-10-11
 * 登录态实测），因此这里不需要任何 HTML 模板解析，全部按字段读取：
 *
 *   课程列表：GET /v2/api/web/courses/list?identity=2
 *             → data.list[]（classroom_id / name / teacher.name / term / course.*）
 *   作业/考试：GET /mooc-api/v1/lms/learn/course/chapter?…（课程大纲树）
 *             + GET /mooc-api/v1/lms/learn/course/schedule?…（每个 leaf 的完成状态）
 *             → 只保留 leaf_type 5（考试）与 6（作业），其余类型（视频/直播等）忽略
 *
 * 所有请求都需要自定义头 `xtbz: ykt`（实测缺它会返回 "XTBZ IS REQUIRED"）。
 *
 * 字段缺失/结构不符时的原则与其它解析器一致：**如实记录（Observed 三态 / ParseIssue），
 * 绝不猜测**；只有确认能证明完整的快照才允许上层做软删除。
 */
class YuketangParser
{
public:
    /// 数据来源键（写进 courses.source）
    static QString sourceKey();

    /// leaf_type：考试 / 作业（实测：预习检测=5、课后作业=6、视频=0）
    static constexpr int kLeafTypeExam = 5;
    static constexpr int kLeafTypeHomework = 6;

    /// 解析课程列表响应；结构不符时返回空列表并记 ParseIssue
    static QList<Course> parseCourseList(const QJsonObject &root, const QDateTime &observedAt,
                                         ParseIssues *issues = nullptr);

    /**
     * 合并「课程大纲（chapter）」与「学习进度（schedule）」→ 作业/考试条目。
     *
     * @param chapterRoot  chapter 接口响应
     * @param scheduleRoot schedule 接口响应（可为空对象：此时状态记 Missing）
     * @param courseId     本地课程主键（classroom_id 的字符串形式）
     * @param classroomId  站点侧 classroom_id（用于拼详情链接）
     */
    static QList<Assignment> parseCourseWork(const QJsonObject &chapterRoot,
                                             const QJsonObject &scheduleRoot,
                                             const QString &courseId, qint64 classroomId,
                                             const QDateTime &observedAt,
                                             ParseIssues *issues = nullptr);
};

} // namespace njulms
