#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""PC 侧工具脚本共用的 MQTT / 加密配置。

值不写死在这里，而是读 viewer/secrets.js —— 那是唯一一处不会进版本库的
私密配置，固件端对应 main/secrets.h。想临时覆盖就用环境变量：

    LAB_MQTT_BASE=esp32cam/xxxxxxxx  LAB_KEY_HEX=<64 hex>  python frames_watch.py

这样仓库里任何一个文件都不含真实密钥和主题前缀。
"""
import os
import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parent


def _read_secrets_js():
    """从 viewer/secrets.js 取 KEY_HEX 和 BASE；没有就退到 .example。"""
    for name in ('secrets.js', 'secrets.js.example'):
        p = ROOT / 'viewer' / name
        if p.exists():
            txt = p.read_text(encoding='utf-8')
            key = re.search(r"KEY_HEX\s*:\s*'([0-9a-fA-F]{64})'", txt)
            base = re.search(r"BASE\s*:\s*'([^']*)'", txt)
            return (key.group(1) if key else ''), (base.group(1) if base else '')
    return '', ''


KEY_HEX, BASE = _read_secrets_js()
KEY_HEX = os.environ.get('LAB_KEY_HEX', KEY_HEX).strip()
BASE = os.environ.get('LAB_MQTT_BASE', BASE).strip()

HOST = os.environ.get('LAB_MQTT_HOST', 'broker.emqx.io')
PORT = int(os.environ.get('LAB_MQTT_PORT', '8883'))


def key_bytes():
    """返回 32 字节密钥；没配置时给一条能看懂的报错。"""
    if not re.fullmatch(r'[0-9a-fA-F]{64}', KEY_HEX or ''):
        raise SystemExit(
            '没有可用的加密密钥。\n'
            '  cp viewer/secrets.js.example viewer/secrets.js 后填 KEY_HEX，\n'
            '  或者设环境变量 LAB_KEY_HEX=<64 位 hex>')
    return bytes.fromhex(KEY_HEX)
