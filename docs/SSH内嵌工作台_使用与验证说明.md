# OrbiDev 内嵌 SSH 工作台

本轮在 `codex/ssh-setup-wizards` 分支继续开发。首次配置与家庭授权基线提交为 `2393728`；内嵌工作台基线提交为 `d59a198`。旧 Client/Agent 和外部 Windows Terminal 启动器保留。后续执行顺序见 [本分支开发计划](SSH工作台_分支开发计划.md)。

## 启动与使用

1. 在公司电脑完成首次配置，连接测试通过后点击“保存配置并完成”。
2. 双击仓库根目录 `Build-SshWorkbench.cmd` 构建，完成后双击 `Start-SshWorkbench.cmd`。也可解压工作台携带包运行 `OrbiDevWorkbench.exe`。
3. 左侧添加项目，填写家庭电脑上的目录；留空使用家庭用户目录。双击项目或点击“打开勾选项目”优先切换已有的准备中／活动终端；没有可复用标签时才创建。选中项目点击“新建终端”始终额外创建一个会话。
4. 每个标签中直接显示 SSH 和 Claude。工作区信任提示由用户自己确认。状态“SSH 运行中”只说明本地进程存在，不代表 Claude 已就绪。
5. 复制／粘贴可用终端右键菜单或 `Ctrl+Shift+C`／`Ctrl+Shift+V`，也支持 `Ctrl+V` 粘贴。鼠标选中终端文字后 `Ctrl+C` 复制；没有选区时 `Ctrl+C` 仍中断远端操作。已退出或禁止输入的终端不能粘贴。缩放使用工具栏字号按钮或 `Ctrl+=`／`Ctrl+-`。中文、Tab、方向键等交给内嵌终端与 SSH 传递；Shift＋方向键的远端输入行选择能力取决于远端 CLI，不等同本地文本框。
6. 关闭活动标签或整个窗口需要确认；断开后不保证远端任务继续。已退出或正在关闭的标签不复用，再次打开会创建新会话。

SSH-02 支持多主机档案。顶部下拉框选择主机，左侧仅显示当前主机的项目；批量打开只处理当前主机勾选的项目。切换主机不会关闭其他主机的标签。

在“连接设置…”菜单中：

- “添加家庭电脑…”创建新档案；可以导入家庭端导出的连接 JSON，已有主机不被覆盖。同一公司端身份可被不同家庭电脑分别授权。
- “编辑当前主机／测试连接…”修改显示名、地址、用户、端口、本机身份路径和主机指纹。独立测试显示网络、SSH、密钥认证和 Claude 检查结果，不启动 Claude 会话。新地址或变化的非空指纹须重新核对；空指纹仍受 known_hosts 严格校验。
- “首次配置向导（当前主机）…”通过临时配置运行原向导，只回写启动向导时对应的主机。向导运行中禁止编辑该主机和退出工作台，避免覆盖或丢失配置。没有主机时完成向导会创建第一台主机。
- “移除当前主机…”会检查并列出关联项目和标签；先在项目编辑中调整“所属主机”或移除项目，再关闭对应标签，才能移除档案。不会删除密钥、远程目录或修改家庭 SSH 服务。

项目编辑支持选择所属主机。已打开标签保持创建时的主机和目录快照；编辑主机或重新分配项目只影响后续打开操作。

SSH-03 增加项目与标签联动：

- 左侧“X 个标签”按稳定项目 ID 计数，包含准备中、运行中、正在关闭和保留的退出标签；关闭标签后扣除。单击项目只选择，双击仍按 SSH-01 优先复用。
- 选择或再次点击标签会定位对应项目，并在需要时切换左侧主机；不创建会话。项目重新分配主机后定位其当前归属，标签提示仍保留原目标；项目已移除时不误选其他项目。
- 标签使用本窗口内唯一、递增的 `#编号`，同名项目及同项目额外会话均可区分。改项目名称同步更新标签标题，不改变项目身份和连接目标。悬停提示包含创建时的主机名、用户、地址、端口和远程目录。
- 状态依次为“准备终端”“校验主机”“SSH 运行中”；关闭时显示“正在关闭”，结束后显示“已退出”或“失败”，保留退出码、错误和原终端输出。“SSH 运行中”只确认本地 ssh.exe 已启动，认证与 Claude 是否就绪需查看终端。
- 退出或终端初始化失败后显示“重新连接…”。确认框列出原标签创建时的主机与目录；确认后额外创建新会话，旧标签保留便于查看错误。即使档案已编辑，也不悄悄改变重新连接的目标；要使用更新后的配置，请重新打开左侧项目。SSH 主机及 Claude 工作区信任仍由用户核对，不恢复原进程。
- 关闭确认可以取消。确认期间发生启动或退出时重新检查实际会话状态；后台校验与清理期间窗口保持响应，不在界面线程等待网络探测。

