# M0-01：ConPTY 技术验证与代码导航

> 日期：2026-10-16 ｜ 分支：`codex/m0-conpty-poc`
> 状态：2026-10-17 已获准接入官方 ConPTY 运行库；正式工程 Debug / Release 各 6/6 测试通过。M0-01 待阶段审查，M0-02 尚未开始。
> 本说明记录 M0-01 的实现与验证边界，不表示 M0 或产品已全部完成。

## 1. 实现范围与入口

本轮仅实现 TASK-M0-01。纯 C++20 / Win32，无 Qt、网络、Provider、真实 Claude 或产品 UI；不会操作真实项目仓库或复制 CLI 登录数据。

下表路径相对仓库根目录：

| 文件 | 入口 | 职责 |
|---|---|---|
| [CMakeLists.txt](../CMakeLists.txt) | 构建目标、CTest | 编译、严格告警、可选静态分析与六项测试 |
| [ConPtySession.h](../platforms/windows/ConPty/ConPtySession.h) | `rad::poc::ConPtySession` | POC API 和线程、错误、容量契约 |
| [ConPtySession.cpp](../platforms/windows/ConPty/ConPtySession.cpp) | `start`、`writeQueuedInput`、`readOutput`、`cleanup` | ConPTY、进程树、双向管道与资源生命周期 |
| [FakeCli](../tests/fixtures/FakeCli/main.cpp) | `run`、`execute` | 可重复的控制台输入输出夹具 |
| [集成测试](../tests/integration/ConPtyPocTests.cpp) | `interaction` 等场景 | 用真实 ConPTY 启动同目录 FakeCli |
| [系统探针](../tools/ConPtyOsProbe.cpp) | `createAndCloseConsole` | 不启动子进程、不使用封装，独立复现系统句柄增长 |

没有新建产品层公共接口，也没有实现附加、分离或持久化。当前对象析构会结束其拥有的 POC 进程，不能将该行为直接作为未来 `session.detach` 的实现。

## 2. 启动、输入输出与收尾

1. `start` 校验正数列行、规范化路径，要求显式 `.exe` 位于工作目录直接下一层；单个实例只允许尝试启动一次。这个限制用于同目录测试程序，**不是产品工作区白名单**，也不防御本地恶意替换文件的竞态。
2. 创建两条同步匿名管道和 HPCON，启动持续排空的输出线程；构造进程属性列表。
3. 通过非空 `lpApplicationName` 和加引号的可执行路径调用 `CreateProcessW`；无 shell、无自由参数。进程先挂起，加入 kill-on-close Job，再启动输入线程并恢复主线程。
4. `writeInput` 将字节入有界队列；单独线程写入管道。256 KiB 上限包含队列和正在写入的块，超过上限抛异常，入队成功不等于子进程已经消费。
5. 输出线程持续读原始 UTF-8/VT 字节，保留最近 1 MiB 并累计总量。环形截断可能处于 UTF-8 或 VT 序列中间，不能把该快照当成完整终端画面恢复。
6. `waitForExit` 等待退出、记录退出码并收尾；`terminate` 显式终止自有 Job 进程树。停止输入时循环取消同步写，避免停止标记检查与进入 `WriteFile` 之间的竞态。
7. 清理时保持读取线程工作直到 `ConptyClosePseudoConsole` 完成，以排空尾帧；随后等待线程结束并释放管道、进程等句柄。启动失败也走清理路径。

控制方法与析构只允许构造线程使用；输出快照、累计字节、异步错误查询可跨线程。同步错误抛标准异常；异步错误由 `ioError()` 返回，析构错误输出诊断而不抛异常。

`waitForExit` 的超时仅约束进程等待；之后同步关闭 ConPTY 和排空输出可能额外耗时。当前还不是具备完整取消时限保证的生产后端。

### 本机已修复的兼容问题

- CTest 重定向父进程标准输入输出时，Windows 可能复制这些句柄到子进程。启动信息显式设置 `STARTF_USESTDHANDLES` 并留空标准句柄，让连接 ConPTY 的子进程获得控制台句柄。回归测试全部由 CTest 启动覆盖此场景。
- 本机控制台的 UTF-8 `ReadFile` 输入未正确回显中文。FakeCli 改用 `ReadConsoleW`，再将读取字符转换成 UTF-8；宿主 ConPTY 管道仍原样传输 UTF-8。此结果不表示所有第三方 CLI 的代码页实现均兼容。

