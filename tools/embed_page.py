# -*- coding: utf-8 -*-
"""把 tools/embedded_page.html 转成 C 字符串，写回 main/cam_web.c。

改动内嵌页面时不要再手工在 C 源码里拼字符串——直接在 embedded_page.html
里改 HTML/CSS/JS，然后跑一次本脚本：

    python tools/embed_page.py           # 写入
    python tools/embed_page.py --check   # 只比对，不写（CI/提交前用）

脚本负责两件事：把每行包成 C 字符串字面量、把 \\ 和 " 正确转义。
JS 里的正则（如 /\\s*(\\d+)/）能原样保留下来。
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HTML = os.path.join(ROOT, 'tools', 'embedded_page.html')
TARGET = os.path.join(ROOT, 'main', 'cam_web.c')
BEGIN = 'static const char k_index_html[] ='


def build() -> str:
    with open(HTML, encoding='utf-8') as f:
        lines = f.read().split('\n')
    while lines and not lines[-1].strip():
        lines.pop()

    out = [BEGIN]
    for i, line in enumerate(lines):
        esc = line.replace('\\', '\\\\').replace('"', '\\"')
        out.append('"' + esc + ('\\n"' if i < len(lines) - 1 else '"'))
    out.append(';')
    return '\n'.join(out)


def main():
    block = build()
    with open(TARGET, encoding='utf-8', newline='') as f:
        src = f.read()

    start = src.find(BEGIN)
    if start < 0:
        sys.exit('main/cam_web.c 里找不到 %s' % BEGIN)
    end = src.find(';\n', start) + 1
    old = src[start:end]

    if block == old:
        print('内容一致，无需写入（%d 字节）' % len(block))
        return
    if '--check' in sys.argv:
        sys.exit('内嵌页面与 cam_web.c 不一致，请运行 python tools/embed_page.py')

    with open(TARGET, 'w', encoding='utf-8', newline='') as f:
        f.write(src[:start] + block + src[end:])
    print('已更新 main/cam_web.c：%d → %d 字节（HTML 主体 %d 字节）'
          % (len(old), len(block), os.path.getsize(HTML)))


if __name__ == '__main__':
    main()
