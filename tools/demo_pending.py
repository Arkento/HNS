#!/usr/bin/env python3
"""在本地数据库里插入/删除一条「未提交」演示作业（开发辅助脚本）。

用途：前端「未提交」页签平时可能是空的（作业都交了），这个脚本放一条一眼能认出
是测试数据的记录，方便在 iPad 上验证卡片与右端的 ↗ 跳转按钮。

注意：这条记录在站点上并不存在，所以**下次完整全量扫描会把它清掉**
（快照里没有它 → 标记为缺失 → 界面不再显示）。想再看就再跑一次本脚本。
「快速增量」如果判断该课程无变化，是不碰它的，所以未必立刻消失。

用法：
    python tools/demo_pending.py                 # 用默认库路径插入（幂等）
    python tools/demo_pending.py --remove        # 删除这条演示数据
    python tools/demo_pending.py --db <路径>     # 指定数据库

数据库路径与程序一致：`<exe 同级>/data/njulms.db`（默认按 build/bin 相对定位）。
运行前请先退出程序（避免 SQLite 写锁与程序读取互相干扰）。
"""

import argparse
import os
import sqlite3
import sys

DEMO_SOURCE_ID = "homework-demo-pending"

DEFAULT_DB = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "build", "bin", "data", "njulms.db",
)


def main() -> int:
    parser = argparse.ArgumentParser(description="插入/删除一条未提交演示作业")
    parser.add_argument("--db", default=DEFAULT_DB, help="数据库路径")
    parser.add_argument("--remove", action="store_true", help="删除演示数据")
    parser.add_argument("--course", default=None, help="课程 ID（默认取库里第一条有作业的课程）")
    parser.add_argument("--activity", default=None, help="借用哪条真实作业的详情链接（默认取库里第一条）")
    args = parser.parse_args()

    if not os.path.exists(args.db):
        print(f"找不到数据库：{args.db}", file=sys.stderr)
        return 1

    connection = sqlite3.connect(args.db)

    if args.remove:
        removed = connection.execute(
            "delete from assignments where source_id = ?", (DEMO_SOURCE_ID,)).rowcount
        connection.commit()
        print(f"已删除演示数据：{removed} 行")
        return 0

    # 借一条真实作业的详情链接，这样点 ↗ 打开的是真页面（而不是又一个"找不到资源"）
    row = connection.execute(
        "select course_id, activity_id, detail_url from assignments "
        "where detail_url is not null and detail_url <> '' "
        "and source_id <> ? order by course_id limit 1", (DEMO_SOURCE_ID,)).fetchone()
    if row is None:
        print("库里没有任何带详情链接的作业，无法借用链接", file=sys.stderr)
        return 1
    course_id, activity_id, detail_url = row
    course_id = args.course or course_id
    activity_id = args.activity or activity_id
    if args.course or args.activity:
        detail_url = f"https://lms.nju.edu.cn/course/{course_id}/learning-activity#/{activity_id}"

    course_name = connection.execute(
        "select course_name from courses where course_id = ?", (course_id,)).fetchone()

    # 幂等：先删旧的同名演示记录
    connection.execute("delete from assignments where source_id = ?", (DEMO_SOURCE_ID,))
    connection.execute(
        """
        insert into assignments (
            course_id, source_id, activity_id, title, description, detail_url,
            published_at, due_at, status, submitted, score,
            content_hash, content_signature, observed_at, first_seen_at, last_seen_at, present)
        values (?, ?, ?, ?, ?, ?,
                datetime('now', '-1 days'), datetime('now', '+3 days'), 'pending', 0, null,
                'demo', 'demo', datetime('now'), datetime('now'), datetime('now'), 1)
        """,
        (
            course_id,
            DEMO_SOURCE_ID,
            activity_id,
            "【测试】未提交示例：点右端 ↗ 打开原站详情",
            "<p>这是一条本地演示数据，站点上并不存在。</p>"
            "<p>用途：验证「未提交」页签的卡片与右端的 ↗ 跳转按钮；"
            "下次完整全量扫描后它会自动消失。</p>",
            detail_url,
        ),
    )
    connection.commit()

    name = course_name[0] if course_name else course_id
    print(f"已插入演示作业：课程={name}（{course_id}），详情链接={detail_url}")
    print("提示：这是本地数据，下次完整全量扫描会把它清掉；想手动删除用 --remove。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