## 配置兼容

SSH-06 增加独立的日常偏好：

- 顶部“外观…”集中调整终端字号（10～30），主题沿用石墨灰／青绿深色。确认后应用到已有和之后新建的终端；取消不改动。顶部 A−／A+ 也使用同一全局字号。
- 记住窗口位置、正常尺寸、最大化状态、项目栏宽度，以及每台主机最近选择的项目。再次启动只恢复选择，不自动建立 SSH 或启动 Claude。
- 屏幕布局或分辨率变化时，将恢复的窗口限制在当前可用屏幕内，避免窗口落在已移除的显示器外。
- 偏好独立存放于项目路径附加 `.preferences.json` 的文件中，默认是 `projects.json.preferences.json`，不改变主机／项目档案格式。损坏或不支持的偏好文件保留原样并提示，本次运行不覆盖；可在关闭工作台后自行备份移走，再重新设置。

旧版工作台读取首次配置工具的 `connection.json`，项目位于当前用户 `OrbiDev/OrbiDevWorkbench/projects.json`。SSH-02 使用同目录 `projects.json.workbench.json` 统一保存主机、项目绑定及当前主机选择。新档案不存在时，优先迁移已有 `projects.json`，否则读取根目录脚本 `--legacy-config` 指定的 `.local-run/launcher-projects.json`。迁移创建默认主机并绑定旧项目；保留名称、目录、勾选、稳定 ID 与 `terminal_path`。源文件先按原字节备份为 `.ssh02.bak`，再原子写入新档案；原文件不修改。已有新档案时不再次导入旧配置。备份冲突、旧连接缺失或配置损坏时停止并显示原因，不猜测所属主机。内嵌模式不要求安装 Windows Terminal。

SSH-01 的独立项目配置为 v2，给每个项目持久保存独立 UUID。同名、同目录的不同项目互不复用；重命名保留身份，修改目录或连接配置后不会误切回旧目标。多个可复用标签中选择最近使用的一个。覆盖已有 v1 项目文件前保存同路径 `.v1.bak` 原字节备份；备份冲突、读取或保存失败会报错并停止本次打开，不覆盖原配置。旧外部启动器仍使用原 v1 文件。

命令行 `--config`、`--projects`、`--legacy-config` 指定迁移源配置位置；新档案路径是在 `--projects` 路径后附加 `.workbench.json`。迁移后独立启动旧向导更改旧 `connection.json` 不会自动覆盖新档案，请通过工作台菜单编辑指定主机。`--screenshot` 只渲染预览，不读写用户配置或连接 SSH。

## 源码导航

路径相对仓库根目录。

| 文件 | 入口／职责 |
| --- | --- |
| `tools/SshWorkbench/main.cpp` | 定位公司端配置、应用统一主题、组装窗口 |
| `tools/SshWorkbench/WorkbenchWindow.cpp` | `openProject` 复用标签，`createTerminal` 捕获目标并接线状态及重新连接；`selectTerminalProject` / `updateProjectLabels` / `updateTerminalLabel` 联动项目、计数与标题；主机及向导操作仍按指定 ID 编排 |
| `tools/SshWorkbench/WorkbenchStore.cpp` | `loadOrMigrateWorkbenchState` 只在新文件缺失时迁移；`saveWorkbenchState` 校验引用并原子保存；`canRemoveHost` 阻止删除有引用档案 |
| `tools/SshWorkbench/HostDialog.cpp` | `importConnection` 保留本机身份；`checkConnection` 后台执行连接测试；`validateFields` 检查字段与指纹确认 |
| `tools/SshWorkbench/SshSession.cpp` | 后台创建本地 ConPTY、SSH 参数、输入输出、尺寸与退出；界面线程按校验主机、进程启动、输出、错误、退出顺序发信号，不从终端内容推断 Claude 状态 |
| `tools/SshSetup/EnvironmentService.cpp` | `prepareSshArguments` 复用已核对主机指纹并限制 SSH 行为 |
| `tools/ProjectLauncher/LauncherCore.cpp` | 复用项目配置验证与原子保存 |
| `apps/RemoteClient/terminal/TerminalView.cpp` | 复用离线 xterm.js、中文输入、剪贴板与字号 |
| `platforms/windows/ConPty/ConPtyProcess.cpp` | 复用 Win32 进程、管道与官方 ConPTY 运行库 |
| `tools/SshWorkbench/Build.ps1` | 编译、测试、Qt WebEngine／ConPTY 部署与压缩包 |

