# ADR-014：共享 ConPTY 引擎与异步 Windows 后端

日期：2026-10-17。范围：TASK-M2-01，不包含 SessionManager、网络、UI 集成或 Claude Provider。

## 改动前计划与保护

将原 ConPtySession 的 Win32 资源与读写执行逻辑抽出为 ConPtyProcess；原 POC 保留薄包装及严格 EXE 同目录约束。改动前已运行既有 Release ConPTY 和核心接口测试，7/7 通过。抽出后同组测试必须再次通过，不以新测试取代旧回归。

新增 WinConPtyBackend 实现 ITerminalBackend，链接 RADCore 与共享 Win32 引擎。生产后端不能依赖 POC 专用命名空间，也不复制整份句柄管理实现。

## 线程与所有权

- Qt 对象属于调用方的控制线程，控制方法只校验/提交请求，不等待文件系统、进程或管道。
- 每个后端有独立生命周期执行线程负责路径解析、启动、resize、等待和清理；共享引擎保留独立读管道与写管道线程。
- 输出在有界缓冲中保留，通知合并后交给 Qt 控制线程，通过 takeOutput 拉取，截断明确返回。输入额度计入正在写出的字节，不因出队而提前释放限额。
- 正常退出、显式终止、启动失败与对象销毁均关闭本会话资源。Job Object 约束进程树；读线程必须持续工作到关闭 ConPTY 的尾帧排空。
- 析构是资源兜底，可能等待执行线程完成关闭；正常路径先异步 terminate 并等待 finished 后再销毁，不将对象销毁当作客户端 detach。

## 启动边界

后端使用显式白名单根目录，默认为空即拒绝启动。使用路径句柄解析重解析点，启动期间禁止目标共享删除，按最终路径精确大小写检查真实目录边界；不使用字符串前缀代替路径包含关系。LaunchSpec 的可执行路径由可信 Provider/调用方选定，后端仍检查显式绝对 EXE 路径与合法参数。大小写敏感目录不能因忽略大小写而扩大授权；少数合法别名可能保守拒绝。此检查不是文件系统沙箱，也不能替代后续 Agent 的身份鉴权和 Provider 授权。

只直接启动 EXE，参数按 Windows 参数规则转义一次，不拼接 shell 命令。CMD/脚本包装器适配保留给实际 Provider 任务单独审查。原始控制台字节不解码、不清洗 ANSI。

## 依赖与取舍

复用已获准的 Microsoft.Windows.Console.ConPTY 1.25.260930003，不新增第三方依赖。拒绝在 Qt 控制线程直接调用原同步 POC：创建和收尾可能阻塞。拒绝每块输出都 queued 投递：消费者停顿会造成无界内存增长。

M2 的输出是有界字节缓冲，不是持久会话重放。M1 的 detach 语义由未来 SessionManager 实现，后端本身没有 detach 方法。Agent 入口暂不自动启动任何程序。

## 官方依据

- [创建与关闭 Pseudoconsole](https://learn.microsoft.com/en-us/windows/console/creating-a-pseudoconsole-session)：分离同步管道的服务线程，关闭期间持续排空输出；启动连接期的关闭需谨慎处理。
- [MSVC 命令行参数解析](https://learn.microsoft.com/en-us/cpp/c-language/parsing-c-command-line-arguments?view=msvc-170)：空参数、引号和反斜杠的解析规则，需用真实子进程 argv 验证。
