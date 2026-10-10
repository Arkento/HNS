#pragma once

#include "FieldState.h"

#include <QDateTime>
#include <QString>
#include <QStringList>

#include <optional>

namespace njulms {

/**
 * 课程。字段来源：POST /api/my-courses（实测 2026-10-09）。
 *
 * 实测该接口在 body 为 {} 时返回本人全部在修课程，包含 id / name / url /
 * course_code / department / instructors / semester / academic_year 等字段。
 * **不含** created_at / updated_at，因此本结构没有这两个字段。
 */
struct Course {
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
