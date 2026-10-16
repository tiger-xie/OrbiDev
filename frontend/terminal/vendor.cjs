const fs = require('fs');
const path = require('path');
// 将锁文件对应的发布资源复制入 QRC，运行及 C++ 编译均不依赖 Node 或网络。
const files = {
  'xterm/lib/xterm.js': 'xterm.js',
  'xterm/css/xterm.css': 'xterm.css',
  'xterm/LICENSE': 'xterm-LICENSE.txt',
  'xterm-addon-fit/lib/xterm-addon-fit.js': 'xterm-addon-fit.js',
  'xterm-addon-fit/LICENSE': 'fit-LICENSE.txt'
};
fs.mkdirSync(path.join(__dirname, 'vendor'), { recursive: true });
for (const [source, destination] of Object.entries(files)) {
  const input = path.join(__dirname, 'node_modules', source);
  const output = path.join(__dirname, 'vendor', destination);
  if (process.argv.includes('--check')) {
    if (!fs.readFileSync(input).equals(fs.readFileSync(output))) throw new Error('资源与锁定包不一致: ' + destination);
  } else {
    fs.copyFileSync(input, output);
  }
}
console.log('已核对/复制 ' + Object.keys(files).length + ' 个离线资源及许可文件。');
