/**
 * MqttLite —— 够用就好的 MQTT 3.1.1 客户端（WebSocket 传输）
 *
 * 为什么不引 mqtt.js：国内从 CDN 拉脚本经常卡住，而这个页面只做
 * 「连上 + 订阅几个 topic + 收 PUBLISH + 发一条 PUBLISH」，
 * 手写两百行，没有第三方依赖，断网也不会白屏。
 *
 * 浏览器里：new MqttLite({url, topics:[{topic,handler}], onState})
 * Node 里：  const {MqttLite} = require('./mqtt-lite.js')  （自动化测试用同一份）
 *
 * 兼容旧用法：传 opt.topic / opt.onFrame / opt.onMeta 时，等价于订阅这两个主题。
 */
(function (root) {
  'use strict';

  function encStr(s) {
    const b = new TextEncoder().encode(s);
    const out = new Uint8Array(2 + b.length);
    out[0] = b.length >> 8;
    out[1] = b.length & 0xff;
    out.set(b, 2);
    return out;
  }

  function encRemLen(n) {
    const out = [];
    do {
      let b = n % 128;
      n = Math.floor(n / 128);
      if (n > 0) b |= 0x80;
      out.push(b);
    } while (n > 0);
    return out;
  }

  function concat(parts) {
    let len = 0;
    for (const p of parts) len += p.length;
    const out = new Uint8Array(len);
    let o = 0;
    for (const p of parts) { out.set(p, o); o += p.length; }
    return out;
  }

  function utf8(s) { return new TextEncoder().encode(s); }

  class MqttLite {
    constructor(opt) {
      this.url = opt.url;
      this.handlers = new Map();     // topic -> handler(bytes, topic)
      this.onState = opt.onState || function () {};
      this.onConnect = opt.onConnect || function () {};

      if (opt.topic) {
        const metaTopic = opt.metaTopic || opt.topic.replace(/\/[^/]*$/, '/meta');
        if (opt.onFrame) this.handlers.set(opt.topic, opt.onFrame);
        if (opt.onMeta) this.handlers.set(metaTopic, opt.onMeta);
      }
      if (opt.topics) {
        for (const t of opt.topics) this.handlers.set(t.topic, t.handler);
      }

      this.ws = null;
      this.buf = new Uint8Array(0);
      this.connected = false;
      this.alive = false;          // 订阅成功后才算真正可用
      this.pingTimer = null;
      this.retry = 0;
      this.stopped = false;
      this.pid = 1;
    }

    /** 连接期间也能加订阅，返回 false 表示要等下次连接才生效 */
    subscribe(topic, handler) {
      this.handlers.set(topic, handler);
      if (this.connected && this.ws && this.ws.readyState === 1) {
        this._sendSubscribe([topic]);
        return true;
      }
      return false;
    }

    /** 发一条 QoS0 的 PUBLISH；连接没起来时返回 false，由调用方提示 */
    publish(topic, payload) {
      if (!this.connected || !this.ws || this.ws.readyState !== 1) return false;
      const body = typeof payload === 'string' ? utf8(payload) : payload;
      const topicBuf = encStr(topic);
      const rem = topicBuf.length + body.length;
      const pkt = concat([
        new Uint8Array([0x30]),
        new Uint8Array(encRemLen(rem)),
        topicBuf,
        body,
      ]);
      try { this.ws.send(pkt); return true; } catch (e) { return false; }
    }

    start() {
      this.stopped = false;
      this._connect();
    }

    stop() {
      this.stopped = true;
      this._clearPing();
      if (this.ws) { try { this.ws.close(); } catch (e) {} }
      this.ws = null;
    }

    _connect() {
      this.onState('connecting');
      let ws;
      try {
        // 第二个参数是子协议：EMQX 的 /mqtt 端点必须带，不带会直接握手失败
        ws = new WebSocket(this.url, 'mqtt');
      } catch (e) {
        this._scheduleRetry();
        return;
      }
      ws.binaryType = 'arraybuffer';
      this.ws = ws;

      ws.onopen = () => {
        // CONNECT：协议名 MQTT / 级别 4 / CleanSession / keepalive 60
        const cid = 'web-' + Math.random().toString(16).slice(2, 10);
        const vh = concat([
          encStr('MQTT'),
          new Uint8Array([0x04, 0x02, 0x00, 0x3c]),
          encStr(cid),
        ]);
        const pkt = concat([new Uint8Array([0x10]), new Uint8Array(encRemLen(vh.length)), vh]);
        ws.send(pkt);
        this.onState('handshake');
      };

      ws.onmessage = (ev) => {
        const data = new Uint8Array(ev.data);
        this.buf = concat([this.buf, data]);
        this._drain();
      };

      ws.onclose = () => {
        this.connected = false;
        this.alive = false;
        this._clearPing();
        if (!this.stopped) {
          this.onState('closed');
          this._scheduleRetry();
        }
      };

      ws.onerror = () => {
        if (!this.stopped) this.onState('error');
      };
    }

    _scheduleRetry() {
      this.retry = Math.min(this.retry + 1, 6);
      const wait = Math.min(1000 * Math.pow(1.6, this.retry), 8000);
      this.onState('retry', Math.round(wait));
      setTimeout(() => { if (!this.stopped) this._connect(); }, wait);
    }

    _clearPing() {
      if (this.pingTimer) { clearInterval(this.pingTimer); this.pingTimer = null; }
    }

    _sendSubscribe(topics) {
      const parts = [new Uint8Array([this.pid >> 8, this.pid & 0xff])];
      this.pid = (this.pid % 0xffff) + 1;
      for (const t of topics) {
        parts.push(encStr(t), new Uint8Array([0x00]));
      }
      const vh = concat(parts);
      const pkt = concat([new Uint8Array([0x82]), new Uint8Array(encRemLen(vh.length)), vh]);
      this.ws.send(pkt);
    }

    _subscribeAll() {
      const topics = Array.from(this.handlers.keys());
      if (topics.length) this._sendSubscribe(topics);
    }

    _drain() {
      for (;;) {
        if (this.buf.length < 2) return;
        // 解析剩余长度（变长整数）
        let mult = 1, rem = 0, i = 1, byte;
        do {
          if (i >= this.buf.length) return;
          byte = this.buf[i++];
          rem += (byte & 127) * mult;
          mult *= 128;
          if (mult > 128 * 128 * 128) return;   // 报文坏了，等重连
        } while (byte & 0x80);

        const total = i + rem;
        if (this.buf.length < total) return;

        const type = this.buf[0] >> 4;
        const body = this.buf.subarray(i, total);
        this.buf = this.buf.slice(total);

        if (type === 2) {                     // CONNACK
          if (body[1] === 0) {
            this.connected = true;
            this.retry = 0;
            this._subscribeAll();
            this._clearPing();
            this.pingTimer = setInterval(() => {
              if (this.ws && this.ws.readyState === 1) {
                this.ws.send(new Uint8Array([0xc0, 0x00]));
              }
            }, 30000);
            this.onConnect();
          } else {
            this.onState('refused', body[1]);
          }
        } else if (type === 9) {              // SUBACK
          this.alive = true;
          this.onState('subscribed');
        } else if (type === 3) {              // PUBLISH（本页只用 QoS0，没有 packet id）
          const tlen = (body[0] << 8) | body[1];
          const topic = new TextDecoder().decode(body.subarray(2, 2 + tlen));
          const payload = body.subarray(2 + tlen);
          const fn = this.handlers.get(topic);
          if (fn) {
            try { fn(payload, topic); } catch (e) {}
          }
        }
      }
    }
  }

  if (typeof module !== 'undefined' && module.exports) {
    module.exports = { MqttLite };
  } else {
    root.MqttLite = MqttLite;
  }
})(typeof self !== 'undefined' ? self : this);
