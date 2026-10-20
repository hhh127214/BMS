# -*- coding: utf-8 -*-
r"""守卫：.bat 里不许再出现 `set NAME=value )`（括号块内赋值带尾随空格）。

**为什么要这一条**：cmd 在 `( ... & set FAIL=1 )` 里执行赋值时，`1` 后面那个空格
**会进变量值**，`FAIL` 得到 `"1 "`；后面 `if "%FAIL%"=="1"` 恒假 ⇒ `[FAIL]` 分支
永不进入 ⇒ 脚本明明有失败项，还是印 `[OK] ... 全部通过` 并以 0 退出。

这不是理论问题：2026-10-18 全量构建里 `13/` 第一次真失败，才暴露出
`PASS=73 FAIL=10` 之后紧跟着一行 `[OK] 13\ Modbus 三层测试全部通过（813 条）`。
同形状的写法当时在 `13/`、`21/`、`P3/` 共 8 处（CHANGES §79.4）。

正确写法：`set "FAIL=1"`（引号形式，cmd 会剥掉首尾空白）。
单独占一行、`)` 在下一行的 `set FAIL=1` **不受影响**（换行不进值），不报。

用法：
    python scripts/check_bat_set_trap.py          # 只报，有问题则 exit 1
    python scripts/check_bat_set_trap.py --list   # 连"安全写法"一起列出来看
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SKIP_DIRS = {'.git', 'build', '.venv', 'code', '.workbuddy', '__pycache__', 'node_modules'}

# 同一行里既有 `set NAME=val` 又有收尾括号 —— 只有这种才有尾随空格问题
TRAP = re.compile(r'set\s+([A-Za-z_][A-Za-z0-9_]*)=([^"\r\n)]*?)\s*\)\s*$')
# 已经写对的：set "NAME=val" )
OKFORM = re.compile(r'set\s+"[A-Za-z_][A-Za-z0-9_]*=[^"]*"\s*\)\s*$')


def bat_files():
    for base, dirs, files in os.walk(ROOT):
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
        for fn in files:
            if fn.lower().endswith('.bat'):
                yield os.path.join(base, fn)


def main():
    show_all = '--list' in sys.argv
    bad = []
    good = 0
    for p in sorted(bat_files()):
        raw = open(p, 'rb').read()
        if b'\r\n' not in raw:
            bad.append((p, 0, '整个文件不是 CRLF（cmd 会吃掉行尾 CR）'))
        try:
            txt = raw.decode('utf-8')
        except UnicodeDecodeError:
            txt = raw.decode('gbk', 'replace')
        for i, line in enumerate(txt.split('\n')):
            s = line.rstrip('\r')
            if TRAP.search(s):
                bad.append((p, i + 1, s.strip()))
            elif OKFORM.search(s):
                good += 1

    if show_all:
        print('[info] 已按正确写法 `set "X=Y" )` 的位置共 %d 处' % good)

    if not bad:
        print('[OK] 没有 `set NAME=value )` 这种括号内裸赋值（会带上尾随空格）')
        return 0

    for p, ln, s in bad:
        print('[BAD] %s:%d  %s' % (os.path.relpath(p, ROOT).replace('\\', '/'), ln, s[:110]))
    print('\n[BAT TRAP] %d 处：改成 set "NAME=value"（带引号），'
          '否则后面的 if "%%NAME%%"=="value" 永远为假。见 CHANGES §79.4' % len(bad))
    return 1


sys.exit(main())
