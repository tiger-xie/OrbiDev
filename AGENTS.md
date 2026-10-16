# AGENTS.md — RADManager 仓库开发约束（Codex 必须遵守）

## 项目背景

Remote AI Development Manager：公司端 Qt Client，经 Tailscale 访问家庭 Windows Agent；Agent 用 Windows ConPTY 持有多个 Claude Code/未来 Codex CLI 的交互式会话；NAS Gitea 承载 Git，不承载用户的 Claude 登录信息。

第一优先级：跨客户端断线持续运行的独立 ConPTY 进程、可靠输入输出和严格身份认证。

## 核心约束

1. **先阅读** `01_RADManager_完整开发计划.md` 和 `03_CODEX_分阶段任务清单.md`，只做用户当前指定的任务卡。
2. C++20 / Qt 5.15.2 MSVC x64 / VS 2022（v143）/ CMake，业务模块优先 C++ 与 Qt，不为方便引入无关框架。
3. 核心模块分层：`apps/` 只编排；`core/` 定义抽象和业务模型；`platforms/windows/` 实现 Win32 ConPTY；`providers/` 实现 CLI 适配。
4. **严禁**把 AI Provider 启动逻辑塞进 UI、WebSocket 或 ConPTY 后端。
5. **严禁**把 `detach()` 实现为进程关闭；`terminate()` 必须显式控制并确认。
6. **不承诺** Agent 或 Windows 重启后原 ConPTY 进程能够无损恢复。
7. 不把纯文本编辑框当 VT 终端；要验证 ANSI、UTF-8、窗口尺寸、输入控制序列。
8. 进程启动必须检查工作区白名单、文件路径规范化，严格转义参数；不得拼接未经验证的 shell 命令。
9. 不保存、不发送、不打印 Claude token、代理密码、SSH 私钥、银行卡信息等敏感数据。
10. Agent 不暴露公网、不默认以管理员或 LocalSystem 权限运行；Tailscale 和应用级配对双重防护。
11. 会话输入采用单写者控制权，所有网络 API 先鉴权并校验 sessionId。
12. 采用 RAII 管理 Win32 HANDLE；ConPTY 输入、输出由独立执行上下文处理，做好有界队列及资源清理。
13. 初期 Provider 使用静态工厂注册，不要提前实现动态 DLL 插件 ABI。
14. 所有远程操作必须能错误返回，不要吞异常/返回假成功；日志脱敏且按大小/时间轮转。
15. 不自动推送提交、删除 worktree、强制重置、覆盖未提交修改，不执行任何破坏性 Git 动作，除非用户明确要求。
16. 不实现规避账号地区或安全审查的逻辑；遵守软件服务的实际地区与使用资格要求。

## Codex 每次实现流程

1. 先列出需求理解、拟修改文件、影响接口、依赖、潜在风险、测试计划。
2. 只实现当前任务，不额外修改不相关文件。
3. 先写能够复现需求的测试，或说明技术 POC 无法单测时的人工验收脚本。
4. 完成后运行可执行的编译和测试命令；若环境不能编译，必须明确说明，**不能虚构测试通过**。
5. 报告格式：`实现内容 / 变更文件 / 编译验证 / 测试结果 / 未完成事项 / 风险`。
6. 每个阶段完成后等待架构负责人审查，再继续下一阶段。

## 代码风格

- 类名 PascalCase、方法 camelCase、成员变量 m_ 前缀；接口前缀 I。
- Qt 信号槽和跨线程通信使用清晰的对象所有权模型；不要跨线程直接操作 UI。
- CMake 使用 target-based linking，不全局添加不可控 include/link flags。
- 协议 DTO 与业务对象分离；引入新协议字段应更新协议文档与测试。
- 禁止硬编码敏感路径、主机地址、凭据；配置仅包含公开示例值。
- 优先简洁、可测试、可观察的实现；不要生成过度复杂的“通用框架”。

## 参考构建基线（路径按实际安装调整）

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -T v143 -DCMAKE_PREFIX_PATH="D:/Qt/5.15.2/msvc2019_64"
cmake --build build --config Debug --parallel
ctest --test-dir build -C Debug --output-on-failure
```

上述 Qt 路径只是示例，项目尚未创建时不能声称命令执行成功。

## 阶段硬要求

- M0：ConPTY 技术 POC + FakeCLI 验证。
- M1：仓库骨架、最小接口、可编译测试。
- M2：SessionManager、多个独立 CLI、输入输出线程安全。
- M3：远程协议/鉴权。
- M4：Qt 多终端 UI。
- M5：断线附加/状态持久化及可观察错误。
- M6：Claude Provider。
- M7：安全与稳定。
- M8：发布。

**不要跨里程碑提前堆功能。**
