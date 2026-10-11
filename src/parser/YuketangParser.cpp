#include "parser/YuketangParser.h"

#include <QJsonArray>
#include <QJsonValue>
#include <QStringView>
#include <QTimeZone>

namespace njulms {
namespace {

/// 站点字段名（全部来自 2026-10-11 登录态实测样本；集中定义便于改版时一处修改）
namespace field {

constexpr QStringView kData = u"data";
constexpr QStringView kList = u"list";
constexpr QStringView kClassroomId = u"classroom_id";
constexpr QStringView kName = u"name";
constexpr QStringView kTeacher = u"teacher";
constexpr QStringView kTerm = u"term";
constexpr QStringView kCourse = u"course";
constexpr QStringView kUniversityId = u"university_id";
constexpr QStringView kPlatform = u"platform";

constexpr QStringView kCourseChapter = u"course_chapter";
constexpr QStringView kSectionLeafList = u"section_leaf_list";
constexpr QStringView kLeafList = u"leaf_list";
constexpr QStringView kLeafType = u"leaf_type";
constexpr QStringView kId = u"id";
constexpr QStringView kNodeId = u"node_id";
constexpr QStringView kScoreDeadline = u"score_deadline";
constexpr QStringView kStartTime = u"start_time";
constexpr QStringView kLeafSchedules = u"leaf_schedules";
constexpr QStringView kStatus = u"status";
constexpr QStringView kTotal = u"total";
constexpr QStringView kDone = u"done";
constexpr QStringView kScore = u"score";

} // namespace field

/// 数字或字符串 → 非空字符串（站点有的 id 给数字、有的给字符串）
QString idString(const QJsonValue &value)
{
    if (value.isString()) {
        return value.toString();
    }
    if (value.isDouble()) {
        return QString::number(static_cast<qint64>(value.toDouble()));
    }
    return QString();
}

/// 毫秒时间戳 → UTC 时间（<=0 或缺失视为无效）
QDateTime fromMs(const QJsonValue &value)
{
    const double ms = value.toDouble(0);
    if (!(ms > 0)) {
        return QDateTime();
    }
    return QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(ms), QTimeZone::UTC);
}

} // namespace

QString YuketangParser::sourceKey()
{
    return QStringLiteral("yuketang");
}

QList<Course> YuketangParser::parseCourseList(const QJsonObject &root, const QDateTime &observedAt,
                                              ParseIssues *issues)
{
    QList<Course> courses;

    const QJsonValue listValue = root.value(field::kData).toObject().value(field::kList);
    if (!listValue.isArray()) {
        if (issues != nullptr) {
            issues->append(ParseIssue(QStringLiteral("yuketang.courselist"),
                                      QStringLiteral("'data.list' array missing from response"),
                                      FieldState::Missing));
        }
        return courses;
    }

    const QJsonArray arr = listValue.toArray();
    courses.reserve(arr.size());
    for (const QJsonValue &value : arr) {
        const QJsonObject obj = value.toObject();
        const QString classroomId = idString(obj.value(field::kClassroomId));
        if (classroomId.isEmpty()) {
            if (issues != nullptr) {
                issues->append(ParseIssue(QStringLiteral("yuketang.course.classroom_id"),
                                          QStringLiteral("course without classroom_id skipped"),
                                          FieldState::Invalid));
            }
            continue;
        }

        Course course;
        course.source = sourceKey();
        course.courseId = classroomId;
        course.courseName = obj.value(field::kName).toString();
        if (course.courseName.isEmpty()) {
            course.courseName = obj.value(field::kCourse).toObject().value(field::kName).toString();
        }
        const QString teacher = obj.value(field::kTeacher).toObject().value(field::kName).toString();
        if (!teacher.isEmpty()) {
            course.instructors.append(teacher);
        }
        if (obj.value(field::kTerm).isDouble()) {
            course.semesterCode = QString::number(static_cast<qint64>(obj.value(field::kTerm).toDouble()));
        }

        // 详情链接：站点自己的学生课程页路由（实测进入课程后的地址形态）
        const QJsonObject courseObj = obj.value(field::kCourse).toObject();
        const QString universityId = idString(courseObj.value(field::kUniversityId));
        const QString platform = idString(courseObj.value(field::kPlatform));
        if (!universityId.isEmpty() && !platform.isEmpty()) {
            course.detailUrl = QStringLiteral(
                                   "https://www.yuketang.cn/v2/web/studentLog/%1"
                                   "?university_id=%2&platform_id=%3&classroom_id=%1&content_url=")
                                   .arg(classroomId, universityId, platform);
        }

        course.lastSeenAt = observedAt;
        courses.append(course);
    }
    return courses;
}