## 3. FakeCli 行为

| 输入 | 行为 |
|---|---|
| `echo 文本` + Enter | 输出 `[ECHO]` 和文本，用于中文往返 |
| `ansi` | 绿色 SGR 包围 `[COLOR]绿色输出` |
| `size` | 查询实际控制台窗口并输出 `[SIZE]列x行` |
| 上方向键 `ESC [ A` | 输出 `[UP]` |
| Ctrl+C 字节 `0x03` | 清空待解析输入，输出 `[CTRL_C]`；原始输入模式下不验证 Windows 默认 Ctrl+C 杀进程行为 |
| `flood` | 输出 24,000 行后给出完成标记 |
| `stream` | 持续输出，用于输出途中销毁测试 |
| `pause` | 报告暂停后 30 秒不读输入，用于阻塞输入时终止测试 |
| `exit` / `fail` | 分别正常返回 0 / 调用 ExitProcess 返回 23；后者不是访问违规崩溃测试 |

FakeCli 是受控夹具，非完整 VT 解析器。键盘与字体最终视觉验收留到 M0-02。

## 4. 构建与复现

后续已接入 M0-02，当前根工程默认构建 Qt 演示；若只复现本节 M0-01，请在下列配置命令末尾添加 `-DRAD_BUILD_TERMINAL_POC=OFF -DRAD_BUILD_APPLICATIONS=OFF`。完整工程构建改按 [M0-02 说明](M0-02_Qt终端技术验证.md) 传入 Qt 路径。

从仓库根目录在 PowerShell 执行；下列别名只在当前会话生效，不修改全局环境。本轮刻意使用含中文和空格的构建目录验证进程路径处理。

```powershell
Set-Alias cmake 'D:/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
Set-Alias ctest 'D:/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/ctest.exe'
cmake -S . -B 'build-m0 中文 path' -G 'Visual Studio 17 2022' -A x64 -T v143 -DRAD_ENABLE_ANALYSIS=ON
cmake --build 'build-m0 中文 path' --config Debug --parallel
ctest --test-dir 'build-m0 中文 path' -C Debug --output-on-failure
cmake --build 'build-m0 中文 path' --config Release --parallel
ctest --test-dir 'build-m0 中文 path' -C Release --output-on-failure
```

必须等待构建完成再运行测试。M0-01 无需传 Qt 路径。编译使用 C++20、`/W4 /WX /permissive- /utf-8`；开启 `RAD_ENABLE_ANALYSIS` 时使用 MSVC `/analyze`。格式由根 `.clang-format` 约束，可对上述五个 C++ 文件执行 `clang-format --dry-run --Werror`。

单项复现（失败会返回非零退出码）：

```powershell
& './build-m0 中文 path/Debug/ConPtyPoc.exe' interaction
& './build-m0 中文 path/Debug/ConPtyPoc.exe' lifecycle
& './build-m0 中文 path/Debug/ConPtyOsProbe.exe'
```

`ConPtyPoc` 是可重复交互场景驱动器，不是面向用户的终端窗口。

## 5. 历史验证：系统 API 路径（接入前）

环境：Windows 10 IoT 企业版 LTSC x64，10.0.19044；MSVC 19.44.35214、Windows SDK 10.0.26100.0、CMake 3.31.6。证据是本次本机执行，不继承其他项目的测试结果。

| 检查 | 结果 |
|---|---|
| Debug 构建及 MSVC 静态分析 | 通过；初次静态分析指出读取缓冲栈占用，缩小为 8 KiB 后通过 |
| Release 构建及 MSVC 静态分析 | 通过 |
| `conpty.interaction` | 通过：中文往返、颜色 SGR 与文本关联、上方向键、Ctrl+C 字节、实际尺寸查询、正常退出与最后输出保留 |
| `conpty.throughput` | 通过：总输出超过 1 MiB、保留缓冲不超上限、压力输出后继续输入 |
| `conpty.shutdown` | 通过：非零退出、阻塞输入下主动终止、确认开始持续输出后析构 |
| `conpty.validation` | 通过：未启动操作、缺失路径、损坏 EXE 启动失败、非法尺寸、目录不匹配、重复启动、超长输入 |
| `conpty.lifecycle` | **失败**：12 轮正常退出/主动终止/析构后句柄从 89 增为 101 |
| `conpty.os-handles` | **失败**：不创建子进程的公共 API 探针，12 次 ConPTY 创建/关闭仍从 89 增为 101 |

