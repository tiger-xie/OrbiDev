'use strict';
(function () {
  const term = new Terminal({
    fontSize: 15, fontFamily: 'Consolas, "Microsoft YaHei", monospace',
    cursorBlink: false, scrollback: 3000, convertEol: false, disableStdin: true,
    theme: { background: '#181b20', foreground: '#e6e8eb', cursor: '#32b8ad', selection: '#33494b' }
  });
  const fit = new FitAddon.FitAddon();
  term.loadAddon(fit);
  term.open(document.getElementById('terminal'));
  // 远端控制序列只能影响终端内容，不能读写剪贴板或创建链接。
  term.parser.registerOscHandler(52, function () { return true; });
  term.parser.registerOscHandler(8, function () { return true; });
  let enabled = false;
  let replaying = false;
  function updateInputState() {
    term.options.disableStdin = !enabled || replaying;
    term.options.cursorBlink = enabled && !replaying;
  }
  let bridge = null;
  const api = window.clientTerminal = {
    term: term,
    setFontSize: function (size) {
      const boundedSize = Math.max(10, Math.min(30, size));
      if (term.options.fontSize === boundedSize) return;
      term.options.fontSize = boundedSize;
      fit.fit();
    }
  };
  let fitPending = false;
  new ResizeObserver(function () {
    if (fitPending) return;
    fitPending = true;
    requestAnimationFrame(function () { fitPending = false; fit.fit(); });
  }).observe(document.getElementById('terminal'));
  term.attachCustomKeyEventHandler(function (event) {
    // Qt 合成键盘事件可能没有物理 code，逻辑 key 同样适用于系统键盘布局。
    const key = (event.key || '').toLowerCase();
    // 有选区时 Ctrl+C 复制；无选区时保留终端中断，不吞掉远端取消操作。
    const copy = key === 'c' && (event.shiftKey || term.hasSelection());
    const paste = key === 'v';
    if (event.ctrlKey && !event.altKey && (copy || paste)) {
      event.preventDefault();
      if (event.type === 'keydown' && bridge) {
        if (key === 'c') bridge.copy();
        else if (enabled && !replaying) bridge.paste();
      }
      return false;
    }
    return true;
  });
  new QWebChannel(qt.webChannelTransport, function (channel) {
    bridge = channel.objects.bridge;
    term.onData(function (text) { if (enabled && !replaying) bridge.input(text); });
    term.onBinary(function (text) { if (enabled && !replaying) bridge.binaryInput(btoa(text)); });
    term.onResize(function (size) { bridge.resized(size.cols, size.rows); });
    bridge.inputState.connect(function (value) {
      enabled = value;
      updateInputState();
    });
    bridge.output.connect(function (base64, history) {
      const raw = atob(base64);
      const bytes = new Uint8Array(raw.length);
      for (let index = 0; index < raw.length; ++index) bytes[index] = raw.charCodeAt(index);
      // Uint8Array 保留 xterm 的流式 UTF-8 解码状态，拆分中文不会先被 QString 替换。
      // 历史查询的自动回复不能再次送入活进程；回放写入期间暂时禁止输入。
      replaying = history;
      updateInputState();
      term.write(bytes, function () {
        replaying = false;
        updateInputState();
        bridge.acknowledged();
      });
    });
    fit.fit();
    bridge.resized(term.cols, term.rows);
    bridge.loaded();
  });
})();