调用方向：窗口 → SshSession → ConPtyProcess → 系统 ssh.exe → 家庭 PowerShell → Claude。没有接入 Agent 协议、会话数据库或控制权租约。

远程目录单独编码成数据，通过 `Set-Location -LiteralPath -ErrorAction Stop` 切换；失败直接退出，不在其他目录启动 Claude。SSH 连接复用严格主机校验，禁止静默接受变化的指纹。临时信任文件保持到会话结束，私钥内容不进入配置与日志。

## 验证与边界

自动测试包括临时配置迁移、无自动会话启动、非法配置保护、真实本地 ConPTY/FakeCli 中文收发与尺寸同步、双会话隔离、退出／停止，以及错误目录不启动 CLI 的本地 PowerShell 替身验证。终端视图复用测试覆盖 UTF-8、ANSI、剪贴板、输入门控、尺寸与输出溢出。

真实双端验收仍需：两个不同项目同时打开 Claude，中文输入法、`/`、Tab、Esc、方向键、Ctrl+C、复制粘贴、窗口缩放；错误目录、未授权密钥、超时及指纹变化。首次向导的连接测试成功不等于已完成上述交互验收。

不实现断线续跑、原进程恢复、图片剪贴板上传、NAS/Git 集成。关闭时后台停止 SSH 并清理资源；主机探测尚未返回时可能需要等待其有限超时，但界面应保持响应。

### SSH-01 本机回归记录（2026-10-18）

- 计划提交为 `1e14c17`；SSH-01 实现提交为 `37a1309`，未混入原 M5 修改。
- Debug／Release 构建通过，均启用 `/W4 /WX /analyze`；各 8 组 CTest 全部通过。
- 界面测试 11 项通过，包含双击和批量去重、显式新建、最近使用标签选择、活动会话复用、关闭／退出排除、项目身份和连接快照匹配，以及既有退出竞态。
- 项目配置测试 12 项通过，覆盖 v1 备份迁移、v2 ID 持久化、同名独立身份、错误数据和备份冲突保护。
- 复验：打开项目后连续双击、重复点击“打开勾选项目”，标签数应不增加；点击“新建终端”应增加一个，再双击切换最近使用的活动标签。
- 自动去重测试未连接家庭机器；真实 Claude 双端交互和不同项目目录隔离仍待用户验收。

### 2026-10-18 本机验证记录

- Debug／Release 构建及各 7 组 CTest 通过，启用 `/W4 /WX /analyze`。
- Debug 最新会话测试 11 项通过，包含页面输入中文 → ConPTY/FakeCli → 页面回显的整链路；界面测试 8 项通过，包含关闭确认期间新会话启动的竞态。
- 终端测试等待 Qt 窗口焦点后再发送快捷键，避免测试窗口切换时向空指针发送按键。测试初始化关闭 Chromium 后台计时降频；未修改生产终端的安全设置。
- 实际检查 100%／150% 预览布局；截图模式不连接远端。
- 携带包在 PATH 不含 Qt 安装目录时，工作台预览启动和 WebEngine 中文／ANSI 渲染检查返回 0。渲染探针临时使用 QtTest DLL，发布压缩包不依赖测试程序。
- 原 RemoteClient／RemoteAgent 的 Release 目标仍可独立构建。真实远端 Claude 双项目交互尚待用户验收，不能用本地 FakeCli 测试代替。

### SSH-02 状态与验收边界（2026-10-18）

本轮基于 SSH-01 提交 `37a1309`，多主机实现按用户要求独立提交。增加 WorkbenchStore 与 HostDialog，继续复用原 SSH、身份、主机信任与终端组件，不新增依赖或常驻服务。

自动测试覆盖两台主机持久化和重启选择、同名项目隔离、切换主机保留标签、主机编辑后旧标签快照不变、向导按指定主机更新、非法引用拒绝保存、v1/v2 迁移与源字节备份、重复启动不再次导入、备份冲突和损坏配置保护，以及主机编辑的字段／指纹确认。

