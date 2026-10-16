'use strict';
// 自测检查真实 WebEngine 内的 xterm 缓冲与桥接，不代替操作系统输入法验收。
window.terminalPoc.runTests = async function () {
  const api = window.terminalPoc;
  const term = api.term;
  let receivedRows = 0;
  let receivedDone = false;
  let lastStatus = '';
  api.bridge.status.connect(function (message) { lastStatus = message; });
  function assert(condition, message) { if (!condition) throw new Error(message); }
  function write(text) { return new Promise(resolve => term.write(text, resolve)); }
  function contents() {
    const buffer = term.buffer.active;
    const lines = [];
    for (let i = 0; i < buffer.length; i++) lines.push(buffer.getLine(i).translateToString(true));
    return lines.join('\n');
  }
  async function until(predicate, name, timeoutMs = 20000) {
    const start = Date.now();
    while (!predicate()) {
      if (Date.now() - start > timeoutMs) throw new Error('超时: ' + name);
      await new Promise(resolve => setTimeout(resolve, 30));
    }
  }
  function key(name, code, ctrl) {
    term.textarea.dispatchEvent(new KeyboardEvent('keydown', {
      key: name, keyCode: code, which: code, ctrlKey: !!ctrl, bubbles: true, cancelable: true
    }));
  }
  try {
    await write('\x1b[2J\x1b[H\x1b[32mGREEN\x1b[0m 中文\r\n');
    const line = term.buffer.active.getLine(term.buffer.active.baseY);
    assert(line.translateToString(true).indexOf('GREEN 中文') === 0, 'ANSI/中文缓冲内容');
    assert(line.getCell(0).getFgColor() === 2, 'ANSI 绿色单元格属性');
    await write('\x1b[2J\x1b[H重绘成功');
    assert(contents().indexOf('重绘成功') >= 0 && contents().indexOf('GREEN') < 0, '全屏清除重绘');
    // 同一字符跨两个 UTF-8 字节块，验证流式解码而非整字符串碰巧成功。
    await write('\r\n');
    await write(new Uint8Array([0xe4, 0xb8]));
    await write(new Uint8Array([0xad]));
    assert(contents().indexOf('中') >= 0, 'UTF-8 分块');
    const cols = term.cols;
    api.zoom(3);
    assert(term.cols < cols, 'FitAddon 缩放列数');
    await until(() => lastStatus.indexOf(term.cols + ' 列 × ' + term.rows + ' 行') >= 0, '缩放尺寸桥接');
    api.zoom(-3);
    assert(term.cols === cols, 'FitAddon 恢复列数');
    const container = document.getElementById('terminal');
    container.style.right = '210px';
    await until(() => term.cols < cols, '窗口容器 resize');
    await until(() => lastStatus.indexOf(term.cols + ' 列 × ' + term.rows + ' 行') >= 0, '容器尺寸桥接');
    container.style.right = '10px';
    await until(() => term.cols === cols, '容器尺寸恢复');
    api.paste('echo 中文桥接');
    key('Enter', 13);
    await until(() => contents().indexOf('[ECHO]中文桥接') >= 0, '输入输出桥接');
    key('c', 67, true);
    await until(() => contents().indexOf('[CTRL_C]') >= 0, 'Ctrl+C');
    key('ArrowUp', 38);
    await until(() => contents().indexOf('[UP]') >= 0, '方向键');
    term.selectAll();
    assert(api.copy().indexOf('中文桥接') >= 0, '选择复制文本');
    term.clearSelection();
    let nextRow = 0;
    let sequenceValid = true;
    const countRows = function (text) {
      if (text.indexOf('[FLOOD_DONE]') >= 0) receivedDone = true;
      const pattern = /\[ROW (\d+)\]/g;
      let match;
      while ((match = pattern.exec(text)) !== null) {
        if (Number(match[1]) !== nextRow) sequenceValid = false;
        nextRow++;
        receivedRows = nextRow;
      }
    };
    api.bridge.output.connect(countRows);
    api.paste('flood');
    key('Enter', 13);
    // Qt Debug 渲染明显慢于 Release；仍核对所有行，超时只防止测试永久挂起。
    await until(() => contents().indexOf('[FLOOD_DONE]') >= 0, '高吞吐排空', 60000);
    api.bridge.output.disconnect(countRows);
    assert(sequenceValid && nextRow === 24000, '24000 行按序、无缺失或重复');
    assert(contents().indexOf('[ROW 23999]') >= 0, '末行已写入终端');
    api.paste('echo 输出后仍响应');
    key('Enter', 13);
    await until(() => contents().indexOf('[ECHO]输出后仍响应') >= 0, '压力后响应');
    api.bridge.report(true, 'ANSI、中文、UTF-8 分块、重绘、缩放、桥接、快捷键、复制文本、高吞吐通过；' + navigator.userAgent);
  } catch (error) {
    api.bridge.report(false, String(error) + '; rows=' + receivedRows + '; done=' + receivedDone + '; tail=' + contents().slice(-250));
  }
};
