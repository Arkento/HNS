#pragma once

#include "FieldState.h"

#include <QDateTime>
#include <QString>
#include <QStringList>

#include <optional>
#include <utility>

namespace njulms {

/**
 * 一个「已观察的字段」：把值与其可信状态绑在一个对象里。
 *
 * 设计意图：把需求场景 F（字段突然解析不到时不得用空值覆盖旧值）变成**类型层面的约束**，
 * 而不是靠调用方自觉。构造函数私有，只能经四个工厂函数创建，因此不存在
 * 「有值但状态是 Missing」这类自相矛盾的组合。
 *
 * 用法：
 *     Observed<QString> t = Observed<QString>::present(QStringLiteral("第一次作业"));
 *     Observed<QDateTime> d = Observed<QDateTime>::absent();   // 源数据明确为 null
 *     Observed<QDateTime> u = Observed<QDateTime>::missing();  // 本次响应没有该 key
 */
template <typename T>
class Observed
{
public:
    /**
     * 默认构造 = Missing（未加载）。
     *
     * 这是唯一安全的默认值：Missing 不是权威结论，因此不会被写入数据库，
     * 也就不会用「什么都没解析到」去覆盖库中的有效值。
     * 若默认成 Absent（确认空），一个漏赋值的字段就会静默清空已有数据。
     */
    Observed() = default;

    /// 字段确实为空（源数据明确 null / 空串）。允许写入 NULL。
    static Observed absent() { return Observed(FieldState::Absent, std::nullopt); }

    /// 字段未加载（响应缺 key / 被截断）。不得覆盖库中原值。
    static Observed missing() { return Observed(FieldState::Missing, std::nullopt); }

    /// 字段存在但解析失败。不得覆盖库中原值，并应记入解析异常。
    static Observed invalid() { return Observed(FieldState::Invalid, std::nullopt); }

    /// 成功解析出值。
    static Observed present(T value)
    {
        return Observed(FieldState::Present, std::optional<T>(std::move(value)));
    }

    FieldState state() const noexcept { return m_state; }

    /// 是否解析出值
    bool hasValue() const noexcept { return m_value.has_value(); }

    /// 该字段本次是否给出权威结论（Present 或 Absent）——决定能否写入数据库
    bool isAuthoritative() const noexcept { return njulms::isAuthoritative(m_state); }

    const std::optional<T> &value() const noexcept { return m_value; }

    /// 仅在 hasValue() 为真时调用
    const T &require() const { return m_value.value(); }

    /// 取原始 optional，便于数据库绑定
    std::optional<T> optional() const noexcept { return m_value; }

private:
    Observed(FieldState state, std::optional<T> value)
        : m_state(state), m_value(std::move(value))
    {
    }

    FieldState m_state = FieldState::Missing;
    std::optional<T> m_value;
};

/**
 * 作业状态。
 *
 * ⚠️ 重要：站点**没有**一个既有的、原始的状态字符串字段被本项目验证过。
 * 实测 /api/courses/{id}/homework-activities 提供的是若干布尔/数值标志
 * （submitted、is_closed、is_started、score），本枚举是**由这些标志推导**出来的，
 * 不是站点直接给出的字段。推导规则集中在 AssignmentParser 内，并在 UI 上以
 * 「推导」标注，避免被误当成站点权威字段。
 */
enum class AssignmentStatus {
    Unknown,    ///< 关键标志缺失，无法判定（对应 FieldState::Missing/Invalid）
    Pending,    ///< 未提交、未截止
    Submitted,  ///< 已提交
    Overdue,    ///< 未提交且已截止
    Closed,     ///< 已关闭但无法判定提交情况
};

const char *assignmentStatusKey(AssignmentStatus s) noexcept;
QString assignmentStatusDisplay(AssignmentStatus s);

/**
 * 条目类型显示名：homework → 作业；exam → 考试；其它值原样返回。
 *
 * 类型值来自各站点自身（南大：homework / exam；学习通：作业 / 考试列表各自一个采集通道），
 * 数据库里存文本，便于以后扩展（测验等）而不需要迁移。
 */
QString assignmentKindDisplay(const QString &kind);

/// 从存储键还原状态；无法识别时返回 nullopt（调用方应视为解析失败而非 Unknown）
std::optional<AssignmentStatus> assignmentStatusFromKey(const QString &key);

/**
 * 作业 / 课程任务。
 *
 * 字段来源（实测 2026-10-09，见 v1/docs/00-可行性评估.md 第 2.3/2.4 节）：
 *   - GET /api/courses/{id}/homework-activities  → title, deadline, unique_key,
 *     updated_at, data.publish_time, submitted, score, is_closed, is_started
 *   - GET /api/courses/{id}/activities           → 同上但**没有** submitted / score
 *
 * 因此 Parser 必须做「字段并集 + 缺省容错」，不能假定某个接口一定给某个字段。
 */
struct Assignment {
    // ---- 身份 -----------------------------------------------------------------
    QString courseId;   ///< 所属课程 ID（数据库外键）
    QString sourceId;   ///< 站点侧稳定标识，来自实测字段 "unique_key"，例如 "homework-43604"
    QString activityId; ///< 实测字段 "id"（整型），转为 QString；用于拼接详情链接与详情接口

