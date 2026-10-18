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
  let bridge = null;
  const api = window.clientTerminal = {
    term: term,
    zoom: function (delta) {
      term.options.fontSize = Math.max(10, Math.min(30, term.options.fontSize + delta));
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
    if (event.ctrlKey && event.shiftKey && (key === 'c' || key === 'v')) {
      event.preventDefault();
      if (event.type === 'keydown' && bridge) {
        if (key === 'c') bridge.copy();
        else if (enabled) bridge.paste();
      }
      return false;
    }
    return true;
  });
  new QWebChannel(qt.webChannelTransport, function (channel) {
    bridge = channel.objects.bridge;
    term.onData(function (text) { if (enabled) bridge.input(text); });
    term.onBinary(function (text) { if (enabled) bridge.binaryInput(btoa(text)); });
    term.onResize(function (size) { bridge.resized(size.cols, size.rows); });
    bridge.inputState.connect(function (value) {
      enabled = value;
      term.options.disableStdin = !value;
      term.options.cursorBlink = value;
    });
    bridge.output.connect(function (base64) {
      const raw = atob(base64);
      const bytes = new Uint8Array(raw.length);
      for (let index = 0; index < raw.length; ++index) bytes[index] = raw.charCodeAt(index);
      // Uint8Array 保留 xterm 的流式 UTF-8 解码状态，拆分中文不会先被 QString 替换。
      term.write(bytes, function () { bridge.acknowledged(); });
    });
    fit.fit();
    bridge.resized(term.cols, term.rows);
    bridge.loaded();
  });
})();
