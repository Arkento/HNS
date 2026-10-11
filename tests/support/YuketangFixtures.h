#pragma once

#include <QJsonObject>
#include <QString>

namespace njulms {
namespace test {
namespace yuketang_fixtures {

/**
 * 雨课堂测试样本（结构与 2026-10-11 登录态实测一致，值全部匿名化）。
 *
 * ⚠️ 定义放在 .cpp（moc 不处理多行原始字符串，详见 docs/01）。JSON 由函数解析，
 * 因此这里直接返回结构化 QJsonObject，避免每个用例重复解析。
 */

/// 课程列表：POST/GET /v2/api/web/courses/list?identity=2 的响应
QJsonObject courseListTwoCourses();

/// 课程大纲：GET /mooc-api/v1/lms/learn/course/chapter 的响应
QJsonObject courseChapter();

/// 学习进度：GET /mooc-api/v1/lms/learn/course/schedule 的响应
QJsonObject courseSchedule();

} // namespace yuketang_fixtures
} // namespace test
} // namespace njulms
