/**
 * 用 Node 跑一遍页面里那份完全相同的 MQTT 客户端逻辑，
 * 确认「订阅 + 解帧」这条链路没问题，再把页面发到公网。
 *
 * 用法：node viewer_test.js [秒数]
 */
const fs = require('fs');
const path = require('path');
const { MqttLite } = require('./viewer/mqtt-lite.js');

/* 主题前缀取 viewer/secrets.js（不进版本库），和页面、固件保持同一来源。 */
function loadBase() {
  for (const n of ['viewer/secrets.js', 'viewer/secrets.js.example']) {
    try {
      const txt = fs.readFileSync(path.join(__dirname, n), 'utf8');
      const m = txt.match(/BASE\s*:\s*'([^']*)'/);
      if (m && m[1]) return m[1];
    } catch (e) { /* 下一个 */ }
  }
  return 'esp32cam/changeme00';
}

const DUR = (process.argv[2] || 20) * 1000;
const TOPIC = loadBase() + '/jpg';

let frames = 0, bytes = 0, metas = [];
const t0 = Date.now();

const cli = new MqttLite({
  url: 'wss://broker.emqx.io:8084/mqtt',
  topic: TOPIC,
  onState: (s, extra) => console.log('[state]', s, extra === undefined ? '' : extra),
  onMeta: (m) => { metas.push(m); console.log('[meta]', JSON.stringify(m)); },
  onFrame: (buf) => {
    frames++;
    bytes += buf.length;
    if (frames <= 3 || frames % 5 === 0) {
      const soi = buf[0] === 0xff && buf[1] === 0xd8 ? 'SOI-ok' : 'SOI-bad';
      console.log(`  frame#${frames} ${buf.length}B ${soi}`);
    }
  },
});

cli.start();

setTimeout(() => {
  const sec = (Date.now() - t0) / 1000;
  console.log(`\n${sec.toFixed(1)}s 内收到 ${frames} 帧，${(bytes / 1024).toFixed(1)} KB`);
  console.log(`平均 ${(frames / sec).toFixed(2)} fps，${(bytes / frames / 1024 || 0).toFixed(1)} KB/帧`);
  console.log(`meta 收到 ${metas.length} 条`);
  cli.stop();
  process.exit(0);
}, DUR);