QList<Assignment> YuketangParser::parseCourseWork(const QJsonObject &chapterRoot,
                                                  const QJsonObject &scheduleRoot,
                                                  const QString &courseId, qint64 classroomId,
                                                  const QDateTime &observedAt,
                                                  ParseIssues *issues)
{
    QList<Assignment> items;

    const QJsonValue chaptersValue =
        chapterRoot.value(field::kData).toObject().value(field::kCourseChapter);
    if (!chaptersValue.isArray()) {
        if (issues != nullptr) {
            issues->append(ParseIssue(QStringLiteral("yuketang.chapter"),
                                      QStringLiteral("'data.course_chapter' array missing"),
                                      FieldState::Missing));
        }
        return items;
    }

    const QJsonObject schedules =
        scheduleRoot.value(field::kData).toObject().value(field::kLeafSchedules).toObject();
    if (schedules.isEmpty() && issues != nullptr) {
        issues->append(ParseIssue(QStringLiteral("yuketang.schedule"),
                                  QStringLiteral("'data.leaf_schedules' missing; statuses left unknown"),
                                  FieldState::Missing));
    }

    // 单个叶子的处理：只保留 作业(6) / 考试(5)
    const QJsonArray chapters = chaptersValue.toArray();
    const auto handleLeaf = [&](const QJsonObject &leaf) {
        const int leafType = leaf.value(field::kLeafType).toInt(-1);
        if (leafType != kLeafTypeExam && leafType != kLeafTypeHomework) {
            return;   // 视频/直播等其它类型不在采集范围
        }

                const QString leafId = idString(leaf.value(field::kId));
                const QString nodeId = idString(leaf.value(field::kNodeId));
                if (leafId.isEmpty()) {
                    if (issues != nullptr) {
                        issues->append(ParseIssue(QStringLiteral("yuketang.leaf.id"),
                                                  QStringLiteral("leaf without id skipped"),
                                                  FieldState::Invalid));
                    }
                    return;
                }

                Assignment item;
                item.courseId = courseId;
                item.kind = leafType == kLeafTypeExam ? QStringLiteral("exam")
                                                      : QStringLiteral("homework");
                item.sourceId = QStringLiteral("ykt-%1").arg(leafId);
                item.activityId = leafId;
                item.title = Observed<QString>::present(
                    leaf.value(field::kName).toString(QStringLiteral("（标题未获取）")));
                item.description = Observed<QString>::missing();

                // 时间：score_deadline(ms) → 截止；start_time(ms) → 发布
                const QDateTime due = fromMs(leaf.value(field::kScoreDeadline));
                item.dueAt = due.isValid() ? Observed<QDateTime>::present(due)
                                           : Observed<QDateTime>::missing();
                const QDateTime start = fromMs(leaf.value(field::kStartTime));
                item.publishedAt = start.isValid() ? Observed<QDateTime>::present(start)
                                                   : Observed<QDateTime>::missing();

                // 进度/状态：leaf_schedules[leafId] 可能是
                //   - 数字（视频类进度 0~1；作业/考试实测多为对象）
                //   - 对象 {status,total,done,score}（status=5 实测为已完成）
                const QJsonValue entry = schedules.value(leafId);
                bool submitted = false;
                bool haveProgress = false;
                if (entry.isDouble()) {
                    haveProgress = true;
                    submitted = entry.toDouble() >= 1.0;
                } else if (entry.isObject()) {
                    haveProgress = true;
                    const QJsonObject progress = entry.toObject();
                    const double total = progress.value(field::kTotal).toDouble(0);
                    const double done = progress.value(field::kDone).toDouble(0);
                    const int status = progress.value(field::kStatus).toInt(-1);
                    submitted = (status == 5) || (total > 0 && done >= total);
                    if (progress.contains(field::kScore) && done > 0) {
                        item.score = Observed<double>::present(
                            progress.value(field::kScore).toDouble());
                    } else {
                        item.score = Observed<double>::missing();
                    }
                }
                if (!item.score.hasValue()) {
                    item.score = Observed<double>::missing();
                }

                if (haveProgress) {
                    item.submitted = Observed<bool>::present(submitted);
                    if (submitted) {
                        item.status = Observed<AssignmentStatus>::present(
                            AssignmentStatus::Submitted);
                    } else if (item.dueAt.hasValue()) {
                        item.status = Observed<AssignmentStatus>::present(
                            item.dueAt.require() < observedAt ? AssignmentStatus::Overdue
                                                              : AssignmentStatus::Pending);
                    } else {
                        // 没有截止时间又未完成：按"未提交"处理（排序时自动排到最后）
                        item.status = Observed<AssignmentStatus>::present(
                            AssignmentStatus::Pending);
                    }
                } else {
                    // 大纲里有这个 leaf、但进度里没有：不猜测，留 Unknown 由上层如实呈现
                    item.submitted = Observed<bool>::missing();
                    item.status = Observed<AssignmentStatus>::missing();
                }

                // 详情链接（实测模板来自 todo_list.link_url）：
                //   https://www.yuketang.cn/ai-workspace/lms-graph/{classroom}/{quiz|exercise}/{leaf}?node_id=…
                if (!nodeId.isEmpty() && classroomId > 0) {
                    const QString kindPath = leafType == kLeafTypeExam ? QStringLiteral("quiz")
                                                                       : QStringLiteral("exercise");
                    item.detailUrl = Observed<QString>::present(
                        QStringLiteral("https://www.yuketang.cn/ai-workspace/lms-graph/%1/%2/%3"
                                       "?node_id=%4")
                            .arg(classroomId)
                            .arg(kindPath, leafId, nodeId));
                } else {
                    item.detailUrl = Observed<QString>::missing();
                }

        item.observedAt = observedAt;
        items.append(item);
    };

    // 实测（2026-10-11）：course_chapter 里两种形态混用，必须都认——
    //   A. section_leaf_list 的元素本身就是 leaf（自带 leaf_type）：
    //      作业/考试多数在这种"未分类教学活动 / L1 …"章节里；
    //   B. section_leaf_list 的元素是"节"，真正的 leaf 在它的 leaf_list 里。
    // 只认其中一种会漏掉全部作业与考试（曾经的真机事故）。
    for (const QJsonValue &chapterValue : chapters) {
        const QJsonArray entries =
            chapterValue.toObject().value(field::kSectionLeafList).toArray();
        for (const QJsonValue &entryValue : entries) {
            const QJsonObject entry = entryValue.toObject();
            if (entry.value(field::kLeafList).isArray()) {
                const QJsonArray leaves = entry.value(field::kLeafList).toArray();
                for (const QJsonValue &leafValue : leaves) {
                    handleLeaf(leafValue.toObject());
                }
            } else if (entry.contains(field::kLeafType)) {
                handleLeaf(entry);
            }
        }
    }

    if (items.isEmpty() && issues != nullptr) {
        // 树里没有任何 作业/考试 叶子：可能是改版，也可能这门课确实没有。
        // 记一条 Missing（不写 Invalid），让上层日志看得到"为什么是 0 条"。
        issues->append(ParseIssue(QStringLiteral("yuketang.chapter"),
                                  QStringLiteral("no 作业/考试 leaves found in chapter tree"),
                                  FieldState::Missing));
    }
    return items;
}

} // namespace njulms
