#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
把 template.html + mqtt-lite.js + crypto.js + app.js 拼成单文件 index.html

为什么非要内联：发布后的静态托管要求每个资源请求都带 ?sharecode=，
而浏览器请求 <script src="mqtt-lite.js"> 时不会带上查询串，直接 403。
所以对外只能有一个 index.html，资源全部内嵌。

改完源码后跑：python build.py
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def read(name):
    with open(os.path.join(HERE, name), encoding='utf-8') as f:
        return f.read()


def main():
    tpl = read('template.html')
    lib = read('mqtt-lite.js')
    sec = read('crypto.js')
    app = read('app.js')

    # secrets.js 不进版本库：有就用真的，没有就退到 example 并提醒。
    sec_path = os.path.join(HERE, 'secrets.js')
    if os.path.exists(sec_path):
        secrets = read('secrets.js')
    else:
        secrets = read('secrets.js.example')
        print('！ 没有 viewer/secrets.js，已注入 secrets.js.example 的占位值')
        print('  页面会因为密钥为空而解不开图像：cp viewer/secrets.js.example viewer/secrets.js 后重跑')

    for mark in ('<!--@secrets-->', '<!--@mqtt-lite-->', '<!--@crypto-->', '<!--@app-->'):
        if mark not in tpl:
            sys.exit('模板里找不到占位标记 %s，检查 template.html' % mark)

    tpl = tpl.replace('<!--@secrets-->', '<script>\n' + secrets + '\n</script>')
    tpl = tpl.replace('<!--@mqtt-lite-->', '<script>\n' + lib + '\n</script>')
    tpl = tpl.replace('<!--@crypto-->', '<script>\n' + sec + '\n</script>')
    tpl = tpl.replace('<!--@app-->', '<script>\n' + app + '\n</script>')

    out = os.path.join(HERE, 'index.html')
    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        f.write(tpl)

    print('已生成 %s  (%d 字节)' % (out, len(tpl.encode('utf-8'))))


if __name__ == '__main__':
    main()
