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

  let frameCount = 0, byteCount = 0, totalBytes = 0, t0 = performance.now(), gotFirst = false;

  /* ───────────────────────── 横纹降噪 ─────────────────────────
   * OV2640 暗光下有明显的行级噪声（row noise）：每一整行的亮度随机或固定地
   * 抖动，看起来就是水平横纹。彩条干净证明不是传输/电源问题，是传感器模拟
   * 前端固有特性。这里做两件事：
   *   1. 时域 EMA 平均 —— 压掉每帧随机的行抖动（占大头）；
   *   2. 行均值平滑归一化 —— 把每一行亮度拉回到它邻域的平滑均值，
   *      保留场景真实的明暗渐变，只消掉逐行跳变。
   * 两者都是纯前端处理，不改固件，也不动密钥/链路。 */
  let bufCanvas = document.createElement('canvas');
  const bctx = bufCanvas.getContext('2d', { willReadFrequently: true });
  const fctx = img.getContext('2d', { willReadFrequently: true });
  let acc = null;              // 时域累积的像素（Uint8ClampedArray）
  let NR_ALPHA = 0.55;         // 时域平滑系数：越大越平均、也越容易拖影
  const NR_LOCAL_K = 10;       // 局部差异阈值：超过它就认为这一像素在动/是边缘，不做时域平均
  const NR_GUARD_M0 = 4;       // 静止帧 mad≈3.8~4.9；超过它才逐渐开启局部运动保护
  const NR_GUARD_SPAN = 3;      // mad 从 M0 到 M0+SPAN 线性过渡到全力保护
  let diffBuf = null;          // 逐像素帧间差异（复用）
  let diffTmp = null;          // 横向 3 抽头平滑结果（复用）
  /* 边缘自适应锐化缓冲：时域平均 + JPEG q30 会抹软细节，只在纹理/边缘区
   * 补回一点高频，平坦天空 mask≈0，不会把横纹一起放大。 */
  const SHARP_AMOUNT = 0.30, SHARP_G0 = 2, SHARP_KG = 8;
  let sharpSrc = null, blurH = null, gradH = null, gradV = null;
  let lumBuf = null;           // 行偏移校正用的逐像素亮度缓冲（复用，避免每帧分配）
  let histBuf = new Uint16Array(256);

  /* 固定行偏置 profile（flat-field 校正）—— 分块版。
   *
   * 为什么必须分块：整幅画面的「行均值」里混着场景本身的水平结构（楼房窗户
   * 横排、楼层线）。把它当成传感器固定偏置减到整行上，会在大片平坦的天空
   * 区域凭空造出原始画面里没有的水平暗带。实测平坦区横纹：原始 2.06、
   * 整幅 profile 处理后 4.46（更糟），而分块只有 1.28。
   * 另一个证据：把画面切成 4 个竖条，各自的固定行 profile 相关系数只有
   * 0.02~0.63，甚至出现负相关 —— 横纹本来就不是「整行均匀」的。
   *
   * 做法：按列切 FFC_SEG 段，每段独立跨帧收敛自己的 R/G/B 行偏置，
   * 应用时在段中心之间线性插值，避免竖直接缝。 */
  const FFC_SEG    = 8;        // 每列分 8 段（VGA 下每段 80 px）
  const FFC_K      = 25;       // 行均值低通半径：覆盖到约 50 行周期的横纹
  const FFC_FAST_N = 20;       // 前 N 帧无条件简单平均（分块后每段像素少，需多几帧）
  const FFC_ALPHA  = 0.05;     // 收敛后的慢 EMA：稳定跟踪固定偏置的缓慢漂移
  const FFC_MAX    = 50;       // 单行偏移上限（实测 ±33~56），防把水平边缘误当横纹
  const FFC_MOTION = 12;       // 建立期之后，仅低运动帧继续慢更新
  let ffc = null;              // Float32Array(FFC_SEG*3*h)
  let ffcSum = null;           // 快速收敛期的累加和
  let ffcCount = 0;
  let rmBuf = null;            // 分块行均值 FFC_SEG*3*h
  let lpBuf = null;
  let segW = 0;
  let segS0 = null, segS1 = null, segF = null;  // 每列的插值索引与权重
  let segOfX = null, segCount = null;           // 每列所属段 / 每段像素数

  function rowNormalize(d, w, h) {
    const rm = new Float32Array(h);
    for (let y = 0; y < h; y++) {
      let s = 0, b = y * w * 4;
      for (let x = 0; x < w; x++) {
        const i = b + x * 4;
        s += 0.299 * d[i] + 0.587 * d[i + 1] + 0.114 * d[i + 2];
      }
      rm[y] = s / w;
    }
    const sm = new Float32Array(h), K = 6;
    for (let y = 0; y < h; y++) {
      let s = 0, n = 0;
      for (let j = y - K; j <= y + K; j++) {
        if (j >= 0 && j < h) { s += rm[j]; n++; }
      }
      sm[y] = s / n;
    }
    const sc = new Float32Array(h);
    for (let y = 0; y < h; y++) {
      let v = sm[y] / (rm[y] || 1);
      v = v < 0.3 ? 0.3 : (v > 3.0 ? 3.0 : v);
      // 整幅行均值里混着场景的水平结构，满强度会把它们烤成新横带；
      // 分块 profile 已经承担了主要校正，这里只保留 30% 做细纹收尾。
      sc[y] = 1 + (v - 1) * 0.3;
    }
    for (let y = 0; y < h; y++) {
      const s = sc[y], b = y * w * 4;
      for (let x = 0; x < w; x++) {
        const i = b + x * 4;
        d[i] = d[i] * s; d[i + 1] = d[i + 1] * s; d[i + 2] = d[i + 2] * s;
      }
    }
  }

  // 加性行偏移校正：暗部横纹除了乘性增益差，还叠加了加性的读出/暗电流行偏移。
  // 逐行取低分位（暗电平）估计该行的加性偏置，高通后按亮度权重只从暗像素里减掉，
  // 亮部交给上面的乘性行归一化，二者互不干扰。
  function rowOffsetSubtract(d, w, h) {
    if (!lumBuf || lumBuf.length !== w * h) lumBuf = new Uint8Array(w * h);
    const lo = new Float32Array(h);
    for (let y = 0; y < h; y++) {
      histBuf.fill(0);
      const b = y * w * 4;
      for (let x = 0; x < w; x++) {
        const i = b + x * 4;
        const L = 0.299 * d[i] + 0.587 * d[i + 1] + 0.114 * d[i + 2];
        const q = L < 0 ? 0 : (L > 255 ? 255 : L | 0);
        lumBuf[x + y * w] = q;
        histBuf[q]++;
      }
      const target = (w * 8 / 100) | 0;   // 第 8 分位作为该行暗电平
      let acc = 0, v = 0;
      for (let bin = 0; bin < 256; bin++) {
        acc += histBuf[bin];
        if (acc >= target) { v = bin; break; }
      }
      lo[y] = v;
    }
    const sm = new Float32Array(h), K = 12;
    for (let y = 0; y < h; y++) {
      let s = 0, n = 0;
      for (let j = y - K; j <= y + K; j++) {
        if (j >= 0 && j < h) { s += lo[j]; n++; }
      }
      sm[y] = s / n;
    }
    for (let y = 0; y < h; y++) {
      const off = lo[y] - sm[y];
      if (off === 0) continue;
      const b = y * w * 4;
      for (let x = 0; x < w; x++) {
        const i = b + x * 4;
        const L = lumBuf[x + y * w];
        if (L >= 90) continue;
        const wgt = 1 - L / 90;
        d[i]     -= off * wgt;
        d[i + 1] -= off * wgt;
        d[i + 2] -= off * wgt;
      }
    }
  }

  // 固定行偏移 profile：跨帧累积传感器固定的行读出偏置（flat-field 校正），
  // R/G/B 三通道各自独立。先算每行每通道均值，再做 21 行宽的低通当作「场景真
  // 亮度」，两者之差就是该行该通道的细横纹偏置；低运动帧才把它慢 EMA 进
  // profile，运动帧只应用不更新，避免把运动的水平边缘烤成永久假纹。
  function fixedRowProfile(d, w, h, mad) {
    if (TUNE.resetFfc) {
      TUNE.resetFfc = 0;
      if (ffc) { ffc.fill(0); ffcSum.fill(0); ffcCount = 0; }
    }
    const N = FFC_SEG * 3 * h;
    if (!rmBuf || rmBuf.length !== N || segW !== w / FFC_SEG) {
      rmBuf = new Float32Array(N);
      lpBuf = new Float32Array(N);
      ffc = new Float32Array(N);       // 分辨率变了，profile 重来
      ffcSum = new Float32Array(N);
      ffcCount = 0;
      segW = w / FFC_SEG;
      segS0 = new Uint8Array(w);
      segS1 = new Uint8Array(w);
      segF = new Float32Array(w);
      segOfX = new Uint8Array(w);
      for (let x = 0; x < w; x++) {
        let s = (x / segW) | 0;
        if (s > FFC_SEG - 1) s = FFC_SEG - 1;
        segOfX[x] = s;
        // 段中心之间的线性插值：t = x/segW - 0.5
        let t = x / segW - 0.5;
        if (t < 0) t = 0; else if (t > FFC_SEG - 1) t = FFC_SEG - 1;
        const s0 = Math.floor(t);
        segS0[x] = s0;
        segS1[x] = s0 + 1 < FFC_SEG ? s0 + 1 : FFC_SEG - 1;
        segF[x] = t - s0;
      }
      segCount = new Int32Array(FFC_SEG);
      for (let x = 0; x < w; x++) segCount[segOfX[x]]++;
    }

    // 1) 每段每通道的行均值（分段累加，结构不跨段传播）
    rmBuf.fill(0);
    for (let y = 0; y < h; y++) {
      const b = y * w * 4;
      for (let x = 0; x < w; x++) {
        const i = b + x * 4;
        const o = segOfX[x] * 3 * h + y;
        rmBuf[o] += d[i];
        rmBuf[o + h] += d[i + 1];
        rmBuf[o + 2 * h] += d[i + 2];
      }
    }
    for (let s = 0; s < FFC_SEG; s++) {
      const inv = 1 / (segCount[s] || 1);
      const o = s * 3 * h;
      for (let j = 0; j < 3 * h; j++) rmBuf[o + j] *= inv;
    }

    // 2) 每段自己的行均值低通（半径 TUNE.ffcK），两者之差＝该段的行偏置
    for (let s = 0; s < FFC_SEG; s++) {
      for (let c = 0; c < 3; c++) {
        const o = s * 3 * h + c * h;
        for (let y = 0; y < h; y++) {
          let sum = 0, n = 0;
          const lo = y - TUNE.ffcK > 0 ? y - TUNE.ffcK : 0;
          const hi = y + TUNE.ffcK < h - 1 ? y + TUNE.ffcK : h - 1;
          for (let j = lo; j <= hi; j++) { sum += rmBuf[o + j]; n++; }
          lpBuf[o + y] = sum / n;
        }
      }
    }

    // 3) 建立期无条件累积（室外有风也不会卡住），之后才用运动门控慢更新
    const fast = ffcCount < TUNE.ffcFast;
    if (fast || mad < TUNE.ffcMotion) {
      for (let i = 0; i < N; i++) {
        let r = rmBuf[i] - lpBuf[i];
        if (r > TUNE.ffcMax) r = TUNE.ffcMax;
        else if (r < -TUNE.ffcMax) r = -TUNE.ffcMax;
        if (fast) {
          ffcSum[i] += r;
          ffc[i] = ffcSum[i] / (ffcCount + 1);
        } else {
          ffc[i] += (r - ffc[i]) * TUNE.ffcAlpha;
        }
      }
      ffcCount++;
    }

    // 4) 应用：每段中心之间线性插值，避免竖直接缝
    if (ffcCount === 0) return;
    for (let y = 0; y < h; y++) {
      const b = y * w * 4;
      for (let x = 0; x < w; x++) {
        const i = b + x * 4;
        const a0 = segS0[x] * 3 * h + y;
        const a1 = segS1[x] * 3 * h + y;
        const f = segF[x], g = 1 - f;
        d[i]     -= ffc[a0] * g + ffc[a1] * f;
        d[i + 1] -= ffc[a0 + h] * g + ffc[a1 + h] * f;
        d[i + 2] -= ffc[a0 + 2 * h] * g + ffc[a1 + 2 * h] * f;
      }
    }
    window.__ffc = { seg: FFC_SEG, count: ffcCount, k: TUNE.ffcK };
  }

  // 暗部去饱和：暗光下色度信噪比极低，紫色/绿色噪斑很碍眼，把暗像素往灰度收缩
  function desatDark(d, w, h, thresh) {
    for (let i = 0; i < d.length; i += 4) {
      const L = 0.299 * d[i] + 0.587 * d[i + 1] + 0.114 * d[i + 2];
      const t = L < thresh ? L / thresh : 1;
      const g = (d[i] + d[i + 1] + d[i + 2]) / 3;
      d[i]     = g + (d[i]     - g) * t;
      d[i + 1] = g + (d[i + 1] - g) * t;
      d[i + 2] = g + (d[i + 2] - g) * t;
    }
  }

  /* 边缘自适应轻度锐化。
   * 时域平均和 JPEG q30 都会把细节抹软。这里取 3x3 均值当低频、原图减低频当高频，
   * 再按「局部梯度」加权补回：纹理/边缘区梯度大 -> 正常补，平坦天空梯度≈0 -> 不补，
   * 所以横纹不会被一起放大。实测静止帧锐度回到原始水平，平坦区横纹只 +0.04。 */
  function sharpenEdge(d, w, h) {
    const nPix = w * h;
    if (!sharpSrc || sharpSrc.length !== d.length) {
      sharpSrc = new Uint8ClampedArray(d.length);
      blurH = new Float32Array(d.length);
      gradH = new Float32Array(nPix);
      gradV = new Float32Array(nPix);
    }
    sharpSrc.set(d);

    // 1) 横向 3 抽头模糊 + 前向差分梯度（都从副本读，避免原地修改串扰）
    for (let y = 0; y < h; y++) {
      const row = y * w;
      for (let x = 0; x < w; x++) {
        const p = row + x, i = p * 4;
        const il = x > 0 ? i - 4 : i;
        const ir = x < w - 1 ? i + 4 : i;
        for (let c = 0; c < 3; c++) {
          blurH[i + c] = (sharpSrc[il + c] + sharpSrc[i + c] + sharpSrc[ir + c]) / 3;
        }
        const id = y < h - 1 ? i + w * 4 : i;
        const lc = 0.299 * sharpSrc[i] + 0.587 * sharpSrc[i + 1] + 0.114 * sharpSrc[i + 2];
        const lr = 0.299 * sharpSrc[ir] + 0.587 * sharpSrc[ir + 1] + 0.114 * sharpSrc[ir + 2];
        const ld = 0.299 * sharpSrc[id] + 0.587 * sharpSrc[id + 1] + 0.114 * sharpSrc[id + 2];
        gradH[p] = (Math.abs(lc - lr) + Math.abs(lc - ld)) / 2;
      }
    }
    // 2) 梯度横向 3 抽头平滑
    for (let y = 0; y < h; y++) {
      const row = y * w;
      for (let x = 0; x < w; x++) {
        const p = row + x;
        const l = x > 0 ? gradH[p - 1] : gradH[p];
        const r = x < w - 1 ? gradH[p + 1] : gradH[p];
        gradV[p] = (l + gradH[p] + r) / 3;
      }
    }
    // 3) 纵向合成 + 按 mask 补高频
    for (let y = 0; y < h; y++) {
      const row = y * w;
      const up = y > 0 ? -w : 0, dn = y < h - 1 ? w : 0;
      const bu = y > 0 ? -w * 4 : 0, bd = y < h - 1 ? w * 4 : 0;
      for (let x = 0; x < w; x++) {
        const p = row + x, i = p * 4;
        const loc = (gradV[p + up] + gradV[p] + gradV[p + dn]) / 3;
        let mask = (loc - SHARP_G0) / SHARP_KG;
        if (mask <= 0) continue;
        if (mask > 1) mask = 1;
        const am = EFF.sharp * mask;
        const av = (EFF.sharpV || 0) * mask;
        for (let c = 0; c < 3; c++) {
          /* 横向分量：只补水平方向的高频，不放大被抹平的行纹。
           * 纵向分量（av）：专门补回 vlp 纵向低通吃掉的「横边」（桌沿、墙面
           * 转角、窗框上下沿）。它是可调的，因为纵向 unsharp 在原理上会
           * 重新强调行间差异，必须实测 band4 不回升才敢开大。 */
          const blur = TUNE.sharpH
            ? blurH[i + c]
            : (blurH[i + bu + c] + blurH[i + c] + blurH[i + bd + c]) / 3;
          const bl = TUNE.sharpH
            ? (sharpSrc[i + bu + c] + sharpSrc[i + c] + sharpSrc[i + bd + c]) / 3
            : blur;
          d[i + c] = sharpSrc[i + c] + am * (sharpSrc[i + c] - blur)
                   + av * (sharpSrc[i + c] - bl);
        }
      }
    }
  }

  /* 运行时可调参数：调试时注入 window.__TUNE 即可热改，用于真实流上做
   * 消融与参数扫描。正常用户不会触碰。
   *
   * 2026-11-10 SVGA 真实流三轮交错扫描定案（见 tune_scan_result*.json）：
   *   - 旧管线（分块 FFC + 整行归一化/偏移 + 3x3 锐化 + guard 常开）
   *     在当前场景下平坦区横纹平均是「负增益」，即会制造横带；
   *   - 方向性平坦区纵向低通(vlp) 是降纹主力；EMA alpha 与守卫门限
   *     必须提高，否则时域降噪被半开的 guard 削弱；
   *   - 纵向低通只损失垂直细节，所以锐化必须只补水平方向(sharpH=1)，
   *     3x3 全向锐化会把刚抹平的行纹一起放大。
   * 最终：横纹 -70%，锐度相对原图仅 -1.5%。FFC/RN/RO 代码保留为开关，
   * 供暗光场景重新启用（暗光下固定行 FPN 更强）。 */
  const TUNE = {
    m0: 7, span: 3, alpha: 0.85,
    sharp: 1.00, sharpH: 1, sharpV: 0.35,
    ffcK: 25, ffcAlpha: 0.05, ffcFast: 20, ffcMotion: 12, ffcMax: 50,
    ema: 1, ffc: 0, rn: 0, ro: 0, sharpOn: 1,
    vlp: 1, vlpK: 4, vlpLam: 0.9, vlpG0: 2, vlpKg: 8,
    resetFfc: 0, rot: 1, auto: 1,
    guardMode: 1, locR0: 2.0, locRK: 1.5,
    flicker: 1, flickerS: 0.85, flickerL: 95,
    /* 列条带校正（column FPN）：与之前跨帧学习的 fixedColProfile 不同，
     * 这里只在当前帧内估计——行带内「列方向高通残差」的均值就是条带，
     * 逐列减掉即可。跨帧 profile 之所以失败是因为图案随时间漂移，
     * 单帧估计没有这个假设。离线（18:20~18:40 实拍）col4 0.39→0.22、
     * acut 只掉 1~3%。夜间帧率低、单帧噪声大，所以学习率/强度做钳位。 */
    colFix: 0, colClamp: 2.5,
  };
  /* 亮度自适应：跨亮度扫描（sim14_lowlight.py，数字增益 L118→L28）表明，
   * 光照下降时 AGC 抬升会让固定行 FPN 与随机噪声同时变强，白天标定参数
   * 在 L28 时降噪率会从 77% 掉到 18%。因此随亮度平滑地把
   *   纵向低通强度 0.9→1.0、时域平均 0.85→0.90、锐化 1.0→0.8
   * 缓变；锐化反向调小是为了避免暗光下把噪点一起放大（视觉 A/B 中
   * 全激进组 sharp=1.2 在暗区明显过锐）。lightEMA 做时间平滑，避免
   * 云影/单帧噪声引起参数抖动。TUNE.auto=0 时完全等价于固定参数，
   * 供 tune_scan.py 做消融扫描使用。 */
  const EFF = { alpha: 0.85, m0: 7, vlpLam: 0.9, sharp: 1.0, sharpV: 0 };
  let lightEMA = -1, autoT = 0;
  function updateAdaptive(d) {
    if (!TUNE.auto) {
      EFF.alpha = TUNE.alpha; EFF.m0 = TUNE.m0;
      EFF.vlpLam = TUNE.vlpLam; EFF.sharp = TUNE.sharp; EFF.sharpV = TUNE.sharpV || 0;
      autoT = 0;
      return;
    }
    let s = 0, n = 0;
    for (let i = 0; i < d.length; i += 16) {   // 每 4 像素抽样，够稳且几乎零开销
      s += 0.299 * d[i] + 0.587 * d[i + 1] + 0.114 * d[i + 2];
      n++;
    }
    const L = s / (n || 1);
    lightEMA = lightEMA < 0 ? L : lightEMA + (L - lightEMA) * 0.12;
    let t = (105 - lightEMA) / 70;            // L>=105 白天，L<=35 入夜
    if (t < 0) t = 0; else if (t > 1) t = 1;
    autoT = t;
    EFF.vlpLam = TUNE.vlpLam + 0.10 * t;
    EFF.alpha = TUNE.alpha + 0.05 * t;
    EFF.sharp = TUNE.sharp - 0.20 * t;
    // 纵向锐化常开：模糊来自 vlp 纵向低通与 EMA，两者白天夜里都在起作用，
    // 只按入夜程度补偿会在白天天亮时把「横边发软」留着。仅夜间略加强。
    EFF.sharpV = (TUNE.sharpV || 0) * (1 + 0.3 * t);
    EFF.m0 = TUNE.m0;
    window.__auto = { L: +lightEMA.toFixed(1), t: +t.toFixed(2),
                      eff: { alpha: +EFF.alpha.toFixed(2), lam: +EFF.vlpLam.toFixed(2),
                             sharp: +EFF.sharp.toFixed(2) } };
  }
  window.__TUNE = TUNE;
  /* 相机是侧装的，实测逆时针 90° 才是正立。转正后 600x800 竖版能铺满
   * 手机视口（横版 800x600 只能占中间一条横带），缩放比 0.615→0.82，
   * 显示混叠也更轻，因此默认转正；用户仍可点底部按钮切回。 */
  try {
    const saved = localStorage.getItem('labRot');
    TUNE.rot = saved === null ? 1 : (parseInt(saved, 10) & 3);
  } catch (e) { TUNE.rot = 1; }

  /* 诊断：同一帧上比较「降噪前 / 降噪后」的平坦区横纹与锐度。
   * 只在 URL 带 ?diag=1 时执行，正常观看零开销。指标与 cam_view_check.py
   * 完全一致（80x80 块、最平坦 10 块的高通残差 std、纹理 10 块的梯度）。 */
  const DIAG = location.search.indexOf('diag=1') >= 0;
  let diagN = 0;
  function measureFrame(d, w, h) {
    const nP = w * h, L = new Float32Array(nP);
    for (let p = 0, i = 0; p < nP; p++, i += 4) {
      L[p] = 0.299 * d[i] + 0.587 * d[i + 1] + 0.114 * d[i + 2];
    }
    const BW = 80, BH = 80, blocks = [];
    for (let by = 0; by + BH <= h; by += BH) {
      for (let bx = 0; bx + BW <= w; bx += BW) {
        let s = 0, s2 = 0;
        for (let y = by; y < by + BH; y++) {
          const row = y * w;
          for (let x = bx; x < bx + BW; x++) {
            const v = L[row + x]; s += v; s2 += v * v;
          }
        }
        const n = BW * BH, m = s / n;
        blocks.push([Math.sqrt(Math.max(0, s2 / n - m * m)), by, bx]);
      }
    }
    blocks.sort((a, b) => a[0] - b[0]);
    const hp = [];
    for (let k = 0; k < Math.min(10, blocks.length); k++) {
      const by = blocks[k][1], bx = blocks[k][2], rr = new Float32Array(BH);
      for (let y = 0; y < BH; y++) {
        let s = 0;
        for (let x = 0; x < BW; x++) s += L[(by + y) * w + bx + x];
        rr[y] = s / BW;
      }
      for (let y = 0; y < BH; y++) {
        let s = 0, c = 0;
        for (let j = y - 8; j <= y + 8; j++) if (j >= 0 && j < BH) { s += rr[j]; c++; }
        hp.push(rr[y] - s / c);
      }
    }
    let m = 0;
    for (const v of hp) m += v;
    m /= hp.length || 1;
    let vv = 0;
    for (const v of hp) vv += (v - m) * (v - m);
    const flat = Math.sqrt(vv / (hp.length || 1));
    let gs = 0, gn = 0;
    for (let k = Math.max(0, blocks.length - 10); k < blocks.length; k++) {
      const by = blocks[k][1], bx = blocks[k][2];
      let g = 0, c = 0;
      for (let y = by; y < by + BH - 1; y++) {
        for (let x = bx; x < bx + BW - 1; x++) {
          const p = y * w + x;
          g += Math.abs(L[p] - L[p + 1]) + Math.abs(L[p] - L[p + w]);
          c++;
        }
      }
      gs += g / c; gn++;
    }
    /* 入夜工频条带指标：行均值的 4 行高通残差。
     * 旧的 flat（80 块内 8 行高通）对周期 4 行的密集条带几乎不敏感，
     * 会出现「指标很干净、屏幕全是横线」的误判，必须单独盯这个数。 */
    const r4 = new Float32Array(h);
    for (let y = 0; y < h; y++) {
      let s = 0;
      for (let x = 0; x < w; x++) s += L[y * w + x];
      r4[y] = s / w;
    }
    const b4 = [];
    for (let y = 0; y < h; y++) {
      let s = 0, c = 0;
      for (let j = y - 4; j <= y + 4; j++) if (j >= 0 && j < h) { s += r4[j]; c++; }
      b4.push(r4[y] - s / c);
    }
    let m4 = 0;
    for (const v of b4) m4 += v;
    m4 /= b4.length || 1;
    let v4 = 0;
    for (const v of b4) v4 += (v - m4) * (v - m4);
    const band4 = Math.sqrt(v4 / (b4.length || 1));
    // 列条纹指标：列均值的 9 抽头高通残差（对应传感器的列 FPN / 竖纹）
    const c4 = new Float32Array(w);
    for (let x = 0; x < w; x++) {
      let s = 0;
      for (let y = 0; y < h; y++) s += L[y * w + x];
      c4[x] = s / h;
    }
    const vc = [];
    for (let x = 0; x < w; x++) {
      let s = 0, c = 0;
      for (let j = x - 4; j <= x + 4; j++) if (j >= 0 && j < w) { s += c4[j]; c++; }
      vc.push(c4[x] - s / c);
    }
    let mv = 0;
    for (const v of vc) mv += v;
    mv /= vc.length || 1;
    let vv2 = 0;
    for (const v of vc) vv2 += (v - mv) * (v - mv);
    const col4 = Math.sqrt(vv2 / (vc.length || 1));

    return { flat: +flat.toFixed(3), sharp: +(gs / (gn || 1)).toFixed(2),
             band4: +band4.toFixed(2), col4: +col4.toFixed(2) };
  }

  /* 方向性平坦区纵向低通。
   * 行纹的物理特征是「整行一致、沿 x 方向几乎不变」，而楼房细节在 x 方向
   * 有梯度。用水平梯度做掩码，只在 gx 小的像素上做跨行均值，就能抹掉行纹
   * 而保留竖直边缘。离线仿真 flat 0.588→0.440，锐度代价 <5%。 */
  let vlpSrc = null, vlpGrad = null;
  function directionalVlp(d, w, h) {
    let K = TUNE.vlpK | 0;
    if (K < 1) K = 1;
    if (K > 4) K = 4;
    const nP = w * h;
    if (!vlpSrc || vlpSrc.length !== d.length) {
      vlpSrc = new Uint8ClampedArray(d.length);
      vlpGrad = new Float32Array(nP);
    }
    vlpSrc.set(d);

    // 1) 水平梯度 gx + 纵向梯度 gy（都用 3 抽头平滑，避免单点噪声）
    for (let y = 0; y < h; y++) {
      const row = y * w;
      for (let x = 0; x < w; x++) {
        const p = row + x, i = p * 4;
        const il = x > 0 ? i - 4 : i, ir = x < w - 1 ? i + 4 : i;
        const iu = y > 0 ? i - w * 4 : i, id = y < h - 1 ? i + w * 4 : i;
        const lx = 0.299 * vlpSrc[ir] + 0.587 * vlpSrc[ir + 1] + 0.114 * vlpSrc[ir + 2];
        const ly = 0.299 * vlpSrc[id] + 0.587 * vlpSrc[id + 1] + 0.114 * vlpSrc[id + 2];
        const lc = 0.299 * vlpSrc[i] + 0.587 * vlpSrc[i + 1] + 0.114 * vlpSrc[i + 2];
        vlpGrad[p] = (Math.abs(lc - lx) + Math.abs(lc - ly)) / 2;
      }
    }
    // 2) 掩码：纹理/边缘 ->1（保留原图），平坦区 ->0（用纵向均值）
    for (let y = 0; y < h; y++) {
      const row = y * w;
      const up = y > 0 ? -w : 0, dn = y < h - 1 ? w : 0;
      for (let x = 0; x < w; x++) {
        const p = row + x, i = p * 4;
        const l = x > 0 ? vlpGrad[p - 1] : vlpGrad[p];
        const r = x < w - 1 ? vlpGrad[p + 1] : vlpGrad[p];
        const loc = (l + vlpGrad[p] + r + vlpGrad[p + up] + vlpGrad[p + dn]) / 5;
        let mask = (loc - TUNE.vlpG0) / TUNE.vlpKg;
        if (mask > 1) mask = 1; else if (mask < 0) mask = 0;
        const wgt = EFF.vlpLam * (1 - mask);
        if (wgt <= 0) continue;
        for (let c = 0; c < 3; c++) {
          let s = 0;
          for (let j = -K; j <= K; j++) {
            const yy = y + j;
            const ii = (yy < 0 ? 0 : (yy >= h ? h - 1 : yy)) * w * 4 + x * 4 + c;
            s += vlpSrc[ii];
          }
          const avg = s / (2 * K + 1);
          d[i + c] = vlpSrc[i + c] + wgt * (avg - vlpSrc[i + c]);
        }
      }
    }
  }

  /* 入夜工频条带抑制。
   * 实测（frames_dusk_1720 连续 8 帧）：路灯/室内灯 50Hz 与 rolling shutter
   * 不同步时，会形成周期 3.7~4.6 行、帧间相位随机的密集条带 ——
   * 帧间行残差相关只有 -0.02（固定 FPN 应 >0.8），每帧强度稳定但位置在变。
   * 这种条纹多帧平均只能压一半，还会把 mad 撑到 11 让运动守卫误判、
   * 反而关掉时域降噪。
   * 做法：逐帧估计行增益 g(y) = 1 - s*hp(y)/rm(y)，hp 是行均值的 4 行高通
   * 残差（周期 4 的条带在 9 点均值里接近抵消，残差最大），按行归一化后
   * 再进 EMA。离线实测 band4 9.90 → 1.08（-89%），flat8 与原图持平，
   * 不产生新的低频横带。只在暗光启用，白天关闭，避免把楼房的楼层/窗户
   * 行结构误当成条纹压掉。 */
  let flickRow = null, flickTmp = null;
  function flickerRowNorm(d, w, h) {
    if (!flickRow || flickRow.length !== h) {
      flickRow = new Float32Array(h);
      flickTmp = new Float32Array(h);
    }
    // 1) 行均值（三通道平均，只改亮度不动色度）
    for (let y = 0; y < h; y++) {
      const row = y * w * 4;
      let s = 0;
      for (let x = 0; x < w; x++) {
        const i = row + x * 4;
        s += d[i] + d[i + 1] + d[i + 2];
      }
      flickRow[y] = s / (w * 3);
    }
    // 2) 4 行高通残差
    for (let y = 0; y < h; y++) {
      let s = 0, c = 0;
      const lo = y - 4 > 0 ? y - 4 : 0;
      const hi = y + 4 < h ? y + 4 : h - 1;
      for (let j = lo; j <= hi; j++) { s += flickRow[j]; c++; }
      flickTmp[y] = flickRow[y] - s / c;
    }
    // 3) 逐行乘性校正（钳位避免纯黑行放大）
    for (let y = 0; y < h; y++) {
      const rm = flickRow[y];
      let g = 1 - TUNE.flickerS * flickTmp[y] / (rm > 1 ? rm : 1);
      if (g < 0.5) g = 0.5; else if (g > 2) g = 2;
      const row = y * w * 4;
      for (let x = 0; x < w; x++) {
        const i = row + x * 4;
        d[i] *= g; d[i + 1] *= g; d[i + 2] *= g;
      }
    }
  }

  /* 单帧列条带校正：逐行算「像素 - 左右邻均值」的残差，按列累加求均值，
   * 得到一个 w 长的偏移向量，再从整幅减掉。条带在列方向是常量、在行方向
   * 近似恒定，所以逐行残差求和能把真实纹理（行方向不成规律）平均掉。
   * 唯一风险是画面里有长竖直边缘时会残留光晕，用 colClamp 钳位限制最大
   * 修正量（夜间实测条带幅度只有 ±1.2 灰阶）。 */
  let colSum = null, colRow = null, colOff = null;
  function colOffsetFix(d, w, h) {
    if (!colSum || colSum.length !== w) {
      colSum = new Float32Array(w);
      colRow = new Float32Array(w);
      colOff = new Float32Array(w);
    }
    colSum.fill(0);
    for (let y = 0; y < h; y++) {
      const row = y * w * 4;
      for (let x = 0; x < w; x++) {
        const i = row + x * 4;
        colRow[x] = (d[i] + d[i + 1] + d[i + 2]) * 0.3333333;
      }
      for (let x = 0; x < w; x++) {
        const m = colRow[x];
        const l = x > 0 ? colRow[x - 1] : m;
        const r = x < w - 1 ? colRow[x + 1] : m;
        colSum[x] += m - (l + m + r) * 0.3333333;
      }
    }
    const cl = TUNE.colClamp || 2, inv = 1 / h;
    for (let x = 0; x < w; x++) {
      let o = colSum[x] * inv;
      if (o > cl) o = cl; else if (o < -cl) o = -cl;
      colOff[x] = o;
    }
    for (let y = 0; y < h; y++) {
      const row = y * w * 4;
      for (let x = 0; x < w; x++) {
        const i = row + x * 4, o = colOff[x];
        d[i] -= o; d[i + 1] -= o; d[i + 2] -= o;
      }
    }
  }

  /* 画面方向：相机是侧装的，实测逆时针 90° 才是楼顶朝上的正立画面。
   * 用像素级重排而不是 CSS transform —— 后者旋转后宽高比与布局对不上，
   * 会出现留白或溢出。默认 0° 保持现状，点底部按钮循环切换并记住选择。 */
  function putFrame(cur, w, h) {
    const rot = TUNE.rot & 3;
    if (rot === 0) {
      if (img.width !== w || img.height !== h) { img.width = w; img.height = h; }
      fctx.putImageData(cur, 0, 0);
      return;
    }
    const nw = (rot & 1) ? h : w, nh = (rot & 1) ? w : h;
    if (img.width !== nw || img.height !== nh) { img.width = nw; img.height = nh; }
    const out = fctx.createImageData(nw, nh);
    const s = cur.data, t = out.data;
    for (let y = 0; y < h; y++) {
      for (let x = 0; x < w; x++) {
        let nx, ny;
        if (rot === 1) { nx = y; ny = w - 1 - x; }          // 逆时针 90
        else if (rot === 3) { nx = h - 1 - y; ny = x; }      // 顺时针 90
        else { nx = w - 1 - x; ny = h - 1 - y; }              // 180
        const si = (y * w + x) * 4, di = (ny * nw + nx) * 4;
        t[di] = s[si]; t[di + 1] = s[si + 1]; t[di + 2] = s[si + 2]; t[di + 3] = 255;
      }
    }
    fctx.putImageData(out, 0, 0);
  }

  /* 与板子内嵌页面同一套单位规则，保证两边数字读法一致 */
  function fmtK(n) {
    if (n < 1024) return n + ' B';
    if (n < 1048576) return (n / 1024).toFixed(1) + ' KB';
    return (n / 1048576).toFixed(2) + ' MB';
  }

  function setState(text, kind) {
    elState.textContent = text;
    dot.className = 'dot ' + (kind || '');
  }

  async function onFrame(buf) {
    // 解码 JPEG 到离屏 canvas，做时域平均 + 行归一化后再显示
    let bmp;
    try {
      bmp = await createImageBitmap(new Blob([buf], { type: 'image/jpeg' }));
    } catch (e) {
      return;   // 解码失败（罕见），丢弃这一帧，不打断流
    }

    const w = bmp.width, h = bmp.height;
    if (bufCanvas.width !== w || bufCanvas.height !== h) {
      bufCanvas.width = w; bufCanvas.height = h;
      img.width = w; img.height = h;
      acc = null;              // 分辨率变了，累积缓冲重来
    }
    bctx.drawImage(bmp, 0, 0);
    bmp.close();

    const cur = bctx.getImageData(0, 0, w, h);
    const d = cur.data;
    const rawDiag = DIAG ? measureFrame(d, w, h) : null;
    updateAdaptive(d);
    if (TUNE.flicker && lightEMA < TUNE.flickerL) flickerRowNorm(d, w, h);
    if (TUNE.colFix) colOffsetFix(d, w, h);

    let mad = 0;
    if (TUNE.ema && acc && acc.length === d.length) {
      const nPix = w * h;
      if (!diffBuf || diffBuf.length !== nPix) {
        diffBuf = new Float32Array(nPix);
        diffTmp = new Float32Array(nPix);
      }
      // 1) 逐像素帧间差异（三通道平均绝对差）。全局平均 mad 决定整体平滑强度。
      let sum = 0;
      for (let p = 0, i = 0; p < nPix; p++, i += 4) {
        const a0 = d[i] - acc[i], a1 = d[i + 1] - acc[i + 1], a2 = d[i + 2] - acc[i + 2];
        const v = ((a0 < 0 ? -a0 : a0) + (a1 < 0 ? -a1 : a1) + (a2 < 0 ? -a2 : a2)) / 3;
        diffBuf[p] = v;
        sum += v;
      }
      mad = sum / nPix;
      // 极端运动才整体降平均强度；一般情况交给下面的局部守卫处理
      NR_ALPHA = mad > 16 ? 0.30 : EFF.alpha;

      /* 关键：静止时局部保护必须完全关闭。静止画面没有拖影，全局 EMA 是纯赚的
       * （横纹压得最干净）；只有检测到运动时才逐渐启用局部权重，保住边缘/细节。
       * 实测静止流 mad≈3.8~4.9，模拟运动 mad≈8~10。 */
      let guard = (mad - EFF.m0) / TUNE.span;
      if (guard < 0) guard = 0; else if (guard > 1) guard = 1;

      /* 2) 3x3 局部平均差异。
       * guardMode=1（默认）：不依赖全局 mad 开关，始终计算 —— 因为局部运动
       * （一个人走过、占画面几个百分点）几乎不改变全局 mad，旧逻辑会让
       * guard=0、EMA 全速平均，在运动区留下明显重影（离线实测 RMSE
       * 25.8 vs 不做平均 5.9）。改成始终启用后，静止区差异只来自噪声
       * （loc≈1~3 < locG0），权重仍≈1，横纹照压；运动区 loc 大，权重
       * 自动降到 0，不留残影。实测静止 flat 只退化 4%，拖影降 73%。 */
      if (guard > 0 || TUNE.guardMode) {
        for (let y = 0; y < h; y++) {
          const row = y * w;
          for (let x = 0; x < w; x++) {
            const p = row + x;
            const l = x > 0 ? diffBuf[p - 1] : diffBuf[p];
            const r = x < w - 1 ? diffBuf[p + 1] : diffBuf[p];
            diffTmp[p] = (l + diffBuf[p] + r) / 3;
          }
        }
      }
      // 3) 运动自适应 EMA：平坦静止区照常多帧平均压噪声（横纹最显眼的地方），
      //    运动处/边缘处权重降到 0，直接用当前帧 —— 不再把运动物体糊成重影。
      const a = NR_ALPHA;
      for (let y = 0; y < h; y++) {
        const row = y * w;
        const up = y > 0 ? -w : 0;
        const dn = y < h - 1 ? w : 0;
        for (let x = 0; x < w; x++) {
          const p = row + x, i = p * 4;
          let ae = a;
          if (TUNE.guardMode) {
            const loc = (diffTmp[p + up] + diffTmp[p] + diffTmp[p + dn]) / 3;
            /* 用「局部差异 / 全局差异」的比值而不是绝对阈值：
             * 静止时 loc≈mad（比值~0.8），权重≈1，照常强平均；
             * 局部运动时 loc>>mad（比值>3），权重→0，不留拖影；
             * 黄昏云影这类全局亮度变化，loc 与 mad 同步变大，比值仍~1，
             * 因此不会被误判成运动而关掉降噪（旧绝对阈值会，云影下
             * flat 0.475 vs 相对判据 0.429）。 */
            const rr = loc / (mad + 1.0);
            let wgt = 1 - (rr - TUNE.locR0) / TUNE.locRK;
            if (wgt < 0) wgt = 0; else if (wgt > 1) wgt = 1;
            ae = a * wgt;
          } else if (guard > 0) {
            const loc = (diffTmp[p + up] + diffTmp[p] + diffTmp[p + dn]) / 3;
            let wgt = 1 - guard * loc / NR_LOCAL_K;
            if (wgt < 0) wgt = 0;
            ae = a * wgt;
          }
          const be = 1 - ae;
          d[i]     = acc[i]     * ae + d[i]     * be;
          d[i + 1] = acc[i + 1] * ae + d[i + 1] * be;
          d[i + 2] = acc[i + 2] * ae + d[i + 2] * be;
        }
      }
      acc.set(d);
      window.__nr = { mad: +mad.toFixed(2), guard: +guard.toFixed(2), alpha: NR_ALPHA };
    } else {
      acc = new Uint8ClampedArray(d);
    }

    if (TUNE.ffc) fixedRowProfile(d, w, h, mad);
    if (TUNE.rn) rowNormalize(d, w, h);
    if (TUNE.ro) rowOffsetSubtract(d, w, h);
    desatDark(d, w, h, 80);
    if (TUNE.vlp) directionalVlp(d, w, h);
    if (TUNE.sharpOn) sharpenEdge(d, w, h);
    putFrame(cur, w, h);
    if (DIAG) {
      window.__diag = {
        n: ++diagN,
        raw: rawDiag, out: measureFrame(d, w, h),
        ffcCount: ffcCount, mad: +mad.toFixed(2), nr: window.__nr,
      };
    }

    frameCount++;
    byteCount += buf.length;
    totalBytes += buf.length;
    if (!gotFirst) {
      gotFirst = true;
      hint.classList.add('hide');
    }
    elSize.textContent = (buf.length / 1024).toFixed(1) + ' KB/帧';

    const now = performance.now();
    if (now - t0 >= 1000) {
      elFps.textContent = (frameCount * 1000 / (now - t0)).toFixed(1) + ' fps';
      elRx.textContent = fmtK(totalBytes);
      frameCount = 0; byteCount = 0; t0 = now;
    }
  }

  function onMeta(buf) {
    try {
      const m = JSON.parse(new TextDecoder().decode(buf));
      if (m && m.w) {
        /* 相机侧装，转正后宽高互换；pill 要显示用户实际看到的尺寸，
         * 否则会出现「标着 800×600、画面其实是 600×800」的矛盾。 */
        const r = TUNE.rot & 3;
        const dw = (r & 1) ? m.h : m.w, dh = (r & 1) ? m.w : m.h;
        elRes.textContent = dw + '×' + dh;
      }
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
    for (const c of obj.cases) map.set(c.id, c);

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
        const c = map.get(it[0]);
        if (!c) continue;
        it[2] = c.result;
        /* 失败原因必须跟着实测结果一起换掉。只更新状态的话，页面上会
         * 出现"状态是 FAIL、原因还是上一次基线文案"——数字对不上，比没有原因更糟。 */
        if (c.msg) it[3] = c.msg;
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
        ['audio.capture_level', '麦克风采集（42 dB→0 dB 验证经 PGA）', 'FAIL',
          '左声道信号过弱（RMS=1 < 8）：查偏置与隔直电容。麦克风无效属硬件故障，不按跳过处理'],
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
        setState('在线', 'ok');
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
        /* 中间提示只在「还没看到第一帧」时出现；一旦出过画面，断线就只靠
         * 顶部状态点（红色 + 重连中）反馈，不再弹中间那行字打断观看。 */
        if (!gotFirst) {
          hint.classList.remove('hide');
          hintT.textContent = '连接断了，正在重连';
          hintS.textContent = '页面会自动恢复，不用刷新';
        }
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

  /* 画面方向按钮：点一下逆时针 90° 转正，再点依次 180 / 顺时针 90 / 还原 */
  const rotBtn = $('rotBtn');
  const ROT_LABEL = ['0°', '⟲90°', '180°', '⟳90°'];
  function paintRot() {
    if (rotBtn) rotBtn.textContent = '方向 ' + (ROT_LABEL[TUNE.rot & 3] || '0°');
  }
  if (rotBtn) {
    rotBtn.addEventListener('click', () => {
      TUNE.rot = (TUNE.rot + 1) & 3;
      try { localStorage.setItem('labRot', String(TUNE.rot)); } catch (e) {}
      paintRot();
    });
  }
  paintRot();

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
