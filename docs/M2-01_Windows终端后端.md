# M2-01 Windows 终端后端：流程与代码对照

## 范围

2026-10-17，基于文档提交 ee89ca5 的工作区实现。提供可独立测试的 WinConPtyBackend，不包含 SessionManager、远程协议、界面接入或真实 Claude Provider；Client/Agent 仍为工程骨架。

## 源码导航

路径以仓库根目录为基准。

| 文件 | 关键符号 | 职责 |
|---|---|---|
| [后端接口](../core/terminal/ITerminalBackend.h) | ITerminalBackend | 接受请求、事件顺序、有界输出契约 |
| [Windows 适配](../platforms/windows/ConPty/WinConPtyBackend.cpp) | Impl::run、collect、CheckedPath | 异步生命周期、白名单、Qt 通知与错误映射 |
| [共享引擎](../platforms/windows/ConPty/ConPtyProcess.cpp) | startValidated、enqueueInput、cleanup、outputSince | Win32 资源、独立管道线程、进程树、原始字节缓冲 |
| [POC 外壳](../platforms/windows/ConPty/ConPtySession.h) | ConPtySession | 保持旧 POC 的严格启动入口，复用共享引擎 |
| [集成测试](../tests/integration/WinConPtyBackendTests.cpp) | WinConPtyBackendTests | 真实进程验证 |
| [测试程序](../tests/fixtures/FakeCli/main.cpp) | wmain | 中文、ANSI、参数、输出压力和子进程夹具 |

## 一次终端会话

调用方在 Qt 控制线程构造后端并指定 allowedWorkspaceRoots。start 同步检查尺寸、路径形式和参数 NUL；返回成功仅表示受理。独立生命周期线程解析真实文件路径、检查目录授权，再以显式 EXE 和逐参数转义的命令行启动。失败通过 failed 和 finished 返回，成功先发 started。

writeInput 把完整字节包放入有界队列，额度包含正在写出的部分；超额返回 QueueFull，不部分写入。读管道线程持续排空到环形缓存；生命周期线程合并增量输出，Qt 线程收到 outputAvailable 后调用 takeOutput。hasMore 时继续拉取，droppedBytes 明示丢失的最旧字节；不解码或修改 VT 数据。未消费数据与在途通知分别记账，主动轮询也不会无限积累 Qt 事件。

resize 只提交最新尺寸，由生命周期线程调用 ConPTY。terminate 设置退出请求，重复调用安全；Job Object 回收会话进程树。所有退出分支先关闭并排空读线程、归并尾部输出，再通知 finished；之后仍能读取尾部缓存。正常路径等待 finished 后销毁；异常析构会同步等待自身资源回收。

## 配置与维护边界

- 白名单默认空，拒绝启动。仅接受本地盘绝对 EXE 路径，拒绝 UNC、设备命名空间、ADS 和脚本包装器。
- 使用句柄最终路径解析 junction/symlink；启动期间持有禁止共享删除的路径句柄。最终路径采用保守的大小写精确匹配，少数合法别名可能被拒绝。
- 白名单只授权启动工作目录，并非进程文件系统沙箱；受信任 EXE 的选择由后续 Provider 负责，应用鉴权由 M3 负责。
- 输入默认 256 KiB；后端输出默认 1 MiB，底层另有 1 MiB 环形缓冲及最多约 1 MiB 的瞬时快照。停读会丢弃旧输出，不承诺完整回放。
- RAD_BUILD_WINDOWS_BACKEND 默认跟随产品骨架开关；单独关闭它可保留纯 M1 骨架。仅构建纯 Win32 POC 时关闭 APPLICATIONS、WINDOWS_BACKEND 和 TERMINAL_POC。
- 复用固定版本的 Microsoft ConPTY 运行库，无新增依赖。不得改回本机已有句柄泄漏证据的系统运行库。

## 自动验收入口

在 VS 开发环境设置 QTDIR 后使用已有 local-msvc 配置与 debug/release 构建预设。运行：

```powershell
ctest --preset debug -R '^backend\.'
ctest --preset release --output-on-failure
```

7 个场景：integration、argumentsAndTail、validation、lifecycleStress、concurrency、backpressure、processCleanup。JUnit 明细在 build-local/tests/integration/Debug 或 Release 的 m2-*.xml；终端无输出时查看 XML，不推断成功。

生命周期检查先显式触发 Qt 互斥锁争用，初始化全局事件句柄池，再预热会话并精确比较进程句柄数；并发测试同时运行三个 FakeCli，其中一个消费者停止读取；额外验证默认拒绝、同名前缀与 junction 越界、参数空值/中文/引号、退出尾部、通知合并、创建中取消、显式终止和析构时子进程回收。

最终后端集成测试 Debug 7/7（19.80 秒）、Release 7/7（17.58 秒）通过，包括预热后 101 次真实进程创建/退出的严格句柄等量检查和 3 实例并发。Debug/Release 构建（含 MSVC /analyze、/W4 /WX）通过，clang-format 与 git diff --check 通过；纯 Win32 POC 独立配置和 Release 构建通过。原 ConPTY 六项回归在两种配置均通过。网络、真实 Claude、UI 操作、长时间耐久性与故障注入不属于本轮已验证范围。设计理由见 [ADR-014](ADR/014_Windows终端后端.md)。

### 句柄测试诊断依据

初轮 Debug/Release 生命周期测试均出现 153→169 的固定增长。独立进程循环 140 次无持续增长；Qt 5.15.2 本机源码 qmutex.cpp 的 QMutexPrivate 全局池首块为 16 项，qmutex_win.cpp 为每项创建一个 Event，池在进程退出时释放。测试显式初始化此池，使基线不受第一次锁争用时序影响，仍要求后续会话结束后句柄数精确相等，不使用容差掩盖泄漏。

### 回归限制

本轮全套回归中 terminal.webengine 在 Debug/Release 均曾超时；Release 单独复跑通过（27.03 秒），Debug 单独复跑仍 100 秒超时，原因尚未定位。本轮未修改 TerminalPoc 或 WebEngine 代码，不能据此宣称 Debug 全套通过。Client/Agent 烟测、核心接口与工程基线测试在两种配置均通过；UI 启动耗时波动仍需单独排查。