本版本最多保存 32 台主机、总计 32 个项目，最多保留 16 个终端标签。档案原子保存，但尚未提供跨进程编辑锁，请不要用多个工作台实例同时编辑同一档案。关闭客户端或 SSH 中断仍不保证远程任务持续运行。

真实双端验收：分别添加家庭笔记本与测试机、各自授权同一公司端公钥并核对主机指纹；为两台电脑分别建立项目，依次切换打开，核对终端中的实际用户与目录；确认切换、改名或编辑另一台主机不会改变已打开会话。此项不由本地替身测试代替。

本机验证：Debug／Release 均构建通过并启用 MSVC 代码分析；Release 全套 10/10 通过。Debug 首轮 9/10 通过，原 SshSession 测试一次 120 秒超时，未修改代码或超时阈值，单独复跑约 9 秒通过；首次超时原因未确定。主机编辑和工作台 100%／150% 实际 Qt 截图已检查，无字段遮挡。自动验证未启动真实 Claude 会话；两台真实主机并行交互仍待验收。

用户验收补充（2026-10-18）：家庭笔记本启动 sshd 后，用户确认连接成功。该结果记录为用户反馈，不扩大为两台主机同时运行 Claude 或完整终端交互验收通过。

### SSH-03 实现与本机验证（2026-10-18）

SSH-03 首轮基于 `7a25a46`，修改 `tools/SshWorkbench/WorkbenchWindow.h/.cpp`、`WorkbenchWindowTests.cpp`、`SshSession.h/.cpp`、`SshSessionTests.cpp` 以及本说明和分支计划，共 8 个文件，以下表格保留该轮原始结果。后续 Debug 排查另外修改共用的 `apps/RemoteClient/terminal/TerminalView.cpp` 和 `tests/ui/TerminalViewTests.cpp`，结果见下一节。SSH 改动未提交、未推送；保留原 M5-02、Client/Agent 的其他修改。复用既有终端、严格主机校验和 ConPTY，集中更新标签标题，并让复用与新建共用同一创建入口，没有新增依赖或配置格式。

自动回归覆盖标签计数、同名项目编号、项目改名不改目标、跨主机标签定位、再次点击当前标签的定位、失败原因与退出码、重新连接确认／取消及原目标快照；关闭期间的启动、退出、确认／取消也有窗口和单标签回归。`hostVerificationStarted` 只报告阶段开始，在对象所属线程异步发出，先于启动或退出，不代表主机已验证通过。

| 验证 | 本轮结果与证据 |
| --- | --- |
| Debug／Release 构建 | 均通过，保留 `/W4 /WX /analyze`；日志 `build-ssh-workbench/ssh03-verified-build-Debug.log`、`ssh03-verified-build-Release.log` |
| 格式／差异检查 | 修改的 6 个 C++ 文件通过 clang-format 检查，8 个文件通过 `git diff --check`；没有独立的 clang-tidy 配置，静态分析使用 MSVC `/analyze` |
| 最终 Release CTest | **10/10 通过**；`build-ssh-workbench/ssh03-verified-test-Release.log` |
| 最终 Debug CTest | **9/10 通过**；`workbench.SshSession` 的既有 `embeddedTerminalRoundTrip` 在本地离线终端初始化时超时，尚未启动 SSH；`build-ssh-workbench/ssh03-verified-test-Debug.log` |
| 窗口回归 | Debug／Release 各 19 个用例通过（QtTest 连同初始化、清理总计 21 项），包含 4 组“确认期间退出”的数据化场景 |
| 会话回归 | Release 11 个用例通过（QtTest 总计 13 项）；Debug 新增阶段测试通过，既有页面整链路测试不稳定；调试器下全套曾通过，不能替代最终 Debug 的失败结果 |
| 实际 Qt 界面 | 100%／150% 的失败状态、项目计数、标签编号与重新连接入口已检查，无字段遮挡；截图 `build-ssh-workbench/ssh03-workbench-100.png`、`ssh03-workbench-150.png`；均为本地替身，没有连接家庭主机 |

构建与测试仍使用 `cmake --build build-ssh-workbench --config Debug/Release --parallel 4` 和 `ctest --test-dir build-ssh-workbench -C Debug/Release --output-on-failure`，实际执行时分别传入一种配置。没有修改生产超时、CTest 超时或跳过失败测试。早期 Debug 轮次还出现过离线页面加载超时、剪贴板测试未取得窗口焦点及一次终端测试崩溃；独立复跑结果不一致，原生调试器未再次捕获同一崩溃，根因未确定，不宣称这些问题已修复。

