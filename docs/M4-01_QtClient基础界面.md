# M4-01 Qt Client 基础界面

## 范围与入口

基于 M3-02 提交 `9031644`，实现已选深灰/青绿色 Qt Widgets 基础界面。默认未连接，主机选择明确标注 FakeAgent；连接后显示两个示例工作区和运行、只读、已退出会话。本阶段不访问网络、不启动 CLI、不接受真实令牌。

在 VS 选择 RemoteClient 调试，或运行：

```powershell
cmake --preset local-msvc
cmake --build --preset debug
& .\build-local\apps\RemoteClient\Debug\RemoteClient.exe
# 自动展示模拟会话，用于界面预览
& .\build-local\apps\RemoteClient\Debug\RemoteClient.exe --demo
ctest --preset debug -R 'client\.(window|ui-smoke|smoke)'
```

正常启动需要本机 Qt bin 在 PATH 中，VS 调试环境已由 CMake 配置。上述命令不执行部署，复制 EXE 到其他电脑仍需完整 Qt 部署。

## 源码与调用链

| 文件 | 职责 |
|---|---|
| [main.cpp](../apps/RemoteClient/main.cpp) | Qt 初始化、设置、窗口与模拟服务组装、验收入口 |
| [ClientWindow.cpp](../apps/RemoteClient/ui/ClientWindow.cpp) | 菜单、工具栏、项目树、多标签、新建及关闭确认 |
| [Theme.cpp](../apps/RemoteClient/ui/Theme.cpp) | 唯一内置 dark 主题的 QPalette/QSS 与主题标识回退 |
| [ClientController.cpp](../apps/RemoteClient/controllers/ClientController.cpp) | 串行异步操作、错误、会话快照与界面通知 |
| [FakeAgent.cpp](../apps/RemoteClient/controllers/FakeAgent.cpp) | 内存模拟服务、失败注入、会话与控制权状态 |
| [WebEngineSmoke.cpp](../apps/RemoteClient/WebEngineSmoke.cpp) | 保留原 M1 WebEngine 资源启动烟测 |
| [ClientWindowTests.cpp](../tests/ui/ClientWindowTests.cpp) | 实际 Qt 控件与确认框行为验证 |
| [ADR-018](ADR/018_Client基础界面.md) | 阶段边界及生命周期决策 |

窗口 → ClientController → FakeAgent 异步完成 → 会话快照与信号 → 更新树和标签。FakeAgent 不依赖界面；UI 不直接调用 ConPTY、Provider 或 SessionManager。沿用 Qt Widgets/Qt Test，没有新增依赖。

## 操作规则

- 连接后单击会话打开；同一个 ID 只出现一个标签。新建只允许已列出的示例工作区，名称不能为空或超过 256 字符。
- 请求处理中禁用重复操作；失败显示错误并保留已有标签和状态，可以重试。
- 关闭运行会话标签先选择分离、终止或取消；默认分离。终止还需明确确认，确认框默认取消。
- 分离成功才关闭标签，模拟进程状态保留，可按原 ID 重附加；关闭主窗口只断开模拟连接。
- 断线保留最后已知进程状态，清除附加和写权限；只读会话不能终止。状态页不提供终端输入框，避免将模拟视图误认为可用 VT 终端。
- `appearance/theme` 存入 QSettings；当前仅内置 dark，未知标识回退并记录警告。不提供无实际选项的换肤菜单。

## 界面验收

`--ui-smoke-test` 使用临时设置，不改用户主题；可加 `--demo` 验证已连接状态。不加则验证初始离线状态。`--screenshot path` 保存窗口内容，`--window-size 1280x800` 指定逻辑尺寸。保留 `--smoke-test` 验证 WebEngine，两种烟测不能同时使用。

2026-10-17 本机验证：

- Debug / Release 的 RemoteClient、RADClientUi、RADClientWindowTests 构建通过，沿用 `/W4 /WX /analyze`；clang-format 检查通过。
- Debug 的 client.smoke、client.ui-smoke、client.window 3/3 通过（23.63 秒）；关闭按钮及请求中禁止取消修正后，两个受影响 UI 测试再次 2/2 通过。
- Release 全套 CTest 25/25 通过（63.87 秒），包含既有 ConPTY、SessionManager、协议、Agent WebSocket 与新 UI 测试。
- client.window 含 11 个行为测试方法（不计 init/cleanup），覆盖真实确认框默认按钮、创建失败重试/执行中取消、附加失败、重复标签、分离及重附加、终止两级确认、关闭窗口、断线、只读、长中文名称与主题回退。未知主题的预期警告由测试显式匹配。
- 实际运行并查看 100%、125%、150% 截图。100% 覆盖 1280×800、1440×1024，125% 的 1280×800 逻辑尺寸输出 1600×1000 像素；150% 窗口被本机可用屏幕高度限制，实际截图为 1920×1061，不能声称验证了完整 1920×1200。
- 长名称在树中省略并保留提示，测试确认 256 字中文名称不会撑大上述逻辑窗口；截图发现的黑色关闭图标已改为高对比按钮并重新截图/回归。

[已连接模拟界面](ui/m4-01-client.png) · [初始离线界面](ui/m4-01-empty.png)。其他缩放截图保存在本机 build-local/m4-01-*.png（构建产物，不纳入版本库）。截图来自 QWidget::grab，只证明静态窗口内容；跨屏动态 DPI、原生标题栏/阴影、全新机器部署、实际网络与 Claude 均未在本阶段验收。

## 后续边界

M4-02 再接入 WebEngine/xterm.js、真实 Agent 连接与终端字节流；本阶段没有远程连接配置页、图片附件、会话恢复或自动重连。NAS 与正式家庭网络验收继续保留。
