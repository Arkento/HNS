#pragma once

#include "models/Assignment.h"
#include "models/Course.h"
#include "parser/AssignmentParser.h"

#include <QDateTime>
#include <QList>
#include <QString>

namespace njulms {

/**
 * 学习通（i.chaoxing.com）的 HTML 解析。
 *
 * 为什么是 HTML 解析：实测学习通**没有可用的 JSON 接口** ——
 * 课程列表页与作业列表页都是服务端渲染的 HTML（见 docs/06-学习通探测.md）。
 * 所以这里针对**实测到的模板**做定向提取，而不是写通用 HTML 解析器。
 *
 * 模板一旦改版：提取不到就返回空列表 / 缺失状态（`Observed<T>` 的三态），
 * 由上层如实呈现（"本次没解析到"而不是"没有作业"）——绝不猜测。
 */
class ChaoxingParser
{
public:
    /// 数据来源键（写进 courses.source）
    static QString sourceKey();

    /**
     * 课程列表页（`mooc1-1.chaoxing.com/visit/interaction?s=…`）→ 课程。
     *
     * 课程卡片里的链接形如
     * `…/visit/stucoursemiddle?courseid=…&clazzid=…&cpi=…&ismooc2=1&v=2`，
     * 课程名在同一张卡片的链接文本里（实测）。因为没有现成的 course id 字段，
     * 这里用 `courseid-clazzid` 作为本地主键（同一门课的不同教学班要分开）。
     */
    static QList<Course> parseCourseList(const QString &html, const QDateTime &observedAt,
                                         ParseIssues *issues = nullptr);

    /**
     * 作业列表页（`mooc1.chaoxing.com/mooc2/work/list?…`）→ 作业。
     *
     * 每一条形如：
     * ```
     * <li data="…/mooc2/work/task?…&workId=…&enc=…" aria-label="标题 ; 状态">
     *   <p class="overHidden2">标题</p><p class="status">状态</p>
     *   <div class="time notOver">剩余101小时4分钟</div>
     * </li>
     * ```
     * 截止时间站点只给"剩余多久"的相对值，这里按 `observedAt + 剩余` 折算成绝对时间
     * （并在注释里标明是折算值）；折算失败就记为缺失。
     */
    static QList<Assignment> parseWorkList(const QString &html, const QString &courseId,
                                           const QDateTime &observedAt,
                                           ParseIssues *issues = nullptr);

    /**
     * 考试列表页（`mooc1.chaoxing.com/exam-ans/mooc2/exam/exam-list?…`）→ 考试。
     *
     * 实测模板（2026-10-10，两门课程各含一条已完成考试）：
     * ```
     * <li>
     *   <div … role="option" onclick="viewExamAnswer('10953874','171363707')">
     *     <p class="overHidden2 fl">Level A Vocab. Quiz 1</p>
     *     <p class="status fl">已完成</p>
     *   </div>
     * </li>
     * ```
     * 详情链接：
     *   - 已完成（viewExamAnswer）→ 拼接"查看答卷"页（路径来自该页 viewExamAnswer 函数源码）；
     *   - 其它状态 → 退回考试列表页本身（进入考试的地址由加密参数生成，不猜）。
     *
     * @param examListUrl 考试列表页完整 URL（含 courseid/clazzid/cpi 参数，用于构造链接）
     */
    static QList<Assignment> parseExamList(const QString &html, const QString &courseId,
                                           const QString &examListUrl,
                                           const QDateTime &observedAt,
                                           ParseIssues *issues = nullptr);
};

} // namespace njulms