SSH-03 实现待架构审查，Debug 不稳定问题应继续定位。SSH-04 的两个不同真实项目、两台授权主机、真实 Claude 输入、断网与至少 30 分钟／20 次创建关闭等场景仍未执行；不会以本地替身结果代替，不提前进入 SSH-05、SSH-06。可使用仓库根目录 `Start-SshWorkbench.cmd` 启动本次构建的 Release 工作台；原携带压缩包本轮未重新打包。

### SSH-03 Debug 终端稳定性排查（2026-10-18）

已确认并修复两处缺陷：

- 初始化计时器原先在构造函数内启动，首次窗口显示、批量构造或其他同步操作会在页面获得事件处理机会前消耗 20 秒预算。现在把页面加载与计时一起排入绑定当前对象的首次事件回调；提前销毁对象会取消回调。新增“构造后同步等待 21 秒再显示”的回归，旧实现失败，修复后通过。初始化与输出确认仍各为 20 秒，CTest 仍为 120 秒。
- 键盘测试原先依赖 Chromium 内部焦点控件，窗口未激活时会丢失输入，控件失效时会触发 QtTest 断言。现在明确等待窗口激活与浏览器焦点，把组合键送入稳定的顶层窗口，由窗口按每个事件的实际焦点分发；保留 Tab、Esc、方向键、Ctrl+C 和剪贴板的原始字节断言。

补充加载前、加载中、就绪后存在待处理脚本回调的销毁回归，每个阶段重复两次。最后一种场景先等待就绪，再发起脚本请求，不分派事件就立即销毁终端，确认尚未返回的回调恰好执行一次且结果无效，覆盖页面析构时的取消路径。沿用页面先于 profile 销毁的所有权顺序，没有增加共享 profile、延长超时、关闭安全检查或引入依赖。

| 验证 | 本轮证据 |
| --- | --- |
| 超时回归红／绿验证 | `debugfix-regression-red.txt` 记录旧实现失败；修复后同一回归通过，并纳入正式终端套件 |
| 工作台 Debug／Release 构建 | 均通过，保留 `/W4 /WX /analyze`；`debugfix-final-build-Debug.log`、`debugfix-final-build-Release.log` |
| 共用终端目标 Debug／Release 构建 | 原工程的 `RADTerminalViewTests` 均通过；`debugfix-final-shared-build-Debug.log`、`debugfix-final-shared-build-Release.log` |
| 最终正式 Debug CTest | 收紧取消用例后连续两轮 **10/10 通过**，分别约 83 秒、79 秒；`debugfix-clean-ctest-Debug-3.log`、`debugfix-clean-ctest-Debug-4.log` |
| 最终正式 Release CTest | 收紧取消用例后 **10/10 通过**，约 36 秒；`debugfix-clean-ctest-Release-2.log` |
| 共用终端目标 Debug／Release 测试 | 原工程的 `client.terminal` 各 **1/1 通过**，分别约 62 秒、37 秒；`debugfix-final-shared-test-Debug.log`、`debugfix-final-shared-test-Release.log`，另存 JUnit XML |
| 原生诊断重复验证 | 同一生产代码的诊断终端套件串行 **10 轮，每轮 11 项通过且进程退出码为 0**；`debugfix-current-native-summary.txt`、`debugfix-current-native-1.txt` 至 `debugfix-current-native-10.txt`；随后收紧脚本取消用例，最终正式套件单独验证 |
| 强制取消回调诊断 | 收紧后的三阶段销毁用例共 5 项通过，退出码 0；`debugfix-final-cancellation.txt` |
| 格式与差异 | 本分支当前改动的 8 个 C++ 源文件／头文件通过 clang-format，`git diff --check` 通过；没有独立 clang-tidy 配置，静态分析采用 MSVC `/analyze` |

以上日志均位于 `build-ssh-workbench/`。排查时曾在同一块机械磁盘上并行读取约 2.2 GB 的 Qt 私有符号，随后仍出现页面实际加载超过 20 秒；该轮正式 Debug 全套为 9/10，保留在 `debugfix-clean-ctest-Debug-1.log`，没有抹去失败或只记录单独复跑。停止大文件符号读取后，最新全套为上表结果。磁盘竞争是该轮的干扰因素，不能据此断言历史上所有初始化超时都只有一个原因。

