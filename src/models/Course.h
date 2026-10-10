#pragma once

#include "FieldState.h"

#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QStringView>

#include <optional>

namespace njulms {

/**
 * 已知的数据来源键（写进 Course::source / courses.source）。
 *
 * 用途有两个：
 *   1. 课程快照的软删除**按来源隔离**——写学习通的快照绝不能把智汇南雍的课程
 *      标记为 present=0，反之亦然；
 *   2. 前端卡片上的"来源标签"直接使用这里的键。
 *
 * 用 constexpr QStringView 而不是 QString：避免头文件里的全局对象初始化顺序问题，
 * 调用点用 .toString() 显式转换，成本可忽略。
 */
namespace source_keys {

constexpr QStringView kNjuLms = u"nju-lms";      ///< 智汇南雍（南京大学 LMS）
constexpr QStringView kChaoxing = u"chaoxing";   ///< 学习通（超星）

} // namespace source_keys

/**
 * 课程。字段来源：POST /api/my-courses（实测 2026-10-09）。
 *
 * 实测该接口在 body 为 {} 时返回本人全部在修课程，包含 id / name / url /
 * course_code / department / instructors / semester / academic_year 等字段。
 * **不含** created_at / updated_at，因此本结构没有这两个字段。
 */
struct Course {
    /// 数据来源（多站点支持）：`nju-lms`（智汇南雍）/ `chaoxing`（学习通）。
    /// 作业与课程的"来源标签"、以及快照的软删除范围都以它为准。
    QString source;

    /// 站点侧课程 ID（实测 JSON 字段 "id"，整型）。作为主键，转为 QString 存储。
    QString courseId;
    QString courseName;

    /// 实测 JSON 字段 "url"，例如 https://lms.nju.edu.cn/course/11311/index
    QString detailUrl;

    /// 实测 JSON 字段 "course_code"
    QString courseCode;

    /// 实测 JSON 字段 "department".name（可能缺失）
    std::optional<QString> department;

    /// 实测 JSON 字段 "instructors"[].name（可能为空数组）
    QStringList instructors;

    /// 实测 JSON 字段 "semester".code，例如 "2026-1"
    std::optional<QString> semesterCode;

    /// 实测 JSON 字段 "academic_year".code，例如 "2026"
    std::optional<QString> academicYearCode;

    /// 本系统最后一次观察到该课程仍在本人课程列表中的时间（UTC）。
    /// 这是**采集时间**，不是站点提供的任何时间字段。
    QDateTime lastSeenAt;

    /// 该课程是否在本次快照中被观察到（用于课程列表变化的软删除标记）
    bool present = true;
};

} // namespace njulms
