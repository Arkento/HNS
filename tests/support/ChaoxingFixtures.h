#pragma once

#include <QString>

/**
 * 学习通（超星）HTML 样本（结构与边界情形见 .cpp 内注释）。
 *
 * ⚠️ 定义放在 ChaoxingFixtures.cpp 而不是本头文件：多行原始字符串字面量
 * 会破坏 moc 的解析（见 docs/01-实现决策.md「moc 与原始字符串字面量」一节），
 * 而 .cpp 不被 moc 处理，可以放心承载这些样本。
 */
namespace njulms {
namespace test {
namespace chaoxing_fixtures {

/// 课程中心（visit/interaction?s=…）→ 两门课程，每门两个链接
QString courseListTwoCourses();

/// 作业列表页（mooc2/work/list）→ 五种状态 + 一条无 workId 的坏条目 + 分页块
QString workListMixedStatuses();

/// 列表页改版后的模样：没有任何作业条目（解析器必须返回空 + ParseIssue，而不是猜）
QString workListTemplateChanged();

/// 权威空列表：页面明确写着「暂无作业」（解析器不应记 Missing，上层可安全软删旧数据）
QString workListEmptyState();

/// 考试列表页（两条：已完成走"查看答卷"，未完成走列表页兜底）
QString examListTwoItems();

/// 考试列表权威空状态：页面明确写着「暂无考试」
QString examListEmptyState();

} // namespace chaoxing_fixtures
} // namespace test
} // namespace njulms