**尚未关闭的原生崩溃记录：**中间诊断版本捕获过 Chromium `sequence_checker.h:127` 的序列断言，符号定位到 `web_cache::WebCacheManager::Remove` 析构 Mojo endpoint 的页面销毁链。后续符号初始化中的访问异常属于该次 fatal 之后的异常，不能仅凭临近导出符号把它归为 Qt 样式动画问题。正常现场的创建与销毁线程一致；已准备仅采集模块偏移和检查器线程字段的临时诊断程序，受控断点验证可采样，但尚未取得同一低频失败对象的字段。另有中间版本出现断言均通过后进程退出码 3，原因也未确定。临时诊断文件只留在忽略的构建目录，不进入生产代码。最新通过不能证明这些低频问题已经消除，SSH-03 暂不标为稳定验收完成，也不提前推进 SSH-04。

进一步对照：以相同生产源码和最终销毁用例，仅把键盘投递改回旧的 `QPointer<QWidget>` 方式，诊断变体首轮在第二个用例开始约 41 毫秒时复现原生访问异常（`debugfix-widget-ab-1-console.txt`），尚不能建立它与按键投递的因果关系。该轮 QtTest 首先报告 Mojo 监视器容器的迭代器异常，采样还记录到 `DCLayerOverlayProcessor` 析构、`GpuSwitchingManager::RemoveObserver` 的后续调用链（精确符号报告 `dia-symbol-probe/core-second-av-symbols.txt`）。两条链的先后因果尚未确定，不能据此禁用 GPU 或把容器模板的折叠符号当成完整根因。请求拦截器的 UI 线程、QPointer 保护及页面先于 profile 销毁合同均已核对；没有证据支持改动这些所有权规则。调试器首次完整复跑 11 项通过，结果保留在 `debugfix-first-av-test.txt`；缩小到最先出错的两个用例后，首次异常断点模式又运行 10 轮，各 4 项通过，尚未捕获原始异常堆栈，摘要在 `debugfix-first-pair-summary.txt`。这些复跑不能替代上述失败记录。

### 首次异常取证补强（2026-10-18，继续排查）

旧 `TerminalExceptionProbe.h` 使用全局 `printing` 防重入，展开一个线程的栈时会跳过另一个线程的异常；QtTest 使用的是 `SetUnhandledExceptionFilter`，而非 VEH，并在异常处理中调用 `SymInitialize`，因此日志文字排列不能证明两个异常的先后。新的离线诊断捕获器在展开栈之前，将前四次匹配异常的线程、单调时间、异常记录和寄存器写到独立预开文件，首个记录另生成小型转储；运行时指定 `-nocrashhandler`。它不加载 SSH 配置，不连接远程主机。捕获器及诊断目标保留在忽略目录 `build-ssh-workbench/first-exception-probe/`，说明见其中 `README.md`，共享捕获头位于上一层 `FirstExceptionCapture.h`。

受控 `RaiseException` 已验证转储能还原 `RaiseException → main` 堆栈（`capture-selftest-stack.txt`）；这是人为验证，不是产品首次异常。进程内转储仍可能受堆损坏或锁冲突影响，原始文件槽号也只代表 handler 入口的原子排序，不能冒充跨线程硬件异常的严格顺序。目前没有 Debug／Release Qt DLL 混用或项目覆盖 `_ITERATOR_DEBUG_LEVEL` 的证据。

新增集中验证：`stress-2.txt` 的 40 次就绪后输出／销毁通过（179081ms）；`interrupted-1.txt` 的两个原有终端用例及 100 次加载开始后 `deleteLater` 关闭通过（86050ms）。相同测试首轮仍有初始化失败，保留 `stress-1.txt`；没有原生异常记录。阶段计时显示另一轮首次 profile 构造约 7.8 秒，页面从发起加载到 ready 约 18.2 秒，其中同步 load 约 3 秒（`interrupted-1-console.txt`）。这说明实际页面初始化可能接近 20 秒预算，但不能据此解释全部历史超时，更不能解释原生访问异常。本轮尚未获得真实崩溃的首次异常转储，没有据此改动生产销毁顺序、GPU 设置或超时阈值。

### 观察者断言根因与模块补丁（2026-10-19）

