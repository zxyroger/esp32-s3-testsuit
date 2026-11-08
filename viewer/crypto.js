/* ═══════════════════════════════════════════════════════════════════════
 * 端到端加密：AES-256-GCM，用 WebCrypto 实现
 *
 * 板子在发布前把每个 payload 加密，broker 上流的是密文；浏览器订阅到之后
 * 在这里解开。附加认证数据（AAD）用 topic，密文被搬到别的话题上会校验失败。
 *
 * 线格式（与固件 main/sec_crypto.c 完全一致）：
 *   [0] 版本 0x01  [1] 保留 0x00  [2..13] 12 字节 IV  [14..] 密文‖16 字节 tag
 *
 * 注意：密钥是"共享密钥"——它就写在这个文件里，拿到网页的人就能拿到它。
 * 它挡的是公共 broker 上的陌生人偷看，而不是挡拿到分享链接的人。
 * ═══════════════════════════════════════════════════════════════════════ */
(function (root) {
  'use strict';

  /* 密钥来自 secrets.js（不进版本库）。留空时仍可用地址栏 ?k=<hex> 临时指定。 */
  const KEY_HEX = (root.LAB_SECRETS && root.LAB_SECRETS.KEY_HEX) || '';

  /* 密钥来源优先级：地址栏 ?k=<64 位 hex>（会记住）> 上次记住的 > 文件里的默认值。
   * 换密钥：板子上 `sec key <hex>` → 重启 → 用 …&k=<同一串> 打开一次页面即可。 */
  function pickKeyHex() {
    let url = '';
    try { url = (new URLSearchParams(location.search).get('k') || '').trim(); } catch (e) {}
    if (/^[0-9a-fA-F]{64}$/.test(url)) {
      try { localStorage.setItem('secKeyHex', url.toLowerCase()); } catch (e) {}
      return url.toLowerCase();
    }
    try {
      const saved = localStorage.getItem('secKeyHex');
      if (saved && /^[0-9a-f]{64}$/.test(saved)) return saved;
    } catch (e) {}
    return KEY_HEX;
  }

  const enc = new TextEncoder();
  let keyPromise = null;

  function getKey() {
    if (!keyPromise) {
      const hex = pickKeyHex();
      if (!/^[0-9a-fA-F]{64}$/.test(hex)) {
        throw new Error('未配置加密密钥：填 viewer/secrets.js 的 KEY_HEX，或用 ?k=<64 位 hex> 打开页面');
      }
      const raw = new Uint8Array(hex.match(/../g).map((h) => parseInt(h, 16)));
      keyPromise = crypto.subtle.importKey('raw', raw, 'AES-GCM', false, ['encrypt', 'decrypt']);
    }
    return keyPromise;
  }

  /** 解密一个 PUBLISH 的 payload；失败（密钥不符 / 被篡改 / 格式不对）返回 null */
  async function open(aad, buf) {
    if (!buf || buf.length < 30 || buf[0] !== 0x01) return null;
    try {
      const iv = buf.subarray(2, 14);
      const ct = buf.subarray(14);
      const pt = await crypto.subtle.decrypt(
        { name: 'AES-GCM', iv: iv, additionalData: enc.encode(aad), tagLength: 128 },
        await getKey(), ct);
      return new Uint8Array(pt);
    } catch (e) {
      return null;
    }
  }

  /** 加密成同样的线格式，返回可直接 publish 的 Uint8Array */
  async function seal(aad, bytes) {
    const iv = crypto.getRandomValues(new Uint8Array(12));
    const ct = new Uint8Array(await crypto.subtle.encrypt(
      { name: 'AES-GCM', iv: iv, additionalData: enc.encode(aad), tagLength: 128 },
      await getKey(), bytes));
    const out = new Uint8Array(2 + 12 + ct.length);
    out[0] = 0x01;
    out[1] = 0x00;
    out.set(iv, 2);
    out.set(ct, 14);
    return out;
  }

  root.Sec = { open: open, seal: seal, ready: typeof crypto !== 'undefined' && !!crypto.subtle };
})(window);
