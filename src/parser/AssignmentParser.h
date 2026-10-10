#pragma once

#include "models/Assignment.h"
#include "models/Course.h"
#include "models/FieldState.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringView>

namespace njulms {

/**
 * 解析结果封装：数据 + 会话有效性判定。
 *
 * 需求场景 C 的关键：**不能**把「会话失效返回登录页」误判为「空的作业列表」。
 * 因此解析入口必须先区分「这是不是一个可用的 JSON 响应」，再谈字段。
 */
struct ParseOutcome
{
    enum class Kind {
        Ok,             ///< 正常 JSON 响应
        NotJson,        ///< 响应体不是 JSON（极可能是登录页 HTML）→ 提示重新登录
        UnexpectedShape ///< 是 JSON 但结构不符合预期 → 记录解析错误，不写入空数据
    };

    Kind kind = Kind::Ok;
    QString detail;  ///< 供日志使用的诊断信息（不含正文）

    bool ok() const noexcept { return kind == Kind::Ok; }
    bool needsLogin() const noexcept { return kind == Kind::NotJson; }
};

/**
 * 智汇南雍页面数据解析器。
 *
 * 职责边界（对应需求文档第三节第 3 点）：
 *   - 只负责 JSON → 统一数据模型 的转换
 *   - **不**访问网络、**不**触碰数据库、**不**做调度
 *
 * 所有字段名均来自 2026-10-09 的本机实测（见 v1/docs/00-可行性评估.md）。
 * 未经实测的字段一律不做假设，缺失时标记为 Missing 而不是猜测。
 */
class AssignmentParser
{
public:
    AssignmentParser() = default;

    // ---- 会话与信封校验 -------------------------------------------------------

    /// 判定响应体是否为 JSON 对象。非 JSON（如登录页 HTML）返回 NotJson。
    static ParseOutcome inspectEnvelope(const QString &body);

    // ---- 课程 ----------------------------------------------------------------

    /**
     * 解析 POST /api/my-courses 的响应。
     *
     * 实测该接口在 body 为 {} 时返回 {"courses":[...]}，课程对象含
     * id / name / url / course_code / department / instructors / semester / academic_year。
     * 实测**不含** created_at / updated_at。
     *
     * @param issues 输出：字段级问题
     */
    static QList<Course> parseCourseList(const QJsonObject &root,
                                         const QDateTime &observedAt,
                                         ParseIssues *issues = nullptr);

    /// 解析单个课程对象
    static Course parseCourse(const QJsonObject &obj,
                              const QDateTime &observedAt,
                              ParseIssues *issues = nullptr);

    // ---- 作业 ----------------------------------------------------------------

    /**
     * 解析 GET /api/courses/{id}/homework-activities 或 /activities 的响应。
     *
     * 两个接口的**字段集不同**（实测）：
     *   - homework-activities 额外含 submitted / score / user_submit_count / submitted_status
     *   - activities 含更多顶层标志
     * 因此本函数做字段并集解析，缺失的字段标记为 Missing 而非 Absent。
     *
     * @param courseId  所属课程 ID
     * @param items     作业对象数组
     * @param observedAt 本次采集时刻（UTC），仅写入 observedAt 字段
     * @param issues    输出：字段级问题
     */
    static QList<Assignment> parseAssignments(const QString &courseId,
                                              const QJsonArray &items,
                                              const QDateTime &observedAt,
                                              ParseIssues *issues = nullptr,
                                              const QString &kind = QStringLiteral("homework"));

    /// 解析单条作业
    static Assignment parseAssignment(const QString &courseId,
                                     const QJsonObject &obj,
                                     const QDateTime &observedAt,
                                     ParseIssues *issues = nullptr,
                                     const QString &kind = QStringLiteral("homework"));

    // ---- 考试 ----------------------------------------------------------------

    /**
     * 考试列表解析结果。
     *
     * `ok == false` 表示响应信封里没有 exams 数组（结构不符或未登录形态），
     * 调用方必须把它当成"这次没拿到考试列表"，而不是"这门课没有考试"。
     */
    struct ExamListOutcome
    {
        bool ok = false;
        QList<Assignment> exams;
        ParseIssues issues;
    };

    /**
     * 解析 GET /api/courses/{id}/exam-list 的响应（考试接入）。
     *
     * 接口与字段来源（2026-10-10 实测，证据为该站点自己的前端 bundle
     * static/12308.f90daf57.js 对 exams[] 的读取）：
     *   - 信封：{"exams":[…], "total":n, "page":…, "pages":…, "page_size":…}
     *   - 条目：id / title / start_time / end_time / is_closed / is_started /
     *           submission_count / makeup_exam_submission_count / score / final_score /
     *           make_up_record / data.description
     * 本账号当前（2026-10-10）所有课程 exams 均为空，因此条目字段属
     * 「前端代码实测 + 防御式读取」：解析不到就保持 Missing，绝不猜测。
     */
    static ExamListOutcome parseExamList(const QJsonObject &root, const QString &courseId,
                                         const QDateTime &observedAt);

    /// 解析单条考试
    static Assignment parseExam(const QString &courseId, const QJsonObject &obj,
                                const QDateTime &observedAt, ParseIssues *issues = nullptr);

    // ---- 内容摘要 -------------------------------------------------------------

    /**
     * 计算内容摘要。
     *
     * 只使用**权威**字段（Present 或 Absent）。理由：若把 Missing/Invalid 也纳入，
     * 某次解析失败会让摘要变化，从而产生一次无意义的「内容已变化」记录。
     *
     * 同时输出可读的规范串 signature（不含作业正文），便于测试断言与排查。
     */
    static QString computeContentSignature(const Assignment &a);

private:
    /// 从 JSON 值提取字符串字段，区分 Absent / Missing / Invalid
    static Observed<QString> readString(const QJsonObject &obj, QStringView key,
                                        QStringView fieldLabel, ParseIssues *issues);

    /// 从 JSON 值提取时间字段
    static Observed<QDateTime> readTime(const QJsonObject &obj, QStringView key,
                                        QStringView fieldLabel, ParseIssues *issues);

    /// 从 JSON 值提取布尔字段
    static Observed<bool> readBool(const QJsonObject &obj, QStringView key,
                                   QStringView fieldLabel, ParseIssues *issues);

    /// 从 JSON 值提取数值字段（站点有时给字符串 "100"，有时给数字 100）
    static Observed<double> readNumber(const QJsonObject &obj, QStringView key,
                                       QStringView fieldLabel, ParseIssues *issues);
};

} // namespace njulms