后续对照复跑前 13 轮通过，第 14 轮捕获到更早的 CRT 断言现场，而非只有后续访问异常：`vector(177): vector iterators incompatible`。`capture-pair-14-assert.dmp` 保存了 VizCompositorThread 的 `DCLayerOverlayProcessor` 析构 → `GpuSwitchingManager::RemoveObserver` → vector 迭代器检查调用链。该轮尺寸回调超时，随后退出清理等待被断言对话框阻塞的 Viz 线程；保存现场后只终止了该诊断进程，摘要记录退出码 -1，不算测试通过。

进一步用精确私有符号断点记录，确认 UI 线程的 `RenderViewHostImpl` 与 Viz 线程的 `DCLayerOverlayProcessor` 增删同一个无锁 `GpuSwitchingManager` 观察者列表。原生 Chromium 原本隔离在不同进程的两类对象，在 Qt 的进程内架构中共享该单例；这条跨线程 vector 修改路径与现场迭代器失效吻合。单例仍存活，退出时 AtExit 尚未执行，不采用“单例提前销毁”的解释。软件合成对照仍存在相同跨线程增删，并另有一次初始化超时，故没有把关闭 GPU 当作修复。

已新增 [Qt 模块补丁与证据说明](../patches/README.md)：仅在 Qt/Windows 使用已有 OverlayProcessorStub，避免为不支持 DC overlay 的离屏 surface 创建并注册 DCLayerOverlayProcessor。两处条件修改已在源码副本通过应用检查，构建依赖与 surface 能力经独立审查。**安装中的 Qt DLL 没有替换，补丁还未完成编译和运行时验收。** 本机 qmake 重建预检虽然退出 0，但因缺少 Python 2.7.5+ 明确跳过 webenginecore/webenginewidgets 等全部相关模块；这是构建阻碍，不是构建通过，记录位于 `qt-rebuild-preflight/configure-output.txt`。

当前生产源码的额外诊断验证：四页面同时加载、交错关闭共 12 组／48 个页面通过（`first-exception-probe/parallel-1.txt`，65792ms）；新的 CRT 断言捕获钩子已通过人为断言验证，能在继续执行无效容器操作前保存现场并以退出码 3 结束。人工断言的转储不计作真实故障。仍需重建匹配 Debug/Release 模块，验证 Viz 线程不再注册上述 observer，并重跑 SSH-03。更早 Mojo/sequence checker 故障是否全部同源尚未证明，SSH-03 不标为稳定完成，不进入 SSH-04。

补充：当前正式终端源码的两个最早相关用例，在新版断言捕获器下连续 10 轮均通过且进程退出码为 0（`first-exception-probe/current-pair-summary.txt`）；这些运行仍使用原安装 DLL，不能代替补丁后的回归。诊断目标 Debug 构建保持 `/W4 /WX /analyze` 并通过；工作区差异检查通过。原有 SSH-03 六个实现文件与排查前保存的内容哈希一致，无关修改保留，未提交或推送。

### Release 原生崩溃专项复测（2026-10-19）

使用当前生产终端代码及原安装 Qt 5.15.2 DLL，未应用上述 Qt 模块补丁。Release 诊断目标以 `/W4 /WX /analyze` 编译通过；运行时模块清单确认加载 `Qt5WebEngineCore.dll` 等 Release 库，无 Debug DLL 混用，未禁用 GPU。诊断沿用首次异常捕获器及 `-nocrashhandler`，不连接真实远程主机。

- 两个历史相关终端用例（UTF-8/ANSI 渲染、输入与尺寸）连续运行 20 个独立进程，全部通过、退出码均为 0。
- 三轮压力测试，每轮 40 次就绪后输出并关闭、100 次加载开始后关闭、12 组四页面同时加载并交错关闭；共 564 次页面生命周期操作，全部通过。三轮进程耗时分别约 74.5、66.1、56.8 秒，退出码均为 0。
- 23 个诊断进程均未生成非空异常记录或转储，没有本轮初始化超时。首次冷启动进程约 38.5 秒，其中 profile 构造约 12.4 秒、发起页面加载至 ready 约 5.9 秒；进程总耗时不能当作页面超时。
- 正式工作台 Release 增量构建通过；完整 CTest **10/10 通过**，约 59.54 秒，包括 SshSession、WorkbenchWindow 和终端套件。

证据目录：`build-ssh-workbench/release-crash-check-20261019/`，包含 `summary.txt`、各轮测试及控制台日志、`loaded-qt-modules.txt`、`exception-files.txt`、`workbench-build.txt`、`ctest-release.txt` 和复测脚本。空的预开 `.bin` 是捕获器正常行为，不计作异常。

