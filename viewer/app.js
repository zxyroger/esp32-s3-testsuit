/* ═══════════════════════════════════════════════════════════════════════
 * ESP32-S3 板级测试台 —— 四个页面共用一条 MQTT 连接
 *
 *   #/cam     画面    订阅 <BASE>/jpg      + <BASE>/meta
 *   #/serial  串口    订阅 <BASE>/serial/tx，发布 <BASE>/cmd
 *   #/batt    电池    订阅 <BASE>/batt（板子每 3 s 推一条 JSON）
 *   #/tests   测试    静态结果（数据来自板子最近一次全量自检）
 *
 * 为什么几条通道都走 MQTT：板子/MQTT 是「主动外连」，不需要公网 IP、
 * 不需要端口映射，也不怕 DDNS 地址漂移。
 *
 * 串口这一段由**板子自己**处理：固件里有一条独立连接订阅 <BASE>/cmd，
 * 收到命令就在本机执行，输出发回 <BASE>/serial/tx。电脑、USB 线、
 * bridge_serial_mqtt.py 都不参与，四个页面全部只跟板子对话。
 * ═══════════════════════════════════════════════════════════════════════ */
(function () {
  'use strict';

  const BROKER_WS = 'wss://broker.emqx.io:8084/mqtt';
  /* 主题前缀来自 secrets.js（不进版本库），与固件 main/secrets.h 的
   * SECRET_MQTT_PREFIX 必须一致。 */
  const BASE      = (window.LAB_SECRETS && window.LAB_SECRETS.BASE) || 'esp32cam/changeme00';
  const T_FRAME   = BASE + '/jpg';
  const T_META    = BASE + '/meta';
  const T_TX      = BASE + '/serial/tx';          // 板子 -> 页面
  const T_CMD     = BASE + '/cmd';                // 页面 -> 板子（板子自己执行）
  const T_BATT    = BASE + '/batt';               // 电池采样 JSON
  const T_CHAL    = BASE + '/chal';               // 板子发的防重放挑战码（8 s 一换）
  const T_TEST    = BASE + '/test';               // 板子开机自检结果（每次启动自动推一次）
  /* 旧通道（电脑桥接用）已不再订阅：/serial/rx、/serial/st。
   * 现在四个页面全部只跟板子对话。 */

  const $ = (id) => document.getElementById(id);

  /* ───────────── 端到端解密：所有下行报文都是密文 ───────────── */
  let secFails = 0;

  function noteSecFail() {
    secFails++;
    if (secFails === 1) {
      const lock = $('lock');
      if (lock) {
        lock.textContent = '🔓 解密失败';
        lock.style.color = '#ff7a7a';
      }
      toast('数据解不开：页面与板子的密钥不一致');
    }
  }

  /* 把任意 handler 包成"先解密再处理"。topic 作为 AAD，
   * 报文被搬到别的话题上会直接校验失败。 */
  function secure(fn) {
    return function (buf, topic) {
      Sec.open(topic, buf).then((pt) => {
        if (!pt) { noteSecFail(); return; }
        fn(pt, topic);
      });
    };
  }

  /* ───────────────────────── 路由 ───────────────────────── */
  const PAGES = ['cam', 'serial', 'batt', 'tests'];
  let current = 'cam';

  function route() {
    const h = (location.hash || '').replace(/^#\/?/, '');
    const page = PAGES.indexOf(h) >= 0 ? h : 'cam';
    current = page;
    for (const p of PAGES) {
      $('page-' + p).classList.toggle('active', p === page);
    }
    document.querySelectorAll('#tabs a').forEach((a) => {
      a.classList.toggle('on', a.dataset.page === page);
    });
    if (page === 'serial') setTimeout(scrollTerm, 30);
    // 隐藏时 clientWidth 为 0，画出来会是空图，切回来必须重画
    if (page === 'batt') setTimeout(drawBattChart, 30);
  }
  window.addEventListener('hashchange', route);

  /* ───────────────────────── 提示条 ───────────────────────── */
  let toastTimer = null;
  function toast(msg) {
    const el = $('toast');
    el.textContent = msg;
    el.classList.add('show');
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => el.classList.remove('show'), 2200);
  }

  /* ───────────────────────── 画面 ───────────────────────── */
  const img = $('frame'), dot = $('dot'), elState = $('state'), elFps = $('fps');
  const elRes = $('res'), elSize = $('size'), elRx = $('rx');
  const hint = $('hint'), hintT = $('hintTitle'), hintS = $('hintSub');

  let objUrl = null, frameCount = 0, byteCount = 0, t0 = performance.now(), gotFirst = false;

  function setState(text, kind) {
    elState.textContent = text;
    dot.className = 'dot ' + (kind || '');
  }

  function onFrame(buf) {
    // Blob + objectURL 比 base64 快，也不会把内存撑爆（旧 URL 记得回收）
    const next = URL.createObjectURL(new Blob([buf], { type: 'image/jpeg' }));
    const prev = objUrl;
    img.src = next;
    objUrl = next;
    if (prev) setTimeout(() => URL.revokeObjectURL(prev), 1500);

    frameCount++;
    byteCount += buf.length;
    if (!gotFirst) {
      gotFirst = true;
      hint.classList.add('hide');
    }
    elSize.textContent = (buf.length / 1024).toFixed(1) + ' KB/帧';

    const now = performance.now();
    if (now - t0 >= 1000) {
      elFps.textContent = (frameCount * 1000 / (now - t0)).toFixed(1) + ' fps';
      elRx.textContent = (byteCount / 1024).toFixed(0) + ' KB';
      frameCount = 0; byteCount = 0; t0 = now;
    }
  }

  function onMeta(buf) {
    try {
      const m = JSON.parse(new TextDecoder().decode(buf));
      if (m && m.w) elRes.textContent = m.w + '×' + m.h;
    } catch (e) {}
  }

  /* ───────────────────────── 串口 ───────────────────────── */
  const term = $('term'), cmdInput = $('cmd'), sendBtn = $('send');
  const serDot = $('serDot'), serState = $('serState');
  let termLen = 0;
  let boardSeen = 0;          // 最近一次收到板子电池上报的时间（毫秒）
  let mqttOn = false;         // 浏览器的这条 MQTT 是否已连上
  let chal = '';              // 板子当前挑战码，发命令时必须带上
  const TE = new TextEncoder();

  function onChal(pt) {
    const v = new TextDecoder().decode(pt).trim();
    if (/^[0-9a-f]{16}$/.test(v)) chal = v;
  }

  /* 发一条加密命令：明文是 "c=<挑战码>|<命令>"，板子校验挑战码后再执行。
   * 这样重放的旧报文最多 16 秒后就会失效。 */
  async function publishCmd(text) {
    if (!chal) {
      toast('还没收到板子的同步码，等两秒再试');
      return false;
    }
    const sealed = await Sec.seal(T_CMD, TE.encode('c=' + chal + '|' + text));
    if (!cli.publish(T_CMD, sealed)) {
      toast('MQTT 还没连上，稍等一下');
      return false;
    }
    return true;
  }

  function scrollTerm() {
    term.scrollTop = term.scrollHeight;
  }

  function appendTerm(text) {
    if (!text) return;
    const atBottom = term.scrollHeight - term.scrollTop - term.clientHeight < 80;
    // 板子的换行是 \r\n，个别进度行只用 \r，统一成 \n 才不会出现阶梯状
    const clean = text.replace(/\r\n/g, '\n').replace(/\r/g, '\n');
    term.appendChild(document.createTextNode(clean));
    termLen += clean.length;

    // 只保留最近的内容：手机上节点太多会卡
    while (termLen > 100000 && term.firstChild) {
      termLen -= (term.firstChild.nodeValue || '').length;
      term.removeChild(term.firstChild);
    }
    if (atBottom) scrollTerm();
  }

  /* 板子的 `json` 命令会把最近一次 run 的结果整段 JSON 重发一遍，
   * 测试页就靠它把静态快照换成实时数据 —— 不用再往固件里加 HTTP 接口。 */
  let pulling = false, pullBuf = '', pullTimer = null;
  let gotTest = false;          // 是否已收到板子自动推来的自检结果

  /* 按行去重：板子执行命令时会同时打给 USB 控制台和 MQTT，若哪天又把
   * 桥接开起来，同一段输出会来两份（带 "I (1234) mqtt_cmd: " 前缀的那份）。
   * 归一化剥前缀后 1.5 秒内见过就不再显示，纯保险。 */
  const recentLine = new Map();
  let lineBuf = '', lineFlush = null;

  function normLine(s) {
    return s.replace(/^[IWEDV]\s*\(\d+\)\s*[^:]{1,24}:\s*/, '').trim();
  }

  function emitLine(line) {
    const k = normLine(line);
    if (k) {
      const now = Date.now();
      const t = recentLine.get(k);
      if (t && now - t < 1500) return;
      recentLine.set(k, now);
      if (recentLine.size > 400) {
        for (const [kk, tt] of recentLine) {
          if (now - tt > 6000) recentLine.delete(kk);
        }
      }
    }
    appendTerm(line);
  }

  function feedLines(text) {
    lineBuf += text.replace(/\r\n/g, '\n').replace(/\r/g, '\n');
    let i;
    while ((i = lineBuf.indexOf('\n')) >= 0) {
      const line = lineBuf.slice(0, i + 1);
      lineBuf = lineBuf.slice(i + 1);
      emitLine(line);
    }
    /* 半行先压着：可能下一段就补齐了。200 ms 没动静就照常显示
     * （"tf> " 这种不带换行的提示符就是这种情形）。 */
    clearTimeout(lineFlush);
    if (lineBuf) {
      lineFlush = setTimeout(() => {
        const rest = lineBuf;
        lineBuf = '';
        if (rest) emitLine(rest);
      }, 200);
    }
  }

  function onSerialTx(buf) {
    const text = new TextDecoder().decode(buf);
    feedLines(text);
    if (!pulling) return;

    pullBuf += text;
    if (pullBuf.length > 60000) pullBuf = pullBuf.slice(-40000);
    // 报文是一整行，可能被拆成好几个 TCP 段，攒够一行再解析
    const nl = pullBuf.lastIndexOf('\n');
    if (nl < 0) return;
    const lines = pullBuf.slice(0, nl).split('\n');
    pullBuf = pullBuf.slice(nl + 1);
    for (const ln of lines) {
      const idx = ln.indexOf('TFJSON');
      if (idx < 0) continue;
      /* 大括号配对取出一段完整 JSON：这样后面即使跟着桥接中继的重复内容、
       * 或者多了些日志尾巴，也不至于整段解析失败。 */
      const start = ln.indexOf('{', idx + 6);
      if (start < 0) continue;
      let depth = 0, end = -1, inStr = false, esc = false;
      for (let i = start; i < ln.length; i++) {
        const ch = ln[i];
        if (inStr) {
          if (esc) esc = false;
          else if (ch === '\\') esc = true;
          else if (ch === '"') inStr = false;
        } else if (ch === '"') {
          inStr = true;
        } else if (ch === '{') {
          depth++;
        } else if (ch === '}') {
          depth--;
          if (depth === 0) { end = i; break; }
        }
      }
      if (end < 0) continue;
      try {
        applyLiveResults(JSON.parse(ln.slice(start, end + 1)), 'manual');
        finishPull(true);
      } catch (e) {
        finishPull(false, '返回的 JSON 解析失败');
      }
      return;
    }
  }

  function finishPull(ok, msg) {
    pulling = false;
    clearTimeout(pullTimer);
    pullTimer = null;
    pullBuf = '';
    $('pullBtn').disabled = false;
    $('pullBtn').textContent = '从板子拉取最新结果';
    if (!ok) toast(msg || '没收到结果，确认板子在线');
  }

  async function pullResults(silent) {
    if (pulling) return;
    if (!(await publishCmd('json'))) return;
    pulling = true;
    pullBuf = '';
    if (!silent) {
      $('pullBtn').disabled = true;
      $('pullBtn').textContent = '读取中…';
    }
    pullTimer = setTimeout(() => {
      if (pulling) finishPull(false, '超时：确认板子在线（电池页每 3 秒有数据就说明在线）');
    }, 9000);
  }

  function applyLiveResults(obj, src) {
    if (!obj || !Array.isArray(obj.cases)) return;
    /* 只跑了一部分用例时，先把全部标成"未知"再填。
     * 否则没跑的会沿用上一次的结果，看起来像刚跑过一样。 */
    const partial = (obj.full === 0);
    if (partial) {
      for (const g of GROUPS) for (const it of g.items) it[2] = '';
    }
    const map = new Map();
    for (const c of obj.cases) map.set(c.id, c.result);

    /* 固件里新增、页面清单还没跟上的用例：按 cat 补进对应分组。
     * 不补的话这些项既不出现在页面上、也不计入统计，看起来像"少了几项"。
     * 只有全量结果才补，部分运行（run axp2101 之类）不该改动清单。 */
    if (!partial) {
      const known = new Set();
      for (const g of GROUPS) for (const it of g.items) known.add(it[0]);
      for (const c of obj.cases) {
        if (known.has(c.id)) continue;
        const cat = c.cat || c.id.split('.')[0];
        let g = GROUPS.find((x) => x.id === cat);
        if (!g) {
          g = { id: cat, desc: '', items: [] };
          GROUPS.push(g);
        }
        g.items.push([c.id, '固件新增（页面清单待补）', c.result, c.msg || '']);
        known.add(c.id);
      }
    }

    for (const g of GROUPS) {
      for (const it of g.items) {
        const r = map.get(it[0]);
        if (r) it[2] = r;
      }
    }
    if (obj.total) {
      /* 开机自检和手动拉取要说清楚是哪种，否则"开机第 23 秒"这种时间戳
       * 会让人误以为自己刚点过按钮。 */
      const when = (typeof obj.up === 'number')
        ? '板子上电第 ' + obj.up + ' 秒的自检'
        : '板子最近一次自检';
      const head = (src === 'manual') ? '实时读自板子' : when;
      const scope = partial ? '（只跑了 ' + (obj.scope || '部分用例') + '，其余未覆盖）' : '';
      runInfoText =
        head + ' · ' + obj.pass + ' 通过 / ' + obj.fail + ' 失败 / ' + obj.skip +
        ' 跳过 · ' + obj.total + ' 项 · 耗时 ' + (obj.us ? (obj.us / 1e6).toFixed(1) + ' s' : '—') +
        scope + (obj.cut ? ' · ⚠ 结果有截断' : '');
      $('runInfo').title = (src === 'manual')
        ? '由「从板子拉取最新结果」按钮触发'
        : '板子每次上电/联网后自动推送，无需操作';
    }
    renderTests();
    if (src === 'manual') toast('已更新为板子最新结果');
  }

  /* 板子每次启动（以及每次 run 之后）会把自检结果推到 <BASE>/test，
   * 带 RETAIN，所以页面一订阅就能拿到，不用等板子重发。 */
  function onTestResult(buf) {
    let obj;
    try {
      obj = JSON.parse(new TextDecoder().decode(buf));
    } catch (e) {
      return;
    }
    gotTest = true;
    applyLiveResults(obj, 'auto');
  }

  /* 串口页的在线状态只看板子：命令发到 <prefix>/cmd，由板子自己执行，
   * 电脑上的桥接已不再参与（也不再订阅 /serial/st）。板子每 3 秒上报一次
   * 电池数据，用它当心跳即可判断板子是否在网。 */
  function paintBoard() {
    const boardAge = Date.now() - boardSeen;
    if (!mqttOn) {
      serDot.className = 'dot bad';
      serState.textContent = 'MQTT 未连接';
      sendBtn.disabled = true;
    } else if (boardSeen && boardAge < 15000) {
      serDot.className = 'dot ok';
      serState.textContent = '板子直连 · 无需电脑';
      sendBtn.disabled = false;
    } else {
      serDot.className = 'dot warn';
      serState.textContent = '等板子上报…';
      sendBtn.disabled = true;
    }
  }
  setInterval(paintBoard, 2000);

  async function sendCmd(text) {
    const s = (text || '').trim();
    if (!s) return;
    if (await publishCmd(s)) pushHistory(s);
  }

  // 命令历史：手机上没方向键，长按也没用，用上下箭头按钮不合适，就留在输入框里靠 ↑↓
  const hist = [];
  let histIdx = -1;
  function pushHistory(s) {
    if (hist[hist.length - 1] !== s) hist.push(s);
    if (hist.length > 40) hist.shift();
    histIdx = hist.length;
  }

  sendBtn.addEventListener('click', () => {
    sendCmd(cmdInput.value);
    cmdInput.value = '';
  });
  cmdInput.addEventListener('keydown', (e) => {
    if (e.key === 'Enter') {
      e.preventDefault();
      sendCmd(cmdInput.value);
      cmdInput.value = '';
    } else if (e.key === 'ArrowUp') {
      if (!hist.length) return;
      e.preventDefault();
      histIdx = Math.max(0, histIdx - 1);
      cmdInput.value = hist[histIdx] || '';
    } else if (e.key === 'ArrowDown') {
      if (!hist.length) return;
      e.preventDefault();
      histIdx = Math.min(hist.length, histIdx + 1);
      cmdInput.value = hist[histIdx] || '';
    }
  });
  $('serClear').addEventListener('click', () => {
    term.textContent = '';
    termLen = 0;
  });

  const QUICK = ['cam', 'wifi ip', 'mqtt', 'list', 'run core', 'help'];
  for (const c of QUICK) {
    const b = document.createElement('button');
    b.type = 'button';
    b.textContent = c;
    b.addEventListener('click', () => sendCmd(c));
    $('chips').appendChild(b);
  }

  /* ───────────────────────── 测试结果数据 ─────────────────────────
   * 来源：板子最近一次全量自检（`run`），49 项耗时 12.8 s。
   * 每项： [用例 ID, 说明, 结果, 补充]
   * 摄像头组这次 8/8 通过 —— 模组已接上，SCCB 与出厂 ID 都读到了。
   * ──────────────────────────────────────────────────────────── */
  /* 没有板子数据时的兜底说明；一旦收到 /test 就换成实时文案。
   * 必须是变量：renderTests() 每次重绘都会把它写回 DOM，写死常量的话
   * applyLiveResults 设好的文案会被下一次重绘立刻覆盖掉（踩过）。 */
  const RUN_INFO_BASE = '最近一次全量自检 · 2026-11-03 00:20 · 49 项耗时 12.8 s';
  let runInfoText = RUN_INFO_BASE;

  const GROUPS = [
    {
      id: 'core', desc: '芯片信息 / 堆 / tick / PSRAM',
      items: [
        ['core.chip_info', '芯片型号 / 核数 / flash 容量 / MAC', 'PASS', 'ESP32-S3，16 MB flash，MAC 44:B1:76:A4:12:88'],
        ['core.heap', '堆使用情况与最大连续块', 'PASS', ''],
        ['core.timer', '系统时钟与 tick 精度', 'PASS', '阈值按 (N-1)·T 算，避免 tick 对齐造成假失败'],
        ['core.psram', 'PSRAM 容量与可用连续块', 'PASS', '8 MB，最大连续块 ≥ 1 MB（摄像头帧缓冲依赖它）'],
      ],
    },
    {
      id: 'i2c', desc: '总线引脚与器件扫描',
      items: [
        ['i2c.bus_pins', 'I2C 引脚与速率确认', 'PASS', 'SDA=GPIO1 / SCL=GPIO2 / 100 kHz'],
        ['i2c.scan', '扫描总线并列出所有应答地址', 'PASS', '0x34 AXP2101、0x18 ES8311'],
      ],
    },
    {
      id: 'axp2101', desc: '电源管理 PMIC',
      items: [
        ['axp2101.present', 'I2C 地址应答与句柄建立', 'PASS', ''],
        ['axp2101.chip_id', '读取芯片 ID 并核对型号', 'PASS', ''],
        ['axp2101.rw', '写通路验证（写回原值，不改状态）', 'PASS', ''],
        ['axp2101.adc', 'ADC 电压（VBAT/VSYS/VBUS）与温度通道', 'FAIL',
          'VBAT 读回 4969 mV，超出 [2700, 4500] 合理区间。本板只靠 USB 供电、未接电池，' +
          '这个值不是真实电池电压，分压或电量计配置需要单独确认'],
        ['axp2101.irq', '中断状态寄存器可读性', 'PASS', ''],
        ['axp2101.dump', '原始寄存器转储 0x00–0x4F', 'PASS', ''],
      ],
    },
    {
      id: 'es8311', desc: '音频 CODEC',
      items: [
        ['es8311.present', 'I2C 地址 0x18 应答与句柄建立', 'PASS', ''],
        ['es8311.chip_id', '读取芯片 ID / 版本寄存器', 'PASS', '0xFD=0x83 0xFE=0x11 0xFF=0x01'],
        ['es8311.rw', 'DAC/ADC 音量写通路（写回原值）', 'PASS', ''],
        ['es8311.power', 'REG00 CSM 上电位翻转并恢复', 'PASS', '上电默认 0x1F（掉电、从模式）'],
        ['es8311.clk', '时钟管理器寄存器可读且两次一致', 'PASS', '0x01–0x08 = 00 00 10 10 00 03 00 FF'],
        ['es8311.dump', '原始寄存器转储 0x00–0xFF', 'PASS', ''],
      ],
    },
    {
      id: 'audio', desc: 'I2S 通路与麦克风',
      items: [
        ['audio.i2s_init', 'I2S 时钟树（MCLK 4.096M / BCLK 512k）', 'PASS', '实测 MCLK 4096000 Hz、BCLK 512000 Hz'],
        ['audio.codec_cfg', 'ES8311 配成从机 16k/16bit 并回读校验', 'PASS', '16 项寄存器回读后恢复原值'],
        ['audio.pa_ctrl', '功放 NS4150B CTRL 脚（GPIO47）电平翻转', 'PASS', ''],
        ['audio.play_tone', '播放 1 kHz 正弦 200 ms（验时钟与 DMA）', 'PASS', '实测发送 161 ms，DMA 吸收 40 ms'],
        ['audio.dig_loop', '数字回环（REG44=0x68）验 ASDOUT + I2S RX', 'PASS', '左右 RMS 12020、峰值 20000'],
        ['audio.capture_level', '麦克风采集（42 dB→0 dB 验证经 PGA）', 'SKIP',
          '左声道 RMS=0，信号过弱；查 MIC 偏置与隔直电容'],
        ['audio.dump', '关键配置寄存器汇总', 'PASS', ''],
      ],
    },
    {
      id: 'lcd', desc: 'SPI 屏 ILI9342C（当前未插屏）',
      items: [
        ['lcd.bus', 'SPI2 总线与 40 MHz 时钟', 'PASS', '实测 40 MHz'],
        ['lcd.io_pins', 'DC/CS 时序与电平翻转', 'PASS', ''],
        ['lcd.init_seq', '软复位 + 22 条初始化命令', 'PASS', '重复下发堆变化 0 B'],
        ['lcd.backlight', '背光 PWM 与低电平点亮极性', 'PASS', 'duty=256/256 灭、0 亮'],
        ['lcd.fill', '全屏填充吞吐（SPI+DMA）', 'PASS', '单帧 32.7 ms ≈ 4.48 MB/s'],
        ['lcd.read_id', '读 RDDID 确认面板型号', 'SKIP', '本板 SDO 未引出，无 MISO，电学上读不到'],
        ['lcd.bars', '彩条验收（需人眼）', 'SKIP', '已刷 8 条竖彩条并点亮背光，需人看屏'],
        ['lcd.dump', '配置汇总', 'PASS', ''],
      ],
    },
    {
      id: 'camera', desc: 'OV2640 DVP（模组已接）',
      items: [
        ['camera.pmic_cfg', 'AXP2101 三路 LDO 电压设置（不使能）', 'PASS', '0x90 上电即 0x77 → 先断电再设压'],
        ['camera.pmic_on', '三路 LDO 使能与电压回读', 'PASS', 'ALDO2/BLDO1 2800 mV、BLDO2 1200 mV'],
        ['camera.xclk', 'XCLK 10 MHz（LEDC 输出）', 'PASS', '实测 10 000 000 Hz，80/8 整数分频'],
        ['camera.pwdn', 'PWDN 电平控制与读回', 'PASS', '高=掉电，低=工作'],
        ['camera.dvp_pins', 'DVP 11 个数据/同步脚输入配置', 'PASS', ''],
        ['camera.sccb', 'SCCB 0x30 应答与 bank 读回', 'PASS', '等了几轮才通：XCLK 必须由 LEDC 产生'],
        ['camera.read_id', '读 OV2640 出厂 ID', 'PASS', 'PID=0x26 VER=0x42 MID=0x7F 0xA2'],
        ['camera.dump', '配置汇总', 'PASS', ''],
      ],
    },
    {
      id: 'wifi', desc: 'STA / SoftAP 射频自检',
      items: [
        ['wifi.init', '协议栈初始化 + 进入 STA 模式', 'PASS', ''],
        ['wifi.mac', 'STA/SoftAP MAC 与 efuse 一致性', 'PASS', '…A4:12:88 / …A4:12:89'],
        ['wifi.scan', '扫描周边 AP（验证接收通路）', 'PASS', '16 个 AP，最强 −47 dBm'],
        ['wifi.ap_start', '启动 SoftAP（APSTA）并取 IP', 'PASS', 'DHCP 起在 192.168.4.1'],
        ['wifi.ap_beacon', '扫描自己验证 beacon 已广播', 'SKIP', '单射频：扫描时自家 beacon 被抢占'],
        ['wifi.ap_link', 'STA 关联自己的 AP 并 DHCP 取址', 'SKIP', '单射频自环物理上不成立，原因码 201'],
        ['wifi.ap_client', '读 AP 侧已连接客户端（手机可验收）', 'SKIP', '当前无客户端接入'],
        ['wifi.stop', '停止射频并回收资源', 'PASS', '回收约 7.7 KB'],
      ],
    },
  ];

  function renderTests() {
    let pass = 0, fail = 0, skip = 0;
    for (const g of GROUPS) {
      for (const it of g.items) {
        if (it[2] === 'PASS') pass++;
        else if (it[2] === 'FAIL') fail++;
        else skip++;
      }
    }
    const total = pass + fail + skip;

    $('runInfo').textContent = runInfoText;
    $('stats').innerHTML =
      '<div class="stat"><b>' + total + '</b><span>用例</span></div>' +
      '<div class="stat ok"><b>' + pass + '</b><span>通过</span></div>' +
      '<div class="stat bad"><b>' + fail + '</b><span>失败</span></div>' +
      '<div class="stat warn"><b>' + skip + '</b><span>跳过</span></div>';

    // 失败项单独拎出来，别让它埋在折叠的面板里
    const fails = [];
    for (const g of GROUPS) {
      for (const it of g.items) if (it[2] === 'FAIL') fails.push(it);
    }
    if (fails.length) {
      $('failBox').innerHTML = fails.map((it) =>
        '<div class="callout"><span class="idx">⚠</span><span><b>' + it[0] + '</b> —— ' +
        (it[3] || '') + '</span></div>'
      ).join('');
    } else {
      $('failBox').innerHTML = '';
    }

    const box = $('groups');
    box.innerHTML = '';
    for (const g of GROUPS) {
      let p = 0, f = 0, s = 0;
      for (const it of g.items) {
        if (it[2] === 'PASS') p++;
        else if (it[2] === 'FAIL') f++;
        else s++;
      }
      const n = g.items.length;

      const el = document.createElement('div');
      el.className = 'grp' + (f ? ' open' : '');

      const head = document.createElement('header');
      head.innerHTML =
        '<span class="chev">▶</span>' +
        '<span class="name">' + g.id + '</span>' +
        '<span class="desc">' + g.desc + '</span>' +
        '<span class="cnt">' + (f ? p + '/' + n + ' · ' + f + ' FAIL' : p + '/' + n) + '</span>';
      head.addEventListener('click', () => el.classList.toggle('open'));
      el.appendChild(head);

      const bar = document.createElement('div');
      bar.className = 'bar';
      bar.innerHTML =
        '<i class="p" style="width:' + (p / n * 100).toFixed(2) + '%"></i>' +
        '<i class="f" style="width:' + (f / n * 100).toFixed(2) + '%"></i>' +
        '<i class="s" style="width:' + (s / n * 100).toFixed(2) + '%"></i>';
      el.appendChild(bar);

      const items = document.createElement('div');
      items.className = 'items';
      for (const it of g.items) {
        const row = document.createElement('div');
        row.className = 'item';
        const note = it[3] || '';
        row.innerHTML =
          '<div class="top"><span class="id">' + it[0] + '</span>' +
          '<span class="badge ' + it[2] + '">' + it[2] + '</span></div>' +
          '<div class="desc">' + it[1] + '</div>' +
          (note ? '<div class="note">' + note + '</div>' : '');
        items.appendChild(row);
      }
      el.appendChild(items);
      box.appendChild(el);
    }

    $('footNote').innerHTML =
      '说明：<b>PASS</b> 只代表这条断言在电学上成立。SPI 屏没有 MISO、扫描不到自家 beacon 这类' +
      '物理限制，一律记 <b>SKIP</b> 而不是硬判通过 —— 报告里不该出现无法成立的结论。<br><br>' +
      '重跑：到「串口」页发 <code>run</code>（全量）或 <code>run core</code> 这样的单组。' +
      '注意 <code>run wifi</code> 会停射频、<code>run camera</code> 会给摄像头断电，' +
      '跑这两组时画面会中断几秒。<br><br>' +
      '数据由板子自己的测试框架输出，页面这里只是把最近一次结果画出来。';
  }

  /* ───────────────────────── 电池 ─────────────────────────
   * 板子每 3 秒推一条 JSON 到 <BASE>/batt，页面本地累积。
   *
   * 为什么历史存在浏览器而不是板子上：板子只有 8 MB PSRAM，还要给
   * 摄像头帧缓冲；浏览器 localStorage 白拿 5 MB，刷新页面也还在。
   * 代价是换设备/清缓存就没了，所以留了导出 CSV。
   *
   * 关于「电流」：AXP2101 的 ADC 只有 VBAT/VSYS/VBUS/TS/Die 温度五个通道，
   * 没有电流采样。硬凑一个"电流"数字就是编数据，这里只给电压曲线和
   * 它的一阶变化率（mV/min）—— 充电时为正、放电时为负，量级和方向都对得上。
   * ──────────────────────────────────────────────────────── */
  const BATT_KEY  = 's3.batt.v1';
  const BATT_MAX  = 900;                 // 3 s 一点 → 45 分钟
  const BATT_KEEP = 6 * 3600 * 1000;     // 超过 6 小时的点直接丢

  const CHG_NAME = ['涓流充电', '预充电', '恒流充电', '恒压充电', '已充满', '未充电'];

  let batt = [];           // [{t, v, s, u, c, d, p}]
  let battLast = null;     // 最近一条原始报文
  let battPaused = false;
  let battSaveT = 0;

  function battLoad() {
    try {
      const raw = localStorage.getItem(BATT_KEY);
      if (!raw) return;
      const arr = JSON.parse(raw);
      if (!Array.isArray(arr)) return;
      const cut = Date.now() - BATT_KEEP;
      batt = arr.filter((p) => p && typeof p.t === 'number' && p.t > cut);
    } catch (e) { batt = []; }
  }

  function battSave(force) {
    const now = Date.now();
    if (!force && now - battSaveT < 5000) return;
    battSaveT = now;
    try {
      localStorage.setItem(BATT_KEY, JSON.stringify(batt));
    } catch (e) {
      // 配额满了就砍一半再试，别让整个页面挂掉
      batt = batt.slice(-Math.floor(BATT_MAX / 2));
      try { localStorage.setItem(BATT_KEY, JSON.stringify(batt)); } catch (e2) {}
    }
  }

  function onBatt(buf) {
    let m;
    try { m = JSON.parse(new TextDecoder().decode(buf)); } catch (e) { return; }
    if (!m || typeof m.vbat !== 'number') return;

    battLast = m;
    boardSeen = Date.now();
    if (!battPaused) {
      batt.push({
        t: Date.now(),
        v: m.vbat | 0, s: m.vsys | 0, u: m.vbus | 0,
        c: m.chg | 0, d: m.dir | 0, p: (m.pct === undefined ? -1 : m.pct | 0),
      });
      if (batt.length > BATT_MAX) batt.splice(0, batt.length - BATT_MAX);
      battSave(false);
    }
    paintBatt();
  }

  /* 最近 3 分钟的斜率，单位 mV/min */
  function battRate() {
    if (batt.length < 2) return null;
    const now = Date.now();
    const win = batt.filter((p) => now - p.t < 180000);
    if (win.length < 2) return null;
    const a = win[0], b = win[win.length - 1];
    const min = (b.t - a.t) / 60000;
    if (min < 0.2) return null;
    return (b.v - a.v) / min;
  }

  function fmtMin(ms) {
    const m = Math.round(ms / 60000);
    if (m < 60) return m + ' 分钟';
    return (m / 60).toFixed(1) + ' 小时';
  }

  function paintBatt() {
    const m = battLast;
    if (!m) return;

    $('bVbat').textContent = m.vbat;
    $('bVsys').textContent = m.vsys + ' mV';
    $('bVbus').textContent = m.vbus + ' mV';
    $('bPct').textContent = (m.pct === undefined || m.pct < 0) ? '—' : m.pct;

    const chg = m.chg | 0, dir = m.dir | 0;
    let state, kind;
    if (chg <= 3) { state = '正在充电 · ' + CHG_NAME[chg]; kind = 'ok'; }
    else if (chg === 4) { state = '已充满（充电器已停）'; kind = 'ok'; }
    else if (dir === 2) { state = '放电中 · 电池在供系统'; kind = 'warn'; }
    else if (dir === 1) { state = '充电中'; kind = 'ok'; }
    else { state = '待机 · 未充电'; kind = ''; }

    $('bState').textContent = state;
    $('bDot').className = 'dot ' + kind;
    $('bChg').textContent = 'CHG_STAT=' + chg + ' · DIR=' + dir;

    const r = battRate();
    if (r === null) {
      $('bRate').textContent = '—';
    } else {
      const s = (r >= 0 ? '+' : '−') + Math.abs(r).toFixed(1) + ' mV/min';
      $('bRate').textContent = s;
      $('bRate').style.color = r > 0.3 ? 'var(--ok)'
                             : r < -0.3 ? 'var(--warn)' : '';
    }

    /* 单节锂电的物理范围：空 3.0 V、满 4.2 V（高压电芯 4.35 V）。
     * 超出就是读数不可信 —— 本板实测过 BAT 脚被系统 5 V 轨拉高到 4.96 V，
     * 那种状态下 PMIC 会认定"电池已满"而永远不启动充电，必须显式提示。 */
    const warn = $('bWarn');
    const bp = (m.bp === 1 || m.bp === true);
    if (m.vbat > 4400) {
      warn.className = 'bwarn show';
      warn.innerHTML = '<b>VBAT 读数 ' + m.vbat + ' mV 超出单节锂电范围（3.0–4.35 V）。</b><br>' +
        '多半是电池没真正接通（BAT 脚被系统 5 V 轨经 BATFET 拉高），或者 VBAT 采样点接到了 VSYS。' +
        '这种状态下 PMIC 会认为"电池已满 4.9 V"，充电器不会启动。';
    } else if (m.vbat < 2700 && !bp) {
      warn.className = 'bwarn show';
      warn.innerHTML = '<b>VBAT 只有 ' + m.vbat + ' mV，且状态位报告没有电池。</b><br>' +
        '当前应是从 USB 供电。要充放电测试，先把电池接好并断一次电让 PMIC 重新检测。';
    } else {
      warn.className = 'bwarn';
      warn.innerHTML = '';
    }

    if (batt.length) {
      const span = batt[batt.length - 1].t - batt[0].t;
      $('bSpan').textContent = span > 60000 ? '跨 ' + fmtMin(span) : '刚开始记录';
      $('bCount').textContent = batt.length + ' 点';
    } else {
      $('bSpan').textContent = battPaused ? '已暂停' : '—';
      $('bCount').textContent = '0 点';
    }

    if (current === 'batt') drawBattChart();
  }

  const battShow = { v: true, s: false, u: false };   // 默认只画 VBAT
  const BATT_STYLE = {
    v: { color: '#4c8dff', width: 2.0, op: 1 },
    s: { color: '#35d07f', width: 1.2, op: 0.8 },
    u: { color: '#f0b429', width: 1.2, op: 0.8 },
  };

  function drawBattChart() {
    const svg = $('bSvg');
    if (!svg) return;
    const W = Math.max(200, svg.clientWidth || 320);
    const H = Math.max(120, svg.clientHeight || 168);
    svg.setAttribute('viewBox', '0 0 ' + W + ' ' + H);

    if (batt.length < 2) {
      svg.innerHTML =
        '<text x="' + (W / 2) + '" y="' + (H / 2) + '" fill="#5d6875" font-size="12"' +
        ' text-anchor="middle">' + (battPaused ? '已暂停' : '等板子上报，3 秒一个点…') + '</text>';
      return;
    }

    // 全关时兜底画 VBAT，否则图是空的、看着像坏了
    let keys = Object.keys(BATT_STYLE).filter((k) => battShow[k]);
    if (!keys.length) keys = ['v'];

    const padL = 46, padR = 8, padT = 8, padB = 20;
    const iw = W - padL - padR, ih = H - padT - padB;

    // 只画最近 45 分钟内的点（与 BATT_MAX 对齐，避免极端情况挤在一起）
    const now = Date.now();
    let view = batt.filter((p) => now - p.t <= 46 * 60000);
    if (view.length < 2) view = batt;

    /* Y 轴只按"当前显示的曲线"算范围。
     * 三条一起算的话 VBUS 恒在 5 V，会把 3.7 V 附近几十 mV 的变化压成一条直线，
     * 等于白画。所以要能单独看 VBAT。 */
    let lo = Infinity, hi = -Infinity;
    for (const p of view) {
      for (const k of keys) {
        if (typeof p[k] === 'number') {
          lo = Math.min(lo, p[k]);
          hi = Math.max(hi, p[k]);
        }
      }
    }
    if (!isFinite(lo)) { lo = 0; hi = 100; }
    let pad = Math.max(8, (hi - lo) * 0.15);
    lo -= pad; hi += pad;
    /* 最小跨度 100 mV：电压几乎不动时（比如现在这块板读数卡在 4.96 V），
     * 若按实际 ±3 mV 抖动自适应缩放，满屏都是锯齿，反而看不出"没在变"。 */
    if (hi - lo < 100) { const mid = (hi + lo) / 2; lo = mid - 50; hi = mid + 50; }

    let t1 = view[view.length - 1].t;
    let t0 = view[0].t;
    if (t1 - t0 < 60000) { t0 = t1 - 60000; }   // 刚开始时别把 3 个点拉满全屏

    /* 把 Y 轴对齐到 1/2/5×10ⁿ 的整数刻度：不这么做会出现 4146 / 4025 这种
     * 读数，人得先做减法才知道每格多少，图上反而更难读。 */
    const niceStep = (raw) => {
      const pow = Math.pow(10, Math.floor(Math.log10(raw)));
      const n = raw / pow;
      return (n <= 1 ? 1 : n <= 2 ? 2 : n <= 5 ? 5 : 10) * pow;
    };
    let stp = niceStep((hi - lo) / 4);
    while ((hi - lo) / stp > 8) stp *= 2;
    lo = Math.floor(lo / stp) * stp;
    hi = Math.ceil(hi / stp) * stp;

    const x = (t) => padL + (t - t0) / (t1 - t0) * iw;
    const y = (v) => padT + (hi - v) / (hi - lo) * ih;

    const parts = [];

    // 横向网格 + Y 轴刻度
    for (let v = lo; v <= hi + 0.5; v += stp) {
      const yy = y(v).toFixed(1);
      parts.push('<line x1="' + padL + '" y1="' + yy + '" x2="' + (W - padR) +
                 '" y2="' + yy + '" stroke="rgba(255,255,255,.07)" stroke-width="1"/>');
      parts.push('<text x="' + (padL - 6) + '" y="' + yy + '" fill="#6b7583" font-size="10"' +
                 ' text-anchor="end" dominant-baseline="middle">' + Math.round(v) + '</text>');
    }

    // X 轴时间标签（首/中/尾）
    const fmtT = (t) => {
      const d = new Date(t);
      return String(d.getHours()).padStart(2, '0') + ':' + String(d.getMinutes()).padStart(2, '0');
    };
    [0, 0.5, 1].forEach((k) => {
      const t = t0 + (t1 - t0) * k;
      const anchor = k === 0 ? 'start' : k === 1 ? 'end' : 'middle';
      parts.push('<text x="' + x(t).toFixed(1) + '" y="' + (H - 5) + '" fill="#6b7583"' +
                 ' font-size="10" text-anchor="' + anchor + '">' + fmtT(t) + '</text>');
    });

    // 细线先画，主曲线压在上面
    const order = keys.slice().sort((a, b) => BATT_STYLE[a].width - BATT_STYLE[b].width);
    for (const k of order) {
      const st = BATT_STYLE[k];
      let d = '';
      for (let i = 0; i < view.length; i++) {
        d += (i ? 'L' : 'M') + x(view[i].t).toFixed(1) + ' ' + y(view[i][k]).toFixed(1);
      }
      parts.push('<path d="' + d + '" fill="none" stroke="' + st.color + '" stroke-width="' +
                 st.width + '" stroke-opacity="' + st.op +
                 '" stroke-linejoin="round" stroke-linecap="round"/>');
    }

    svg.innerHTML = parts.join('');
  }

  function battCsv() {
    if (!batt.length) { toast('还没有数据'); return; }
    const rows = ['time,vbat_mv,vsys_mv,vbus_mv,chg_stat,cur_dir,pct'];
    for (const p of batt) {
      rows.push([new Date(p.t).toISOString(), p.v, p.s, p.u, p.c, p.d, p.p].join(','));
    }
    const blob = new Blob(['\ufeff' + rows.join('\r\n')], { type: 'text/csv;charset=utf-8' });
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = 'batt-' + new Date().toISOString().slice(0, 16).replace(/[-:T]/g, '') + '.csv';
    document.body.appendChild(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(a.href), 2000);
    toast('已导出 ' + batt.length + ' 条');
  }

  // 回到页面时把暂停期间错过的点标出来（数据本身不会补，只能提示）
  document.addEventListener('visibilitychange', () => {
    if (!document.hidden) paintBatt();
  });

  /* 调版式用：地址加 ?demo=batt 就塞一段模拟曲线，没有板子也能看效果。
   * 真实数据一到就会被挤掉（batt 是同一个数组，按时间排序自然衔接）。 */
  function battSeedDemo(hv) {
    const now = Date.now();
    const n = 900, step = 3000;
    const start = now - n * step;
    batt = [];
    for (let i = 0; i < n; i++) {
      const k = i / n;
      if (hv) {
        // 复现本板实测到的异常：BAT 脚被系统轨拉高，读数恒在 4.96 V
        const v = 4965 + Math.sin(i / 5) * 3;
        batt.push({
          t: start + i * step, v: Math.round(v),
          s: Math.round(v + 18 + Math.sin(i / 9) * 5),
          u: 5032 + Math.round(Math.sin(i / 13) * 3),
          c: 5, d: 2, p: 100,
        });
        continue;
      }
      // 恒流段抬得快、恒压段几乎不动，最后一点点回落（充满停充的典型形状）
      const v = 3720 + 330 * Math.min(1, k * 1.25) + 40 * Math.max(0, k - 0.8) * 5
                + Math.sin(i / 7) * 1.2;
      batt.push({
        t: start + i * step,
        v: Math.round(v),
        s: Math.round(v + 130 + Math.sin(i / 11) * 4),
        u: 5032 + Math.round(Math.sin(i / 13) * 3),
        c: k < 0.02 ? 1 : k < 0.85 ? 2 : k < 0.97 ? 3 : 4,
        d: 1,
        p: Math.min(100, Math.round(20 + k * 80)),
      });
    }
    const last = batt[n - 1];
    battLast = { vbat: last.v, vsys: last.s, vbus: last.u,
                 chg: last.c, dir: last.d, pct: last.p, bp: 1 };
    paintBatt();
    drawBattChart();
  }

  /* ───────────────────────── MQTT ───────────────────────── */
  const cli = new MqttLite({
    url: BROKER_WS,
    topics: [
      { topic: T_FRAME, handler: secure(onFrame) },
      { topic: T_META, handler: secure(onMeta) },
      { topic: T_TX, handler: secure(onSerialTx) },
      { topic: T_BATT, handler: secure(onBatt) },
      { topic: T_CHAL, handler: secure(onChal) },
      { topic: T_TEST, handler: secure(onTestResult) },
    ],
    onState: (s, extra) => {
      if (s === 'connecting') setState('连接中…', 'warn');
      else if (s === 'handshake') setState('握手…', 'warn');
      else if (s === 'subscribed') {
        mqttOn = true;
        setState('已连接', 'ok');
        /* 自检结果是 RETAIN 消息，正常一订阅就来。若 3 秒还没有（broker 清过
         * 保留消息、或换过 broker），静默补拉一次，保证页面不会停在旧结果上。 */
        setTimeout(() => { if (!gotTest) pullResults(true); }, 3000);
        if (!gotFirst) {
          hintT.textContent = '已连上，等画面…';
          hintS.textContent = '板子大约每秒推 2 帧，稍等一下';
        }
        paintBoard();
      } else if (s === 'closed') {
        mqttOn = false;
        setState('重连中…', 'bad');
        hint.classList.remove('hide');
        hintT.textContent = '连接断了，正在重连';
        hintS.textContent = '页面会自动恢复，不用刷新';
        paintBoard();
      } else if (s === 'retry') {
        mqttOn = false;
        setState('重试 ' + (extra || '') + 's', 'bad');
      } else if (s === 'error') {
        mqttOn = false;
        setState('网络异常', 'bad');
      } else if (s === 'refused') {
        mqttOn = false;
        setState('被拒绝 ' + extra, 'bad');
      }
    },
  });

  /* ───────────────────────── 启动 ───────────────────────── */
  route();
  renderTests();

  /* WebCrypto 只在安全上下文（https / localhost / file）里可用。
   * 用 http 打开时所有报文都解不开，这里直接说清楚，省得以为是板子坏了。 */
  if (!Sec.ready) {
    const lock = $('lock');
    if (lock) {
      lock.textContent = '🔓 需要 HTTPS';
      lock.style.color = '#ffb020';
    }
    hint.classList.remove('hide');
    hintT.textContent = '请用 https 打开这个页面';
    hintS.textContent = '浏览器只在 https 下允许解密（WebCrypto），http 打不开画面';
  }

  battLoad();
  if (/(?:^|[?&])demo=batt/.test(location.search)) {
    battSeedDemo(/demo=batt-hv/.test(location.search));
  }
  $('bPause').addEventListener('click', () => {
    battPaused = !battPaused;
    $('bPause').textContent = battPaused ? '继续记录' : '暂停记录';
    if (battPaused) battSave(true);
    paintBatt();
    toast(battPaused ? '已暂停，实时值仍会刷新' : '继续记录');
  });
  $('bCsv').addEventListener('click', battCsv);
  document.querySelectorAll('.lg button').forEach((btn) => {
    btn.addEventListener('click', () => {
      const k = btn.dataset.k;
      battShow[k] = !battShow[k];
      btn.classList.toggle('on', battShow[k]);
      drawBattChart();
    });
  });
  $('bClear').addEventListener('click', () => {
    batt = [];
    battSave(true);
    paintBatt();
    drawBattChart();
    toast('已清空本地记录');
  });
  $('bFoot').innerHTML =
    '板子每 3 秒读一次 AXP2101 的状态寄存器（0x00 / 0x01）和三路 ADC，' +
    '经 MQTT 发到 <code>' + T_BATT + '</code>，页面本地累积并存在浏览器里（刷新还在，' +
    '最多 ' + Math.round(BATT_MAX * 3 / 60) + ' 分钟）。<br><br>' +
    '<b>关于充电电流</b>：AXP2101 的 ADC 只有 VBAT / VSYS / VBUS / TS / 芯片温度五个通道，' +
    '<b>没有电流采样</b>，所以这里给的是电压曲线和它的变化率（mV/min），不是真实电流。' +
    '要看真实电流得在电池回路里串采样电阻。<br><br>' +
    '状态位：<code>CHG_STAT</code> 0 涓流 / 1 预充 / 2 恒流 / 3 恒压 / 4 充满 / 5 未充；' +
    '<code>DIR</code> 0 待机 / 1 充电 / 2 放电。';

  $('pullBtn').addEventListener('click', pullResults);
  cli.start();

  // 回到前台时如果连接已死就重启，省得用户手动刷新
  document.addEventListener('visibilitychange', () => {
    if (!document.hidden && !cli.alive) {
      cli.stop();
      cli.start();
    }
  });

  // 画面页点一下全屏（只在画面页生效，否则点标签栏也会触发）
  $('page-cam').addEventListener('click', () => {
    if (document.fullscreenElement) {
      document.exitFullscreen();
    } else if (document.documentElement.requestFullscreen) {
      document.documentElement.requestFullscreen().catch(() => {});
    }
  });
})();
