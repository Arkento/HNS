#pragma once

#include "IPageBridge.h"
#include "models/Assignment.h"
#include "models/Course.h"
#include "models/TodoItem.h"

#include <QDateTime>
#include <QList>
#include <QObject>
#include <QString>

#include <functional>
#include <memory>

namespace njulms {

/**
 * 智汇南雍（TronClass）网站适配器。
 *
 * 职责边界（需求文档第三节第 2 点）：
 *   - 封装**全部**网站专属逻辑：路径、参数、字段语义
 *   - **不**修改数据库，**不**做定时调度，**不**解析页面 DOM
 *
 * ── 关于数据获取方式的重要说明 ────────────────────────────────────────────
 * 原开发指令假定需要「分析真实 DOM 并实现页面解析器」。本机实测表明该路线不适用：
 *   1. 站点是 SPA，数据由 JS 渲染；
 *   2. 站点会检测 DevTools 并**清空页面渲染**（既有抓包证据：控制台报
 *      "You don't have permission to use DEVTOOL!"，截图全为空白、文本提取为 0 字节）；
 *   3. 站点自身的 JSON 接口不受该检测影响，且返回结构稳定。
 * 因此本适配器改为「在已登录页面上下文里执行同源 fetch，拿 JSON 交给 Parser」。
 * 这同时取消了引入 HTML 解析库的必要。
 *
 * ── 关于接口来源的严格约定 ────────────────────────────────────────────────
 * 本文件中每一个路径与字段名都标注了来源：
 *   [实测] = 2026-10-09 在本机带会话实测确认（见 v1/docs/00-可行性评估.md）
 *   [推导] = 由实测信息推导，**尚未实测**，必须验证后才可依赖
 * 不允许出现既非实测也非推导标记第三类内容。
 */
class NjuLmsAdapter : public QObject
{
    Q_OBJECT

public:
    /// 适配器错误分类。区分它们是为了让上层能正确决定「暂停等登录」还是「重试」。
    enum class ErrorCode {
        None,
        SessionExpired,   ///< 响应不是 JSON，极可能是登录页 → 必须暂停采集并提示重新登录
        UnexpectedShape,  ///< 是 JSON 但结构不符 → 记录解析错误，**不得**当作空列表
        Transport,        ///< JS 执行 / 网络失败 → 可重试
        HttpStatus,       ///< 收到非 2xx 状态码
        Unsupported,      ///< 调用了尚未验证的功能（例如作业详情接口）
    };

    struct Error
    {
        ErrorCode code = ErrorCode::None;
        int httpStatus = 0;
        QString message;

        bool ok() const noexcept { return code == ErrorCode::None; }
        /// 是否属于「需要人工重新登录」的错误
        bool needsLogin() const noexcept { return code == ErrorCode::SessionExpired; }
        /// 是否属于可重试错误
        bool retryable() const noexcept
        {
            return code == ErrorCode::Transport || code == ErrorCode::HttpStatus;
        }

        static Error none() { return {}; }
        static Error make(ErrorCode c, QString message, int status = 0)
        {
            return Error{c, status, std::move(message)};
        }
    };

    /// 会话检查结果
    struct SessionCheck
    {
        Error error;
        /// 会话是否有效。**仅当**成功取得可解析的课程列表 JSON 时为 true。
        bool authenticated = false;
        int courseCount = 0;
    };

    struct CourseListResult
    {
        Error error;
        QList<Course> courses;
    };

    struct AssignmentListResult
    {
        Error error;
        QList<Assignment> assignments;
        ParseIssues issues;
        /// 本次快照是否可证明完整（用于决定断点能否推进，需求第四节场景 H）
        bool snapshotComplete = false;
        /// 服务端报告的条目总数（实测字段 "total"），用于完整性校验
        int reportedTotal = -1;
        /// 是否触发了翻页
        bool paginated = false;
    };

    /// 考试列表结果（考试接入，2026-10-10）。
    struct ExamListResult
    {
        Error error;
        QList<Assignment> exams;
        ParseIssues issues;
        /// 服务端给了 total 且条数与之一致时才算完整
        bool snapshotComplete = false;
        int reportedTotal = -1;
    };

    /// 课程任务（作业 + 测试等）结果。当前仅覆盖 homework 类型条目。
    struct CourseTaskResult
    {
        Error error;
        QList<Assignment> tasks;
        ParseIssues issues;
        /// ⚠️ 实测该接口不返回任何分页元数据，因此**无法证明**集合完整，
        /// 本字段恒为 false，断点不得据此推进。
        bool snapshotComplete = false;
        /// 已解析的 homework 条目数（便于与作业列表接口交叉校验）
        int homeworkCount = 0;
        /// 被跳过的非 homework 条目数（测试/课件等，尚未纳入采集范围）
        int skippedOtherTypes = 0;
    };

    /**
     * 待办列表结果（v2 新增）。
     *
     * `supported == false` 表示这个"变更信号"接口不可用（404/500/形态不符），
     * 调用方必须回退到完整扫描——绝不能把它当成"没有待办"。
     */
    struct TodoListResult
    {
        Error error;
        bool supported = true;
        QList<TodoItem> items;
    };

    explicit NjuLmsAdapter(IPageBridge *bridge, QObject *parent = nullptr);
    ~NjuLmsAdapter() override;

    // ---- 站点常量（全部标注来源）--------------------------------------------

    /// [实测] POST /api/my-courses，body 为 {} 时返回本人全部在修课程。
    /// 实测返回 {"courses":[...]}，课程对象含 id/name/url/course_code/department/
    /// instructors/semester/academic_year；**不含** created_at/updated_at。
    static QString myCoursesPath();

