#ifndef CORE_CRASH_REPORT_H
#define CORE_CRASH_REPORT_H

// ── 崩溃自报告 ────────────────────────────────────────────
// 捕获两类静默死亡并留下现场，写入 exe 同目录的 crash_log.txt（纯文本）：
//   1. 未处理 SEH 异常（访问违例等）→ 异常码 + 出错地址 + 调用栈
//   2. std::terminate（工作线程逃出的异常等）→ 异常 what() + 调用栈
// 栈帧记录"模块内偏移"，release 保留调试符号（-g）后可用 addr2line
// 直接映射到源码行：va = 0x140000000 + off，addr2line -e exe <va>。
// ImGui 断言失败走 assert() → stderr，不经过这两个钩子，Qt Creator
// 的应用程序输出里会直接显示文件与行号。
namespace crash_report
{
    // 进程启动时调用一次
    void install();
}

#endif // CORE_CRASH_REPORT_H
