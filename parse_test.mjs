// 复刻 k_index_html 里的解析算法，验证能否从 multipart 流里正确切出 JPEG
const U = process.argv[2] || 'http://192.168.0.105/stream';

function scanEoh(b) {
  const n = Math.min(b.length, 512);
  for (let i = 0; i + 3 < n; i++) {
    if (b[i] === 13 && b[i + 1] === 10 && b[i + 2] === 13 && b[i + 3] === 10) return i + 4;
  }
  return -1;
}

function contentLen(b, n) {
  let s = '';
  for (let i = 0; i < n; i++) s += String.fromCharCode(b[i]);
  const m = /Content-Length:\s*(\d+)/i.exec(s);
  return m ? parseInt(m[1], 10) : -1;
}

const ctrl = new AbortController();
setTimeout(() => ctrl.abort(), 6000);

const r = await fetch(U, { cache: 'no-store', signal: ctrl.signal });
console.log('HTTP', r.status, '|', r.headers.get('content-type'));

const rd = r.body.getReader();
let buf = new Uint8Array(0);
const frames = [];
let bytes = 0;

try {
  while (true) {
    const q = await rd.read();
    if (q.done) break;
    const nb = new Uint8Array(buf.length + q.value.length);
    nb.set(buf);
    nb.set(q.value, buf.length);
    buf = nb;
    while (true) {
      const eoh = scanEoh(buf);
      if (eoh < 0) break;
      const len = contentLen(buf, eoh);
      if (len <= 0 || buf.length < eoh + len) break;
      frames.push(Buffer.from(buf.subarray(eoh, eoh + len)));
      bytes += len;
      buf = buf.subarray(eoh + len);
    }
  }
} catch (e) {
  if (e.name !== 'AbortError') throw e;
}

console.log('切出帧数', frames.length, '| 总字节', bytes);
let bad = 0;
for (const f of frames) {
  const soi = f[0] === 0xFF && f[1] === 0xD8;
  const eoi = f[f.length - 2] === 0xFF && f[f.length - 1] === 0xD9;
  if (!soi || !eoi) bad++;
}
console.log('SOI/EOI 校验：坏帧', bad, '/', frames.length);

if (frames.length) {
  const fs = await import('node:fs');
  fs.writeFileSync('D:/s3demo/node_frame.jpg', frames[frames.length - 1]);
  console.log('末帧已存 node_frame.jpg', frames[frames.length - 1].length, 'B');
}
process.exit(0);