上述两个句柄测试保持真实失败，CTest 整体不能判为通过。起始计数可能随运行环境变化；关键是每轮增长 1，而非固定基数。

Debug 和 Release 均为 6 项测试中 4 项通过、2 项失败。Release 生命周期计数为 88→100，独立探针为 95→107；与 Debug 的逐次增长一致。五个 C++ 文件的 clang-format 检查通过，独立只读代码审查提出的颜色断言和持续输出时销毁覆盖已修正。

系统探针不使用会话封装、FakeCli、进程属性表或未公开 HPCON 布局，证明本机公共 ConPTY API 路径即可复现句柄增长。当前不能承诺长期运行无泄漏，也不能据此宣布所有 Windows 版本均有同样问题。

**接入前的处理计划（已由下文正式接入结果更新）**：保留复现程序，在经授权维护的系统环境复测；若当前环境必须保持，评估微软受支持的 ConPTY 运行库等替代方案并先审查依赖与部署影响。尚未验证替代方案可解决本机问题。本轮不升级系统、不引入新依赖，不使用私有 HPCON 结构关闭内部句柄，不降低验收阈值。

尚未验证：Qt/WebEngine/xterm.js 实际显示与中文输入法、第三方 CLI、派生多层进程树退出、句柄分配失败等资源故障注入、多会话隔离、网络断线、完整 Agent 生命周期。本节为接入前记录；当前自动化结果见下文，仍不自动推进 M1 或 UI 阶段。

## 6. 维护注意与参考

### 2026-10-17：官方运行库隔离验证（接入前记录）

本机系统 API 探针复测仍失败，12 轮句柄由 85 增为 97。随后将同一份代码复制到临时目录，仅将 ConPTY 创建、调整尺寸和关闭调用改用微软官方包 `Microsoft.Windows.Console.ConPTY` 的 `Conpty*` 导出，链接配套导入库，并按包的部署布局放置 `conpty.dll` 与 `x64/OpenConsole.exe`。没有修改 Windows 系统文件，也没有修改仓库的后端实现或依赖配置。

