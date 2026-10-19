# Qt 5.15.2 Windows 离屏合成观察者竞态补丁

状态：**已知问题 SSH-KI-001，用户于 2026-10-19 明确决定暂停 Debug 排查，保留风险并推进 SSH-04。问题未修复、未关闭。** 根因证据已保存，源码补丁已在副本应用并审查；尚未重建 QtWebEngine DLL，尚未完成运行时修复验收。

## 延期决定与使用影响

- Debug 已实际出现容器断言、终端尺寸回调超时和退出清理卡住；此前还记录过原生访问异常。发生时会影响终端使用或程序退出，不能视为无害日志。
- Release 专项复测的 20 轮独立进程、三轮共 564 次页面生命周期操作和正式 10/10 测试均通过，未捕获原生异常；当前后续验收以 Release 为基线。这不是跨线程竞态已消除的证明，也不保证断线后的远端任务继续运行。
- 暂不重建或替换 Qt SDK，不继续为 Debug 问题扩大业务代码改动，不以关闭 GPU、延时销毁或放宽超时掩盖问题。
- 用户报告 Release 原生崩溃、页面关闭卡死，或后续准备 Qt 升级／发布稳定性验收时，应重新评估本问题；保留首次异常现场，按下文重建门槛验证，不凭旧测试结果关闭问题。
- 更早 Mojo／sequence checker 故障以及实际页面加载超过 20 秒仍分别未关闭，不全部归因于这个观察者竞态。

本决定只延期 Debug 排查，不跳过 SSH-04 真实双端验收。分支计划必须保留这个已知风险，发布记录也必须披露。

## 问题与证据

Qt 的浏览器与 Viz 合成器位于同一进程。`RenderViewHostImpl` 在 UI 线程、`DCLayerOverlayProcessor` 在 Viz 线程增删同一个 `GpuSwitchingManager::observer_list_`，后者是无锁普通 `ObserverList`，内部使用 vector。观察者查找与删除之间的并发修改会使调试迭代器失效。

2026-10-19 的实测证据均在 `build-ssh-workbench/`：

- `native-widget-probe/capture-pair-14-assert.dmp`、`capture-pair-14-assert.txt`：Viz 线程停在 CRT `vector(177): vector iterators incompatible` 断言。调用链为 DCLayerOverlayProcessor 析构 → GpuSwitchingManager::RemoveObserver → ObserverList::RemoveObserver → iterator::_Compat。该轮尺寸回调等待失败，进程退出清理又等待阻塞的 Viz 线程；主线程退出堆栈不是断言最初发生时刻的证明。
- `native-widget-probe/capture-pair-14-vector.txt`：单例和 vector 仍存活，迭代器已失去容器关联；不支持“单例先析构”的解释。
- `native-widget-probe/gpu-trace.txt`：同一个 manager `000002b1ad2d29f0`，UI 线程 `4758` 与 Viz 线程 `1ef58` 都增删观察者。按精确 PDB 映射，UI 调用者为 RenderViewHostImpl，Viz 调用者为 DCLayerOverlayProcessor。其他 manager 地址的折叠函数命中不计入此证据。
- `dia-symbol-probe/gpu-observer-callers.txt`、`first-assert-symbols.txt`：上述私有符号的精确映射，不依赖调试器显示的临近导出符号。
- `native-widget-probe/software-trace.txt`：`--disable-gpu` 后仍存在同一列表的跨线程增删；该对照另有页面初始化超时，不能作为修复或验证通过。

这确认了本次观察者迭代器断言的竞态路径。更早的 Mojo 访问异常和 sequence checker fatal 是否全部由同一竞态引起，尚未证明，仍需保留各自记录。

## 补丁范围

`qtwebengine-5.15.2-windows-overlay-stub.patch` 相对 QtWebEngine 模块源码根目录，只修改 `overlay_processor_interface.cc` 的两处 Windows 条件，使 Qt/Windows 与现有 Qt/macOS 路径一样使用 `OverlayProcessorStub`。

Qt 的 GL 与软件离屏 output surface 没有启用 `supports_dc_layers`，原对象本来就不执行 DC overlay，但仍注册观察者。改用 Stub 消除这项不必要的跨线程注册；非 Qt Windows 保持原实现。Stub 源文件已在 `components/viz/service/BUILD.gn` 无条件编译，不新增依赖或改动 GN。

不采用关闭 GPU、禁用 direct composition、增加销毁延时或泄漏页面：前两者不能阻止该版本无条件构造 DCLayerOverlayProcessor，后两者不修复线程安全问题。

## 已验证与重建门槛

- 已从本机 Qt 5.15.2 原始文件生成补丁，在 `build-ssh-workbench/qt-overlay-patch-check/` 副本通过 `git apply --check` 并实际应用；安装中的 Qt 源码与 DLL 未改动。
- 已独立审查平台分支、Stub 构建依赖及 Qt output surface 能力。
- 本机独立构建预检见 `build-ssh-workbench/qt-rebuild-preflight/configure-output.txt`：qmake 虽退出 0，但明确提示缺少 Python 2.7.5+，webenginecore/webenginewidgets 等模块全部跳过；不能把这个结果当作构建成功。当前可用 Python 为 3.12，MSVC 工具集为 14.44/v143。
- 后续需要准备能构建该旧版本的工具链，在独立源码副本应用补丁，输出到独立安装目录，重建匹配的 Debug/Release 模块及辅助进程；不要直接覆盖现有 SDK。
- 重建后先重复同一断点 trace，确认 Viz 不再向此单例注册 DCLayerOverlayProcessor，再运行 Debug/Release SSH 全套、终端创建/关闭/取消回调/尺寸输入测试，并检查退出码和原生断言。仍需另行确认早期 Mojo 故障及真实双主机交互。
