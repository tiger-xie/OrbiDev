# M0-02 离线终端资源

候选组合为 xterm.js 4.19.0 / xterm-addon-fit 0.5.0，精确版本和包完整性记录在 `package-lock.json`。实际验证结果以 [M0-02 说明](../../docs/M0-02_Qt终端技术验证.md) 为准。

`vendor/` 是从官方 npm 发布包原样复制的 JS、CSS 和 MIT 许可，纳入源码后直接编入 QRC。构建和运行终端不需要 npm、CDN 或网络。只有更新/核对第三方资源时需要 Node：

```powershell
cd frontend/terminal
npm ci --ignore-scripts --no-audit --no-fund
npm run verify
```

更新版本时先编辑 package.json、重建锁文件，再运行 `npm run vendor` 并重新执行本机 WebEngine 自动和人工验收。不要只更新 JS 而漏掉配套 FitAddon、CSS 或许可。

上述旧包名称已被 npm 标记 deprecated；新包名为 @xterm/xterm 和 @xterm/addon-fit。这是当前旧 Qt 内核的兼容性 POC，不是长期维护或安全支持承诺，不自动升级为最新版。

`terminal.js` 负责显示、输入及确认，`self-test.js` 只在宿主传入 `--self-test` 时运行。QWebChannel 采用异步调用；输出写入 xterm 完成后才确认，宿主据此推进压力输出。复制返回选择文本，粘贴由 Qt 读取系统剪贴板后通过 JSON 传入，不开放网页剪贴板权限。