- 固定验证版本：`1.25.260930003`（非预览版）。[官方包页面](https://www.nuget.org/packages/Microsoft.Windows.Console.ConPTY/1.25.260930003)声明支持 Windows 10 build 17763 及以上，包元数据声明 MIT 许可。
- 来源：`https://api.nuget.org/v3-flatcontainer/microsoft.windows.console.conpty/1.25.260930003/microsoft.windows.console.conpty.1.25.260930003.nupkg`。
- 包 SHA-256：`02B07B349AF66D801159BDF9E440D4A1CE78BB951F37FC8609731665AFDAE7EE`。
- x64 的 `conpty.dll`、`OpenConsole.exe` 均通过本机 Authenticode 签名校验；两文件合计约 1.2 MB，不包含导入库和头文件。
- 隔离验证目录：`%TEMP%/RADManager-ConPty-1.25.260930003/`；`trial` 为实验源码，`build` 为编译结果，`package` 为下载包。临时目录可能被系统清理，不能作为产品依赖路径。
- Debug、Release 均启用 `/W4 /WX /analyze` 并编译成功；MSBuild 给出了临时目录可能影响增量构建的 `MSB8029` 提示，正式接入时应使用普通构建目录。
- Debug、Release 都是 **6/6 测试通过**，没有降低断言。Debug 单独复测生命周期为 83→83，公共 API 探针连续 12 轮均为 83→83。
- 实验中 `conpty.os-handles` 实际已改用运行库导出，只证明该运行库路径稳定，**不表示系统原生 API 已修复**；原仓库探针仍保留原始系统调用和真实失败结果。

结论：本机已验证可通过随应用部署的官方运行库消除当前复现中的句柄增长，无需升级 Windows 或读取 HPCON 私有布局。该结果仍不覆盖长时间运行、其他机器、第三方 CLI 或派生多层进程树。

### 2026-10-17：获准正式接入及回归结果

用户已明确允许新增该官方依赖。`cmake/ConPtyRuntime.cmake` 固定上述版本、官方 URL 与 SHA-256；后端创建、resize、关闭全部使用 `Conpty*` 导出，无自动回退，无公共接口或协议变更。系统探针仍调用原生 Windows API。

- 正式工程 Debug / Release 均通过 `/W4 /WX /analyze` 编译，各 **6/6 CTest 通过**。
- 原五项场景断言不变；第六项为 `conpty.runtime-handles`。Release 生命周期 83→83，独立运行库探针 12 轮 83→83。
- 原生 `ConPtyOsProbe.exe` 仍真实失败（84→96），不代表系统已修复。默认验收只测试应用实际使用的运行库；设置 `-DRAD_TEST_SYSTEM_CONPTY=ON` 可将原生诊断加入 CTest，当前机器将出现预期的失败，不标记为成功。
- 将 Release 必要文件复制至 `build-deployment-check` 后，五个场景和运行库探针全部通过；两份微软二进制签名均为 Valid。这是同机部署目录验证，不代表干净机器或 Qt 部署通过。
- 离线 `.nupkg` 配置通过；错误包在 SHA-256 校验处明确失败。五个 C++ 文件格式检查通过。

#### 在线、离线配置与部署

第 4 节原构建命令不变。首次配置从 NuGet 下载固定包到构建目录 `_deps`，后续复用缓存。离线环境提前保存同一官方包，然后配置（示例路径自行调整）：

```powershell
cmake -S . -B build -G 'Visual Studio 17 2022' -A x64 -T v143 -DRAD_CONPTY_ARCHIVE='D:/deps/Microsoft.Windows.Console.ConPTY.1.25.260930003.nupkg'
cmake --build build --config Debug --parallel
ctest --test-dir build -C Debug --output-on-failure
```

离线包仍强制校验同一 SHA-256；CMake 在构建目录复制为 ZIP 后解压，不依赖临时实验目录。不要用 FetchContent 的源码覆盖选项跳过包校验。

运行目录必须保留以下布局（构建会自动复制运行库和许可）：

```text
ConPtyPoc.exe
FakeCli.exe
InvalidProgram.exe         # validation 场景的故意损坏夹具
ConPtyRuntimeProbe.exe      # 独立运行库验收
conpty.dll
ConPTY-LICENSE.txt
x64/OpenConsole.exe
```

`ConPtyOsProbe.exe` 为可独立运行的原生诊断，不链接官方 DLL。运行库不能只复制 DLL 而漏掉 `x64/OpenConsole.exe`。许可原文保存在 `third_party/ConPTY-LICENSE.txt`，来源为微软 Terminal 仓库 LICENSE；分发时随二进制保留。MSVC 运行环境仍由目标机器提供，尚未在无 VS 的干净机器验证。

当前仅解除 M0-01 的句柄增长阻塞；未完成项仍包括长时间稳定性、其他 Windows 机器、第三方 CLI、多层进程树，以及 M0-02 Qt/WebEngine/中文输入法的真实交互验证。
### 实现维护约束

- 不能把 `cleanup` 或 `terminate` 绑定到未来客户端分离操作。
- 输入取消后必须等待写线程退出再关句柄；关闭 HPCON 时仍需处理输出，不能调换顺序来缩短代码。
- 独立探针与产品封装保持分离，不能通过共享后端实现丢失诊断价值。
- 新增 CLI 启动参数、工作区授权等应在对应阶段设计，不扩展本 POC 的任意命令入口。

官方参考：[创建 ConPTY 会话](https://learn.microsoft.com/en-us/windows/console/creating-a-pseudoconsole-session)、[关闭 ConPTY](https://learn.microsoft.com/en-us/windows/console/closepseudoconsole)、[标准句柄规则](https://learn.microsoft.com/en-us/windows/console/getstdhandle)、[微软 Terminal 关于重定向标准句柄的讨论](https://github.com/microsoft/terminal/discussions/15814)。
