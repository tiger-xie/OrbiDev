# 参与 OrbiDev

当前范围为 `tools/SshWorkbench` 和 `tools/SshSetup`；Client/Agent 后续开发暂停。较大功能先说明问题、范围和验证方式。

1. 使用 README 的 Qt 5.15.2 / VS2022 环境构建 Release 并运行相关测试。
2. 保持职责分层；不向 UI 塞入未经验证的远程命令，不自动接受主机信任。
3. 图标放 `resources/icon/对应模块`，QSS 放 `resources/style`，登记 `OrbiDev.qrc`。
4. 修改输入、输出、关闭或异步查询时补充生命周期测试；界面修改检查 100% 和 150% 缩放。
5. PR 说明实际测试和未测事项，勿上传私钥、连接配置或真实终端日志。

提交格式遵循仓库 Lore 约定：意图行说明原因，正文说明取舍，用 `Tested:`、`Not-tested:`、`Constraint:` 等原生 Git trailer 留下有价值的证据。提交贡献意味着你有权提供该内容，并按项目 MIT 许可授权；第三方作品需注明原始许可。
