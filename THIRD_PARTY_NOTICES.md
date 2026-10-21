# 第三方组件与对应源码

OrbiDev 自有代码采用根目录的 MIT License。该许可不替代以下组件各自的许可，也不将 Microsoft 运行库重新许可为 MIT。

| 组件 | 使用方式 | 许可及随包文本 |
| --- | --- | --- |
| Qt 5.15.2 | 动态链接 Qt Core、GUI、Widgets、Network、WebEngine、WebChannel 及其依赖/插件 | LGPLv3；`licenses/LICENSE.LGPLv3`、`LICENSE.GPLv3` |
| Qt WebEngine / Chromium 及第三方组件 | 内嵌终端页面 | LGPLv3、部分 LGPLv2.1、BSD 及各组件许可；`licenses/LICENSE.Chromium` 只是 Chromium 总许可，另见 `licenses/qt-5.15.2-notices/` |
| Microsoft ConPTY | 固定版本 `1.25.260930003` 的运行库与 OpenConsole | MIT；`licenses/ConPTY-LICENSE.txt` |
| xterm.js / fit-addon | 终端渲染 | MIT；`licenses/xterm-LICENSE.txt`、`fit-LICENSE.txt` |
| Lucide | 界面图标 | ISC；`licenses/LICENSE-Lucide.txt` |
| Microsoft Visual C++ Runtime、D3Dcompiler | Windows 运行依赖 | Microsoft 对应分发许可；不是本项目 MIT 代码 |

## Qt 对应源码及用户权利

公开预览版 Release 与二进制包一同提供 `qt-everywhere-src-5.15.2.tar.xz`、官方 SHA256 校验文件和 `source-manifest.json`。完整源码包含 Qt 及其随附第三方代码、许可证和构建文件。下载位置：

https://github.com/tiger-xie/OrbiDev/releases

官方原始来源：https://download.qt.io/archive/qt/5.15/5.15.2/single/qt-everywhere-src-5.15.2.tar.xz

官方 SHA256：`3a530d1b243b5dec00bc54937455471aaa3e56849d2593edb8ded07228202240`。

发布脚本从该固定官方源码提取许可/版权/归属说明，以 index.json 对应原始路径并随二进制提供。集合也包含未链接组件，不能据此推断运行依赖。完整源码附件保留未采用通用许可证文件名的其他声明。Qt 的原始在线安装器 `msvc2019_64` 5.15.2 动态库未经本项目修改；仓库 `patches/` 内候选补丁未应用于发布二进制。

用户可以修改 Qt 并替换接口兼容的 DLL，也可以使用本仓库源码重新构建；本项目不限制为调试此类修改而进行的逆向工程。替换时应同时使用匹配的 Qt WebEngine DLL、辅助进程、资源及插件，在独立目录操作。文件哈希校验用于检测传输损坏，不是阻止用户运行修改后的程序。Qt 自身的重建要求以源码内 README、configure 和各模块文档为准；应用构建步骤见 README。Qt 5.15.2 WebEngine 的源码构建工具链不同于本应用的 VS2022 工具链。

公开 Release 必须保留以上源码附件的可获取性。若后续修改 Qt，必须随对应版本提供实际修改和匹配源码，不能继续标记为 `modifications: none`。

## 上游说明

- Qt LGPL 分发义务：https://www.qt.io/development/open-source-lgpl-obligations
- Qt 5.15.2 WebEngine 许可说明：源码 `qtwebengine/src/webengine/doc/src/qwebengine-licensing.qdoc`
- Microsoft 运行库分发：https://learn.microsoft.com/en-us/cpp/windows/redistributing-visual-cpp-files
- ConPTY 固定来源与哈希：`cmake/ConPtyRuntime.cmake`

便携包不提供 Claude、Tailscale 或 OpenSSH Server；使用者需自行安装并遵守其条款。