结论限定为：**本轮 Release 未复现原生崩溃或初始化超时**。Release 未启用 Debug 容器断言，且已定位的跨线程观察者修改路径尚未通过替换 Qt 模块消除，因此不能据此宣布根因修复或 SSH-03 稳定验收完成。真实双主机长期交互不在本轮覆盖范围。本轮未修改生产代码，无关修改保留，未提交或推送。

### 用户决定：延期 Debug 排查，推进 Release 验收（2026-10-19）

用户明确要求暂不继续 Debug 排查，重点记录后本地提交，再按计划推进后续任务。已知问题编号为 **SSH-KI-001**，集中说明见 [Qt 已知问题与候选补丁](../patches/README.md)。该决定解除本轮推进 SSH-04 的 Debug 排查前置条件，不代表问题修复，也不把候选源码补丁记为已部署。后续以 Release 为验收基线；真实双项目、跨主机、断网恢复、30 分钟持续运行及资源趋势仍须逐项记录。历史失败记录继续保留。

### SSH-04 交互缺陷修复（2026-10-20）

依据用户实测，补齐 Ctrl+V、选区 Ctrl+C 和终端右键复制／粘贴菜单，继续复用受控剪贴板入口。Release 程序已更新；实际右键与按键专项通过。完整回归首次 8/10（两个套件初始化超时），失败套件复跑 2/2 通过，不能视作初始化不稳定已消除。用户确认 Shift＋左右方向键在家庭端直接运行 Claude 时同样只能选一个字符，保留为远端编辑环境限制，未改写为客户端已修复。详见 [SSH-04 验收记录](SSH-04_真实双端验收记录.md)。

用户复验补充（2026-10-20）：更新 Release 后用户确认本轮复制粘贴及右键交互修复可提交。真实双端完整验收仍以 SSH-04 逐项记录为准，初始化超时和既有原生问题不因此关闭。

### SSH-06 日常偏好与外观（2026-10-20）

实现集中于 `WorkbenchWindow.*` 的 `loadPreferences`／`savePreferences`、`fitWindowToScreens` 和 `showAppearanceSettings`。偏好使用版本化 JSON 与 `QSaveFile` 原子保存，字段为 `version`、`fontSize`、`railWidth`、`x/y/width/height`、`maximized`、`recentProjects`。最近项目按主机 ID 保存稳定项目 ID，重命名或同名项目不会混淆。退出确认取消时不保存关闭状态；退出保存失败会先明确提示。字号确认和工具栏调整即时保存。

终端复用 `TerminalView::setTerminalFontSize`，在 Qt 侧缓存范围为 10～30 的字号，页面就绪后调用 `clientTerminal.setFontSize`；通过已有 fit／尺寸回调更新行列，不改变输入授权或 SSH 状态。未新增依赖或主题框架；界面仍沿用现有深色样式。

验证证据位于忽略目录 `build-ssh-workbench`：

- Release 构建及 `/W4 /WX /analyze` 通过，clang-format、`git diff --check` 和 JavaScript 语法检查通过。
- 全套 CTest 8/10：会话、窗口、项目配置、主机存储及 4 组配置工具通过；HostDialog 和终端套件因系统剪贴板 `0x800401d0` 失败。本轮没有重现初始化超时，但不关闭此前偶发问题。
- 终端套件 11 项通过、1 项剪贴板失败；新增“初始化前设置字号、就绪后应用、上下界与输入仍禁用”测试通过。
- 最终窗口套件 29 项通过，覆盖偏好往返、不启动会话、损坏／未知版本原文件保护、越屏恢复、最大化恢复、写入失败提示、按主机记住同名项目、全局字号与关闭取消。最大化测试曾因自身未关闭读取文件而阻塞退出保存，修正测试文件生命周期后复测通过。
- 100%／150% 的实际 Qt 工作台和外观对话框截图已检查；两种比例的外观确认／取消、偏好恢复、最大化专项均通过。截图及专项记录位于 `ssh06-visual/`。预览程序在 PATH 不含开发 Qt 的条件下退出码均为 0；测试工具另需 QtTest 运行库，不属于产品依赖。

根目录 `Start-SshWorkbench.cmd` 已指向本次构建的 Release 程序；分发 ZIP 未重打包。本轮未进行真实双端 Claude 操作，也未重启服务、写入用户主机配置或修补 Qt DLL。SSH-04 未执行项和 Debug SSH-KI-001 继续保留；SSH-07 发布整理尚未开始。
