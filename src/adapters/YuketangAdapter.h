#pragma once

#include "adapters/IPageBridge.h"
#include "models/Assignment.h"
#include "models/Course.h"

#include <QObject>
#include <QString>

#include <functional>
#include <memory>

namespace njulms {

/**
 * 雨课堂（yuketang.cn）适配器。
 *
 * 形态介于南大与学习通之间：数据是**结构化 JSON**（不需要 HTML 解析），
 * 但接口分散在三组前缀下（/v2/api/web、/mooc-api/v1、/c27/online_courseware），
 * 且**所有请求都必须带自定义头 `xtbz: ykt`**（实测缺它直接返回 "XTBZ IS REQUIRED"），
 * 部分接口还要大学标识 `sign`（从课程基本信息里取得，进程内缓存一次）。
 *
 * 接口与字段来源（2026-10-11 登录态实测，见 docs/07 与 docs/03）：
 *   - 课程列表：GET /v2/api/web/courses/list?identity=2 → data.list[]
 *   - 课程大纲：GET /mooc-api/v1/lms/learn/course/chapter?cid=&classroom_id=&term=latest
 *   - 学习进度：GET /mooc-api/v1/lms/learn/course/schedule?cid=&classroom_id=&sign=
 *   - 课程信息（取 sign）：GET /c27/online_courseware/course/classroom/{cid}/get_basic_info/
 *
 * 只读：不提交作业、不进入考试；会话失效走统一的"需要登录"流程。
 */
class YuketangAdapter : public QObject
{
    Q_OBJECT

public:
    enum class ErrorCode {
        None,
        SessionExpired,   ///< 未登录 / 会话失效（401、401002、UNAUTHENTICATED、web_redirect）
        UnexpectedShape,  ///< JSON 结构不符（改版）→ 记录解析错误，不得当空数据
        Transport,        ///< JS / 网络失败 → 可重试
        HttpStatus,       ///< 非 2xx
    };

    struct Error
    {
        ErrorCode code = ErrorCode::None;
        int httpStatus = 0;
        QString message;

        bool ok() const noexcept { return code == ErrorCode::None; }
        bool needsLogin() const noexcept { return code == ErrorCode::SessionExpired; }
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

    struct CourseListResult
    {
        Error error;
        QList<Course> courses;
    };

    struct WorkListResult
    {
        Error error;
        QList<Assignment> assignments;   ///< 作业(kind=homework) + 考试(kind=exam)
        ParseIssues issues;
        /// chapter 大纲与 schedule 进度都取到才算完整快照（允许软删除）
        bool snapshotComplete = false;
    };

    explicit YuketangAdapter(IPageBridge *bridge, QObject *parent = nullptr);
    ~YuketangAdapter() override;

    // ---- 路径（全部标注来源）-------------------------------------------------

    /// [实测] 课程列表（学生视角）
    static QString coursesPath();
    /// [实测] 课程大纲（章 → 节 → leaf；作业=leaf_type 6、考试=5）
    static QString chapterPath(const QString &classroomId);
    /// [实测] 学习进度（需要 sign，缺它会返回"参数错误"）
    static QString schedulePath(const QString &classroomId, const QString &sign);
    /// [实测] 课程基本信息（含大学标识 course_sign）
    static QString basicInfoPath(const QString &classroomId);

    /// 一次请求的重试次数（默认 2，与南大适配器一致）
    void setMaxRetries(int retries);
    /// 相邻请求最小间隔（毫秒，默认 400）
    void setRequestIntervalMs(int ms);

    // ---- 接口 ----------------------------------------------------------------

    void discoverCourses(std::function<void(const CourseListResult &)> callback);
    void fetchCourseWork(const Course &course, std::function<void(const WorkListResult &)> callback);

signals:
    /// 会话失效，UI 应提示重新登录
    void sessionExpired();

private:
    class Impl;
    std::unique_ptr<Impl> d;
};

} // namespace njulms
