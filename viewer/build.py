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

# 托管要求每个静态资源请求都带 ?sharecode=，而浏览器自己拼的 URL（manifest、
# apple-touch-icon、icons[]）不会带查询串 → 会在模板里写成 ?__LAB_ASSET__，
# 这里统一注入。分享码不算密钥，链接本来就是公开的；可用 LAB_SHARECODE 覆盖。
SHARECODE = os.environ.get('LAB_SHARECODE', 'ECSHHOLNDL')
ASSET_QS = 'sharecode=' + SHARECODE


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

    tpl = tpl.replace('__LAB_ASSET__', ASSET_QS)
    if '__LAB_' in tpl:
        sys.exit('index.html 里还有未替换的 __LAB_ 占位符，检查 template.html')

    out = os.path.join(HERE, 'index.html')
    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        f.write(tpl)

    # 桌面图标：manifest 里的图标 URL 同样要带 sharecode，且必须是相对 index 的路径
    man = read('manifest.template.json').replace('__LAB_ASSET__', ASSET_QS)
    if '__LAB_' in man:
        sys.exit('manifest.template.json 里还有未替换的 __LAB_ 占位符')
    man_out = os.path.join(HERE, 'manifest.webmanifest')
    with open(man_out, 'w', encoding='utf-8', newline='\n') as f:
        f.write(man)

    print('已生成 %s  (%d 字节)' % (out, len(tpl.encode('utf-8'))))
    print('已生成 %s  (%d 字节)' % (man_out, len(man.encode('utf-8'))))


if __name__ == '__main__':
    main()
