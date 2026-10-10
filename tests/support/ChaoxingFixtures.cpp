#include "support/ChaoxingFixtures.h"

/**
 * 学习通（超星）HTML 样本。
 *
 * ── 来源与真实性声明 ──────────────────────────────────────────────────────
 * 结构与边界情形来自 2026-10-10 对本机账号的真实抓取（见 docs/06-学习通探测.md），
 * 但**所有 ID、课程名、作业标题都已替换为测试值**（111111111 / 测试课程甲 / 单元一…），
 * 不包含任何真实个人信息。域名与 URL 形状保留真实结构，因为那正是解析器要面对的输入。
 *
 * ── 覆盖的实测边界情形 ────────────────────────────────────────────────────
 *   - 同一张课程卡片有两个链接（封面无文本 + 标题带文本）→ 解析必须去重且取到标题
 *   - 作业条目 aria-label = "标题 ; 状态"，标题含 HTML 实体（&amp;）
 *   - 状态：已完成 / 待批阅 / 未完成
 *   - 时间：剩余X天X小时 / 剩余X小时X分钟 / 剩余X分钟 / 已过期（class 含 over）/ 无时间块
 *   - 缺少 workId 的条目（改版残留）→ 解析应跳过并记 ParseIssue
 *   - 与作业无关的 <li>（分页块）→ 应被自然忽略
 */

namespace njulms {
namespace test {
namespace chaoxing_fixtures {

QString courseListTwoCourses()
{
    return QStringLiteral(R"HTML(
<div class="Mconright">
  <div class="course-info">
    <a href="https://mooc1-1.chaoxing.com/visit/stucoursemiddle?courseid=111111111&amp;clazzid=222222222&amp;cpi=333333333&amp;ismooc2=1&amp;v=2" target="_blank" class="courseCover"><img src="/static/course.png" alt=""></a>
    <a href="https://mooc1-1.chaoxing.com/visit/stucoursemiddle?courseid=111111111&amp;clazzid=222222222&amp;cpi=333333333&amp;ismooc2=1&amp;v=2" target="_blank" class="courseName">测试课程甲</a>
  </div>
  <div class="course-info">
    <a href="https://mooc1-1.chaoxing.com/visit/stucoursemiddle?courseid=444444444&amp;clazzid=555555555&amp;cpi=333333333&amp;ismooc2=1&amp;v=2" target="_blank" class="courseCover"><img src="/static/course.png" alt=""></a>
    <a href="https://mooc1-1.chaoxing.com/visit/stucoursemiddle?courseid=444444444&amp;clazzid=555555555&amp;cpi=333333333&amp;ismooc2=1&amp;v=2" target="_blank" class="courseName">测试课程乙 &amp; 实践</a>
  </div>
</div>
)HTML");
}

QString workListMixedStatuses()
{
    return QStringLiteral(R"HTML(
<div class="work-list">
<ul>
<li onclick="goTask(this);" data="https://mooc1.chaoxing.com/mooc-ans/mooc2/work/task?courseId=111111111&amp;classId=222222222&amp;cpi=333333333&amp;ut=s&amp;workId=900001&amp;answerId=800001&amp;enc=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" aria-label="单元一 阅读理解 ; 已完成">
  <p class="overHidden2 fl">单元一 阅读理解</p>
  <p class="status fl">已完成</p>
  <div class="time notOver">剩余101小时4分钟</div>
</li>
<li onclick="goTask(this);" data="https://mooc1.chaoxing.com/mooc-ans/mooc2/work/task?courseId=111111111&amp;classId=222222222&amp;cpi=333333333&amp;ut=s&amp;workId=900002&amp;answerId=800002&amp;enc=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" aria-label="单元二 写作 ; 待批阅">
  <p class="overHidden2 fl">单元二 写作</p>
  <p class="status fl">待批阅</p>
</li>
<li onclick="goTask(this);" data="https://mooc1.chaoxing.com/mooc-ans/mooc2/work/task?courseId=111111111&amp;classId=222222222&amp;cpi=333333333&amp;ut=s&amp;workId=900003&amp;answerId=800003&amp;enc=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" aria-label="单元三 听力 &amp; 口语 ; 未完成">
  <p class="overHidden2 fl">单元三 听力 &amp; 口语</p>
  <p class="status fl">未完成</p>
  <div class="time notOver">剩余3天2小时</div>
</li>
<li onclick="goTask(this);" data="https://mooc1.chaoxing.com/mooc-ans/mooc2/work/task?courseId=111111111&amp;classId=222222222&amp;cpi=333333333&amp;ut=s&amp;workId=900004&amp;answerId=800004&amp;enc=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" aria-label="单元四 报告 ; 未完成">
  <p class="overHidden2 fl">单元四 报告</p>
  <p class="status fl">未完成</p>
  <div class="time over">已过期</div>
</li>
<li onclick="goTask(this);" data="https://mooc1.chaoxing.com/mooc-ans/mooc2/work/task?courseId=111111111&amp;classId=222222222&amp;cpi=333333333&amp;ut=s&amp;workId=900005&amp;answerId=800005&amp;enc=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" aria-label="单元五 小测 ; 未完成">
  <p class="overHidden2 fl">单元五 小测</p>
  <p class="status fl">未完成</p>
  <div class="time notOver">剩余45分钟</div>
</li>
<li data="https://mooc1.chaoxing.com/mooc-ans/mooc2/work/task?courseId=111111111&amp;enc=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" aria-label="改版残留条目 ; 未完成">
  <p class="overHidden2 fl">改版残留条目</p>
</li>
<li class="pageDivider"><div class="pageDiv" id="page"><a href="javascript:;">1</a></div></li>
</ul>
</div>
)HTML");
}

QString workListTemplateChanged()
{
    return QStringLiteral(R"HTML(
<div class="new-template">
  <div class="widgets-empty">暂无内容</div>
</div>
)HTML");
}

} // namespace chaoxing_fixtures
} // namespace test
} // namespace njulms
