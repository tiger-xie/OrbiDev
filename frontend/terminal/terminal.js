'use strict';
(function () {
  const term = new Terminal({
    fontSize: 15, fontFamily: 'Consolas, "Microsoft YaHei", monospace',
    cursorBlink: true, scrollback: 3000, convertEol: false,
    theme: { background: '#181b20', foreground: '#e6e8eb', cursor: '#32b8ad', selection: '#33494b' }
  });
  const fit = new FitAddon.FitAddon();
  term.loadAddon(fit);
  term.open(document.getElementById('terminal'));
  const api = window.terminalPoc = {
    term: term, bridge: null,
    copy: function () { return term.getSelection(); },
    paste: function (text) { term.paste(text); term.focus(); },
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
  new QWebChannel(qt.webChannelTransport, function (channel) {
    api.bridge = channel.objects.bridge;
    term.onData(function (data) { api.bridge.input(data); });
    term.onResize(function (size) { api.bridge.resized(size.cols, size.rows); });
    api.bridge.output.connect(function (text) {
      term.write(text, function () { api.bridge.ackOutput(); });
    });
    fit.fit();
    api.bridge.resized(term.cols, term.rows);
    api.bridge.ready();
    term.focus();
  });
})();