    /// [实测 2026-10-09] GET /api/todos → {"todo_list":[...]}
    /// 跨课程的待办集合（含 course_id / id / type / end_time），
    /// 用作增量同步的「变更信号」；只覆盖待办项，不能替代完整快照。
    static QString todosPath();

    /// 一次请求最多重试次数（需求场景 D：有限次重试 + 递增等待）
    void setMaxRetries(int retries);

    /// 相邻请求之间的最小间隔（毫秒）。默认 400 ms，避免给校方服务造成压力。
    void setRequestIntervalMs(int ms);

    // ---- 适配器接口（对应需求文档建议接口）----------------------------------

    /**
     * 检查当前会话是否仍然有效。
     *
     * ⚠️ 关键：**不能**仅凭首页加载成功判断已登录（需求文档明确要求），
     * 也**不能**凭 HTTP 状态码判断——会话失效时站点很可能返回登录页 HTML 且状态码仍为 200。
     * 因此判定依据是「响应体能否解析为 JSON 且含 courses 数组」。
     */
    void checkSession(std::function<void(const SessionCheck &)> callback);

    /// 发现当前用户有权访问的课程列表。
    /// 每次同步都重新调用，不依赖首次采集结果（需求场景 G）。
    void discoverCourses(std::function<void(const CourseListResult &)> callback);

    /**
     * 读取跨课程待办列表（v2 的「变更信号」）。
     *
     * 用途：增量同步先用它判断"哪些课程可能变了"，再只对变化过的课程做深度扫描。
     * 接口不可用或形态不符时把 `supported` 置 false —— 调用方**必须**回退到完整扫描，
     * 绝不能把"接口坏了"当成"没有待办"。
     *
     * 覆盖范围：只含待办/未完成条目（实测），历史与已提交项不在其中，
     * 因此完整对账仍需按周期执行。
     */
    void fetchTodos(std::function<void(const TodoListResult &)> callback);

    /**
     * 获取某门课程的作业列表。
     *
     * [实测] GET /api/courses/{id}/homework-activities
     *   响应为 {"homework_activities":[...], "page":1, "page_size":10, "pages":1, "total":3}
     *   实测 page_size 可调（10/50/200 均被接受），因此本实现一次请求取全量，
     *   把「翻页」降级为「单次快照」，从根本规避翻页期间列表变化导致漏项（场景 H）。
     *   仍会用 total 做完整性校验；不一致时标记 snapshotComplete=false，断点不推进。
     *
     * ⚠️ 实测该接口的条目**不含 unique_key**（6/6 条均无），稳定标识由 Parser 按
     * 站点的 "{type}-{id}" 规则推导，详见 AssignmentParser 与 docs 2.4 节。
     */
    void fetchAssignments(const QString &courseId,
                          std::function<void(const AssignmentListResult &)> callback);

    /**
     * 获取某门课程的考试列表（考试接入）。
     *
     * [实测 2026-10-10] GET /api/courses/{id}/exam-list?page=1&page_size=N&reloadPage=false
     *   响应 {"exams":[…], "total":n, "page":…, "pages":…, "page_size":…}
     * 接口路径与信封来自站点自身前端代码（static/67958-55938765.js 的 initExamsWithPage）。
     * 本账号当前所有课程 exams 为空数组，因此条目字段解析按"前端代码实测 + 防御式读取"处理，
     * 详见 AssignmentParser::parseExamList。
     */
    void fetchExams(const QString &courseId, std::function<void(const ExamListResult &)> callback);

    /**
     * 获取课程任务列表（作业与测试等活动的合集入口）。
     *
     * [实测] GET /api/courses/{id}/activities 存在并返回 200，载荷为 {"activities":[...]}。
     *   实测每一条 homework 活动都含 unique_key（与 /homework-activities 相反），
     *   且 data.description 存在。
     * ⚠️ **未实测**：该响应实测**不含 total / page / page_size / pages 任何分页元数据**
     *   （现有样本仅来自一门只有 3 条活动的课程）。因此无法证明返回的是完整集合，
     *   本实现把 snapshotComplete 标为 false 并记录告警，**不推进断点**，
     *   以免在活动较多的课程上漏项。验证方法见 docs 第七节 V5。
     */
    void fetchCourseTasks(const QString &courseId,
                          std::function<void(const CourseTaskResult &)> callback);

    /**
     * 获取单个作业详情。
     *
     * ⚠️ 尚未发现经过验证的作业详情 JSON 接口。作业所需的全部字段
     * （标题、说明、截止时间、发布时间、提交状态、得分）实测已包含在列表响应中，
     * 因此当前**不需要**详情接口。此方法保留接口形状但返回 Unsupported，
     * 不凭空构造 URL。
     */
    void fetchAssignmentDetail(const QString &courseId, const QString &assignmentId,
                               std::function<void(const AssignmentListResult &)> callback);

signals:
    /// 会话失效，UI 应暂停采集并提示重新登录
    void sessionExpired();

    /// 发生可重试错误，已安排第 n 次重试
    void retrying(int attempt, int maxAttempts, const QString &reason);

private:
    /**
     * 取某一页作业并按稳定标识合并进 accumulated，必要时自动递归取下一页。
     *
     * 用 shared_ptr 传递累积结果：翻页是异步的，必须让状态活得比当前调用帧长。
     */
    void fetchAssignmentsPage(const QString &courseId,
                              int page,
                              std::shared_ptr<AssignmentListResult> accumulated,
                              std::function<void(const AssignmentListResult &)> callback);

    /// 取某一页考试并按稳定标识合并进 accumulated，必要时自动递归取下一页。
    void fetchExamsPage(const QString &courseId,
                        int page,
                        std::shared_ptr<ExamListResult> accumulated,
                        std::function<void(const ExamListResult &)> callback);

    class Impl;
    std::unique_ptr<Impl> d;
};

} // namespace njulms
