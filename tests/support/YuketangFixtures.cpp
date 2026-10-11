#include "support/YuketangFixtures.h"

#include <QJsonArray>
#include <QJsonDocument>

namespace njulms {
namespace test {
namespace yuketang_fixtures {
namespace {

QJsonObject parse(const char *json)
{
    return QJsonDocument::fromJson(QByteArray(json)).object();
}

} // namespace

QJsonObject courseListTwoCourses()
{
    return parse(R"JSON({
  "errcode": 0,
  "errmsg": "Success",
  "data": {
    "list": [
      {
        "term": 202601,
        "name": "2026秋-测试课程甲01班",
        "students_count": 36,
        "teacher": { "user_id": 90000001, "name": "教师甲" },
        "classroom_id": 33000001,
        "course": { "name": "测试课程甲", "platform": 3, "university_id": 4069, "id": 5900001 }
      },
      {
        "term": 202502,
        "name": "2026春-测试课程乙",
        "students_count": 4434,
        "teacher": { "user_id": 90000002, "name": "教师乙" },
        "classroom_id": 33000002,
        "course": { "name": "测试课程乙", "platform": 3, "university_id": 4069, "id": 5900002 }
      }
    ]
  }
})JSON");
}

QJsonObject courseChapter()
{
    // 结构对齐实测（2026-10-11）：course_chapter 里两种形态混用——
    //   第一章：section_leaf_list 的元素是"节"，leaf 在 leaf_list 里；
    //   第二章：「未分类教学活动」形态，section_leaf_list 的元素**直接就是 leaf**。
    // leaf_type 5=考试、6=作业、0=视频（其它类型应被忽略）。
    return parse(R"JSON({
  "data": {
    "course_id": 5900001,
    "course_name": "测试课程甲",
    "course_chapter": [
      {
        "id": 16827001,
        "name": "第一章",
        "section_leaf_list": [
          {
            "id": 17103001,
            "name": "第一节",
            "leaf_list": [
              {
                "name": "第一章 预习检测",
                "id": 91075025,
                "leaf_type": 5,
                "node_id": 8848673,
                "score_deadline": 1791871259000,
                "start_time": 1790690089000,
                "is_score": true
              },
              {
                "name": "第一章 课后作业",
                "id": 90857633,
                "leaf_type": 6,
                "node_id": 8848666,
                "score_deadline": 1792511999000,
                "start_time": 1790688920000,
                "is_score": true
              },
              {
                "name": "L1 视频",
                "id": 88332495,
                "leaf_type": 0,
                "node_id": 8848665,
                "score_deadline": 1801411199000,
                "start_time": 1789903198000
              },
              {
                "name": "第一章 已完成的作业",
                "id": 90000001,
                "leaf_type": 6,
                "node_id": 8848700,
                "score_deadline": 1790000000000,
                "start_time": 1690000000000,
                "is_score": true
              },
              {
                "name": "第一章 早已截止的作业",
                "id": 90000002,
                "leaf_type": 6,
                "node_id": 8848701,
                "score_deadline": 1600000000000,
                "start_time": 1590000000000,
                "is_score": true
              }
            ]
          }
        ]
      },
      {
        "id": 16827100,
        "name": "未分类教学活动",
        "section_leaf_list": [
          {
            "name": "第二章 预习检测（直接叶子）",
            "id": 91076001,
            "leaf_type": 5,
            "node_id": 8848702,
            "score_deadline": 1792511999000,
            "start_time": 1790690089000,
            "is_score": true
          },
          {
            "name": "直接叶子里的视频",
            "id": 88333001,
            "leaf_type": 0,
            "node_id": 8848703,
            "score_deadline": 1801411199000
          }
        ]
      }
    ]
  },
  "success": true
})JSON");
}

QJsonObject courseSchedule()
{
    // 进度语义（实测样本）：status=5 已完成；{total,done} 计分项；
    // 数字为视频类进度（0~1）。缺条目 = 没有进度信息。
    return parse(R"JSON({
  "data": {
    "leaf_schedules": {
      "91075025": { "status": 1, "total": 0, "done": 0, "score": 0 },
      "90857633": { "total": 1, "done": 0 },
      "88332495": 0.0182,
      "90000001": { "status": 5, "total": 8, "done": 8, "score": 8.0 },
      "90000002": { "total": 1, "done": 0 },
      "91076001": { "status": 5, "total": 5, "done": 5, "score": 5.0 }
    }
  },
  "success": true
})JSON");
}

} // namespace yuketang_fixtures
} // namespace test
} // namespace njulms
