#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""扫描 PixInsight JS 脚本里的界面英文字符串，核对 zh_cn.tsv 的覆盖情况。

用法:
    python src/tools/scan_js.py "<PI根目录>\\src\\scripts" --dict zh_cn.tsv
    python src/tools/scan_js.py "<PI根目录>\\src\\scripts" --dict zh_cn.tsv --all

输出（stdout）: 每行 `文件:行号<TAB>英文原文`，即词典未命中、需要补词条的清单。
统计信息写 stderr。

判定“已覆盖”时复现 pi_zh.dll 中 T() 的回退链：
    精确 → 去尾空白 → 去尾冒号 → 去 & → 富文本剥标签 → 真实换行转字面 \\n
所以本工具报“未命中”的条目，运行期也确实翻不出来；反之报“已覆盖”的条目
一定能在界面上显示中文。

注意:
  · 只能覆盖脚本（JS/PJSH）层的文案；C++ 进程的文案在二进制里，扫不到。
  · 运行时拼接的片段（"<b>" + 版本 + "</b>"、前缀 + 文件名）会作为噪声出现，
    属正常现象，不必逐条补——它们整串无法预知，补了也不会被匹配。
"""
import os
import re
import sys

LIT = re.compile(r'"((?:[^"\\]|\\.)*)"')

# 出现这些上下文才认为该字符串是界面文本
UI_HINT = re.compile(
    r'\.(?:text|toolTip|statusTip|whatsThis|caption|windowTitle|placeholderText)\s*='
    r'|setText\s*\(|setToolTip\s*\(|setWhatsThis\s*\(|setWindowTitle\s*\('
    r'|setLabel\w*\s*\(|setPlaceholderText\s*\(|setHeaderText\s*\('
    r'|(?:Label|Button|CheckBox|GroupBox|RadioButton|ComboBox|ToolBox|TabBox)\s*[(=]'
    r'|add(?:Label|Item|Item_\w+|Title)\s*\('
    r'|titles\s*=|new\s+Label|Column\w*\s*=|header',
    re.I)

# 明显不该进词典的：路径、URL、纯数字、识别符形态、全大写常量
SKIP = re.compile(
    r'^(?:[#./\\]|[A-Za-z]:\\|https?://|\d|%)'
    r'|^\w+\.\w+$'
    r'|^[A-Z_0-9]+$')


def load_dict(path):
    raw = open(path, 'rb').read()
    if raw[:3] == b'\xef\xbb\xbf':
        raw = raw[3:]
    keys = set()
    for line in raw.decode('utf-8').split('\r\n'):
        if not line or line.startswith('#'):
            continue
        k, sep, v = line.partition('\t')
        if sep and v.strip():
            keys.add(k)
    return keys


def _strip_tags(s):
    return re.sub(r'<[^>]*>', '', s)


def _norm_space(s):
    return re.sub(r'\s+', ' ', s).strip()


def candidates(s):
    """按 T() 的顺序，给出该串在运行期可能被拿去查表的全部形式。"""
    out = [s]
    m = s.rstrip(' \t\r\n')
    if m != s:
        out.append(m)
    c = m
    while c.endswith(':') or c.endswith('：'):
        c = c[:-1]
    c = c.rstrip(' \t')
    if c and c != m:
        out.append(c)
    for base in list(out):
        if '&' in base:
            b = base.replace('&', '')
            out.append(b)
            out.append(b[:-1].rstrip(' \t') if b.endswith(':') else b)
    for base in list(out):
        if '<' in base:
            pl = _norm_space(_strip_tags(base))
            if pl and pl != base:
                out.append(pl)
                out.append(pl.replace('< ', '<').replace('> ', '>'))
    for base in list(out):
        if '\n' in base or '\r' in base:
            out.append(base.replace('\r', '').replace('\n', '\\n'))
    return out


def covered(s, keys):
    if len(s) < 2:
        return True
    return any(k in keys for k in candidates(s))


def unescape(body):
    s = body.replace('\\"', '"').replace("\\'", "'")
    s = s.replace('\\n', '\n').replace('\\t', '\t').replace('\\r', '\r')
    s = re.sub(r'\\x([0-9A-Fa-f]{2})', lambda m: chr(int(m.group(1), 16)), s)
    s = re.sub(r'\\u([0-9A-Fa-f]{4})', lambda m: chr(int(m.group(1), 16)), s)
    return s.replace('\\\\', '\\')


def scan_file(path, keys, only_ui=True):
    miss = []
    try:
        src = open(path, 'r', encoding='utf-8', errors='replace').read()
    except OSError:
        return miss
    for i, line in enumerate(src.splitlines(), 1):
        code = line.split('//')[0]
        if not LIT.search(code):
            continue
        if only_ui and not UI_HINT.search(code):
            continue
        for m in LIT.finditer(code):
            s = unescape(m.group(1))
            if len(s) < 2 or len(s) > 2000:
                continue
            if not re.search(r'[A-Za-z]', s):
                continue
            if SKIP.search(s) or covered(s, keys):
                continue
            miss.append((i, s))
    return miss


def main():
    args = [a for a in sys.argv[1:]]
    if not args:
        print(__doc__)
        return 2
    root = args[0]
    dict_path, only_ui, total = 'zh_cn.tsv', True, 0
    for i, a in enumerate(args):
        if a == '--dict' and i + 1 < len(args):
            dict_path = args[i + 1]
        elif a == '--all':
            only_ui = False
    if not os.path.isdir(root):
        print('目录不存在: %s' % root, file=sys.stderr)
        return 2
    keys = load_dict(dict_path)
    print('词典 %s：%d 条' % (dict_path, len(keys)), file=sys.stderr)
    for dirpath, _, files in os.walk(root):
        for fn in sorted(files):
            if not fn.lower().endswith(('.js', '.jsh')):
                continue
            p = os.path.join(dirpath, fn)
            for ln, s in scan_file(p, keys, only_ui):
                rel = os.path.relpath(p, root).replace('\\', '/')
                print('%s:%d\t%s' % (rel, ln, s.replace('\t', ' ')))
                total += 1
    print('未命中合计: %d' % total, file=sys.stderr)
    return 0


if __name__ == '__main__':
    sys.exit(main())
