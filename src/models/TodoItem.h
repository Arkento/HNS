#pragma once

#include <QDateTime>
#include <QString>

namespace njulms {

/**
 * 待办条目（v2 新增；来自 `GET /api/todos` 的 `todo_list[]`，2026-10-09 实测）。
 *
 * 用途：作为「变更信号」。它一次请求就能给出**跨课程**的待办集合，
 * 于是增量同步可以先花 1 个请求判断"哪些课程可能变了"，
 * 再只对变化过的课程做完整（深）扫描，而不是每轮把所有课程都拉一遍。
 *
 * ⚠️ 覆盖范围有限：实测它只包含"待办/未完成"这类条目，
 * 历史作业、已提交、已封存的活动都不在其中，
 * 因此**不能**替代完整快照，只能用来减少深扫次数；
 * 完整对账仍然要按固定周期跑一次（见 Scheduler 的全量对账策略）。
 */
struct TodoItem
{
    QString courseId;    ///< 实测字段 course_id
    QString activityId;  ///< 实测字段 id（与作业的 activity id 同一套编号）
    QString type;        ///< 实测字段 type（本节只关心 "homework"）
    QDateTime endTime;   ///< 实测字段 end_time（站点给的截止时间，UTC；可能无效）

    /// 与解析器的去重键规则保持一致：{type}-{id}（见 AssignmentParser）
    QString sourceId() const
    {
        if (type.isEmpty() || activityId.isEmpty()) {
            return QString();
        }
        return type + QLatin1Char('-') + activityId;
    }
};

} // namespace njulms
