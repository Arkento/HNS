#pragma once

namespace njulms {

/**
 * 把 Chromium 自身的日志级别压到 WARNING 及以上（v2 新增）。
 *
 * 背景：目标站点的 SPA 会周期性调用 console.log 打印大量对象，Qt WebEngine 的
 * Debug 构建会把这些页面控制台消息以
 *
 *     [pid:tid:...:INFO:CONSOLE(1)] "[object Object],[object Object],..." source: ...
 *
 * 的形式写到进程的 stderr。Debug 版程序带控制台窗口，于是日志会持续刷屏
 * （Release 版没有控制台，看不到；但 stderr 写入仍会浪费一点时间）。
 *
 * 处理方式：设置 Chromium 的 `--log-level=2`（0=INFO 1=WARNING 2=ERROR 3=FATAL），
 * 只影响 Chromium 自身的日志，本项目自己的日志系统（Logger）不受影响。
 *
 * 调用方已有的 `QTWEBENGINE_CHROMIUM_FLAGS` 会被尊重：
 *   - 未设置：设为 `--log-level=2`；
 *   - 已设置但没有 `--log-level`：追加；
 *   - 已设置且包含 `--log-level`：保持原样（例如调试时想恢复 INFO 输出）。
 *
 * 必须在创建 QApplication / 任何 QWebEngine 对象之前调用。
 */
void quietChromiumInfoLogs();

} // namespace njulms
