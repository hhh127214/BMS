#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
bat 脚本行尾 / 编码修复器（BMS 项目专用）

背景
----
本项目所有构建入口都是 .bat，且注释里大量使用中文。在中文 Windows（CP936
控制台）上，UTF-8 编码的 .bat 有两个**静默失败**的坑 —— 两者都不会让
`build_all.bat` 报错退出（退出码仍是 0），只会让某个子步骤少编一个目标：

1. **LF 行尾**：`cmd.exe` 按 CR 定位行边界。纯 LF 的文件里，`rem`/`echo`
   前缀会被吃掉，整行中文被当命令执行 → `'xxx' 不是内部或外部命令`。

2. **吞 CR**：UTF-8 被 CP936 读取时，若某行末尾正好落在双字节字符的**前导
   字节**（0x81–0xFE）上，该字节会与行尾的 \\r 配成一对被吃掉 → 下一行的
   `rem` / `echo` 前缀被吞。实测：奇数字节的中文标题行 + 下一行中文 REM
   就会触发。

3. **混进控制字符**（本项**只报不自动改**，见下）：脚本是用 python 批量改写
   生成的时候，`"..\\vendor\\..."` 这类字符串里的 `\\v` / `\\f` / `\\b` 会被
   python 当成转义序列，写出一个 **VT(0x0B) / FF(0x0C) / BS(0x08)** 而不是
   字母。命令照样能跑（`gcc -I` 指向不存在的目录只发一条警告就继续），
   所以**错得毫无声响** —— P3/scripts/build.bat 上真的踩过这一脚。

修法
----
- 统一转 CRLF；
- 逐行做 CP936 扫描模拟，行尾落在前导字节上的行，在行尾补一个空格
  （纯 ASCII 行不受影响，行尾是 `^` 的行也不会被补 —— `^` 是 ASCII）；
- 控制字符**只报不改**：它几乎必然是损坏，但到底该是哪个字符只有人知道
  （`\\v`→`v`？还是整段该删？），自动改是把事故改成另一种事故。
- 幂等：已是 CRLF 且无问题的文件不会被改写。

用法
----
    python fix_bat_encoding.py            # 修仓库里所有 *.bat（跳过 code/）
    python fix_bat_encoding.py --check    # 只检查，不写（CI 用；有问题退出码 1）
    python fix_bat_encoding.py <path>...  # 只修指定文件/目录
"""

import argparse
import pathlib
import sys

# code/ 是 Python 并行实现，不参与重构（见项目约定）
SKIP_DIRS = {"code", ".git", "build", ".workbuddy"}

# 允许出现在正常 .bat 里的控制字符：TAB。
# 其余 < 0x20 的都视为可疑（\\r\\n 在分行前已经处理掉）。
CTRL_NAMES = {
    0x08: "BS", 0x0B: "VT", 0x0C: "FF", 0x0E: "SO", 0x0F: "SI",
    0x1A: "SUB", 0x1B: "ESC", 0x7F: "DEL",
}


def find_ctrl_chars(raw: bytes):
    """返回 [(行号(1-based), 字节值, 上下文)]。TAB 与 CR/LF 不算。"""
    hits = []
    # 先归一化行尾再分行，否则每行末尾的 \r 会被自己判成控制字符（229 处假阳性）
    for lineno, line in enumerate(raw.replace(b"\r\n", b"\n").split(b"\n"), start=1):
        for col, c in enumerate(line):
            if c < 0x20 and c != 0x09:
                ctx = line[max(0, col - 24):col + 24]
                hits.append((lineno, c, ctx))
    return hits


def scans_clean(line: bytes) -> bool:
    """模拟 CP936 双字节扫描：返回 True 表示行尾不落在前导字节上。"""
    i, n = 0, len(line)
    while i < n:
        c = line[i]
        if c < 0x80 or c == 0x80 or c > 0xFE:
            i += 1
        else:
            i += 2
    return i == n


def fix_bytes(raw: bytes) -> bytes:
    text = raw.replace(b"\r\n", b"\n")
    out = []
    for line in text.split(b"\n"):
        line = line.replace(b"\r", b"")          # 清掉孤立 CR
        if line and not scans_clean(line):
            line = line + b" "                   # 行尾补位，避免吞掉下一行前缀
        out.append(line)
    return b"\r\n".join(out)


def iter_bats(targets):
    for t in targets:
        p = pathlib.Path(t)
        if p.is_file():
            yield p
        elif p.is_dir():
            for f in sorted(p.rglob("*.bat")):
                if any(part in SKIP_DIRS for part in f.parts):
                    continue
                yield f


def main() -> int:
    here = pathlib.Path(__file__).resolve().parent.parent
    ap = argparse.ArgumentParser()
    ap.add_argument("paths", nargs="*", help="默认 = 仓库根目录")
    ap.add_argument("--check", action="store_true", help="只检查不写")
    args = ap.parse_args()

    targets = args.paths or [str(here)]
    bad = 0
    fixed = 0
    suspicious = 0
    for f in iter_bats(targets):
        raw = f.read_bytes()

        # ① 控制字符：只报不改（见文件头说明）
        for lineno, c, ctx in find_ctrl_chars(raw):
            suspicious += 1
            name = CTRL_NAMES.get(c, f"0x{c:02X}")
            print(f"[CTRL CHAR] {f}:{lineno}  {name} (0x{c:02X})  ...{ctx!r}...")

        # ② 行尾 / 编码：可自动修
        new = fix_bytes(raw)
        if new == raw:
            continue
        bad += 1
        if args.check:
            print(f"[NEED FIX] {f}")
        else:
            f.write_bytes(new)
            print(f"[FIXED]    {f}")
            fixed += 1

    if args.check:
        msg = f"共 {bad} 个 .bat 需要修复"
        if suspicious:
            msg += f"；另有 {suspicious} 处控制字符需人工确认"
        print(msg + ("" if (bad or suspicious) else " —— 全部合规"))
        return 1 if (bad or suspicious) else 0
    print(f"共修复 {fixed} 个 .bat")
    if suspicious:
        print(f"[WARN] 有 {suspicious} 处控制字符，必须人工确认（工具不敢猜该是什么字母）")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
