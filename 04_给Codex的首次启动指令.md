# 给 Codex 的首次启动指令

复制下方内容发送给 Codex，先不要让它直接生成整个项目。

---

你是 RADManager 项目的实现工程师。我是架构负责人。

请先完整阅读项目根目录的：

1. `01_RADManager_完整开发计划.md`
2. `AGENTS.md`
3. `03_CODEX_分阶段任务清单.md`

开发环境基线：Windows 10 IoT 企业版 LTSC x64（19044）、Qt 5.15.2 MSVC x64、C++20、VS 2022 / v143、CMake。M0-01 为纯 Win32 POC，不链接 Qt。

## 当前阶段：M0 技术可行性验证

只允许执行 `TASK-M0-01`：建立 Windows ConPTY 最小 POC 和 FakeCli 测试程序，验证：

- 通过 ConPTY 启动交互式 CLI。
- 双向字节流 UTF-8、中文和 ANSI/VT 正常。
- Ctrl+C、Enter 和光标控制序列的交互。
- ResizePseudoConsole 生效。
- 输入/输出并发读取不会造成死锁。
- 进程正常结束、异常结束和主动终止时不泄露 Win32 HANDLE。
- FakeCli 支持持续高速输出压力测试。

## 执行规则

- 先输出技术方案、类/文件清单、HANDLE 生命周期和线程模型，经我确认后再编码。
- 暂不接入 Qt、Tailscale、WebSocket、xterm.js、Claude Provider 或 Git。
- 不写跨模块巨大类，不提前实现未来功能，不改未授权文件。
- 生成可重复的 CMake 构建和测试命令。
- 完成后必须执行可执行的测试；环境不具备时明确指出，不能假设通过。
- 最终汇报格式：改动文件、接口与设计说明、编译测试、已知限制、后续待办。

请开始分析 M0-01，并**先给设计方案，不要直接编码**。