    /// 条目类型：homework（作业）/ exam（考试）。未知值原样保留，显示见 assignmentKindDisplay()。
    QString kind = QStringLiteral("homework");

    // ---- 内容字段（三态）------------------------------------------------------
    Observed<QString> title;
    Observed<QString> description;   ///< 实测 data.description；可能是 HTML 片段，按原文保存
    Observed<QString> detailUrl;     ///< 由 courseId + activityId 推导（实测无现成字段）

    // ---- 时间字段（三态，均为 UTC）-------------------------------------------
    /// 实测 data.publish_time。⚠️ 实测该字段**经常缺失**（课程 11311 的作业 44332 无此字段），
    /// 因此绝不能用采集时间兜底。
    Observed<QDateTime> publishedAt;

    /// 实测顶层 "deadline"（与 "end_time" 实测相同）。
    Observed<QDateTime> dueAt;

    // ---- 状态字段（三态）------------------------------------------------------
    /// 由站点标志推导，见 AssignmentStatus 注释
    Observed<AssignmentStatus> status;

    /// 实测布尔字段 "submitted"
    Observed<bool> submitted;

    /// 实测字段 "score"（可能是字符串 "100"、数字、或 null）
    Observed<double> score;

    // ---- 系统字段 -------------------------------------------------------------
    /// 内容摘要：仅由**权威**字段（Present/Absent）规范化后计算，用于判断作业是否发生变化
    ///
    /// 该记录当前是否存在于站点（软删除标志）：false = 本轮快照里没有它，
    /// 仅作为历史保留、界面不再显示。注意与「字段级 Missing」区分：
    /// present 是整条记录的在场性，Missing 是单个字段本轮没拿到。
    bool present = true;

    /// 用户手动软删除（iPad 前端「未提交 → 删除」）：
    /// 与同步的 present 完全独立——present 表示"站点上还在不在"，
    /// userDeleted 表示"用户还想不想看到它"。同步（upsert）不触碰这两个字段，
    /// 因此用户删掉的条目在后续同步里不会复活；恢复只能由用户显式操作。
    bool userDeleted = false;

    /// 用户执行软删除的时间（UTC）；userDeleted=false 时无意义
    std::optional<QDateTime> userDeletedAt;

    QString contentHash;

    /// 计算 contentHash 时采用的规范化输入摘要（便于调试与测试断言，不含正文）
    QString contentSignature;

    /// 本次观察到该记录的时间（采集时间，UTC）
    QDateTime observedAt;

    /// 首次进入本库的时间（UTC），由 Repository 维护
    QDateTime firstSeenAt;

    /// 最后一次在快照中观察到的时间（UTC），由 Repository 维护
    QDateTime lastSeenAt;

    /// 本次解析发现的字段级问题
    ParseIssues issues;
};

} // namespace njulms
